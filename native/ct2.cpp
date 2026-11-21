#include "ct2.hpp"
#include "dictionary.hpp"
#include "features.hpp"
#include "tokenizer.hpp"
#include <ctranslate2/models/whisper.h>
#include <ctranslate2/devices.h>
#include "vendor/whisper/whisper.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace localstt {
namespace ct=ctranslate2;
namespace {
template<class T>T proc(HMODULE m,const char* n){auto f=GetProcAddress(m,n);if(!f)throw std::runtime_error(std::string("Missing VAD symbol: ")+n);return reinterpret_cast<T>(f);}
struct Vad {
    HMODULE dll=nullptr;whisper_vad_context* ctx=nullptr;
    decltype(&whisper_vad_free) free_fn=nullptr;
    decltype(&whisper_vad_segments_from_samples) segments_fn=nullptr;
    decltype(&whisper_vad_segments_n_segments) count_fn=nullptr;
    decltype(&whisper_vad_segments_get_segment_t0) start_fn=nullptr;
    decltype(&whisper_vad_segments_get_segment_t1) end_fn=nullptr;
    decltype(&whisper_vad_free_segments) free_segments=nullptr;
    whisper_vad_params params{};
    ~Vad(){if(ctx&&free_fn)free_fn(ctx);if(dll)FreeLibrary(dll);}
    void load(const fs::path& path){
        if(ctx)return;dll=LoadLibraryExW((executable_dir()/L"whisper.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!dll)throw std::runtime_error("Cannot load native VAD runtime");
        auto defaults=proc<decltype(&whisper_vad_default_context_params)>(dll,"whisper_vad_default_context_params")();defaults.use_gpu=false;defaults.n_threads=2;
        free_fn=proc<decltype(free_fn)>(dll,"whisper_vad_free");segments_fn=proc<decltype(segments_fn)>(dll,"whisper_vad_segments_from_samples");count_fn=proc<decltype(count_fn)>(dll,"whisper_vad_segments_n_segments");start_fn=proc<decltype(start_fn)>(dll,"whisper_vad_segments_get_segment_t0");end_fn=proc<decltype(end_fn)>(dll,"whisper_vad_segments_get_segment_t1");free_segments=proc<decltype(free_segments)>(dll,"whisper_vad_free_segments");
        params=proc<decltype(&whisper_vad_default_params)>(dll,"whisper_vad_default_params")();params.min_speech_duration_ms=0;params.min_silence_duration_ms=2000;params.speech_pad_ms=400;params.max_speech_duration_s=std::numeric_limits<float>::infinity();params.samples_overlap=0;
        proc<decltype(&whisper_log_set)>(dll,"whisper_log_set")([](ggml_log_level,const char* text,void*){log(text);},nullptr);
        ctx=proc<decltype(&whisper_vad_init_from_file_with_params)>(dll,"whisper_vad_init_from_file_with_params")(utf8(path.wstring()).c_str(),defaults);if(!ctx)throw std::runtime_error("Cannot load VAD model: "+utf8(path.wstring()));
    }
    std::vector<std::pair<size_t,size_t>> speech(const std::vector<float>& audio){
        auto segments=segments_fn(ctx,params,audio.data(),int(audio.size()));if(!segments)throw std::runtime_error("Voice activity detection failed");
        struct Guard{decltype(free_segments) fn;whisper_vad_segments* p;~Guard(){fn(p);}} guard{free_segments,segments};
        std::vector<std::pair<size_t,size_t>> out;for(int i=0;i<count_fn(segments);++i){size_t a=std::min(audio.size(),size_t(std::max(0.0f,start_fn(segments,i))*160));size_t b=std::min(audio.size(),size_t(std::max(0.0f,end_fn(segments,i))*160));if(b>a){if(!out.empty()&&a<=out.back().second)out.back().second=std::max(b,out.back().second);else out.emplace_back(a,b);}}return out;
    }
};
}
struct Ct2Backend::Impl {
    Config& cfg;std::unique_ptr<ct::models::Whisper> model;std::unique_ptr<Tokenizer> tokenizer;Vad vad;Json metadata,gpu;std::vector<size_t> initial_prompt;fs::path vad_path;
    Impl(Config& c):cfg(c){}
};
Ct2Backend::Ct2Backend(Config& c):p_(std::make_unique<Impl>(c)){}
Ct2Backend::~Ct2Backend()=default;
void Ct2Backend::load(){
    auto& p=*p_;auto cfg=p.cfg.snapshot();auto raw=cfg.value("native_ct2_model_path","");fs::path path=raw.empty()?executable_dir()/L"models/ctranslate2"/wide(cfg.value("model","large-v3-turbo")):fs::path(wide(raw));
    if(!fs::exists(path/L"model.bin"))throw std::runtime_error("CTranslate2 model missing: "+utf8(path.wstring())+". Run native/setup.ps1.");
    bool cuda=cfg.value("device","cuda")=="cuda";auto device=cuda?ct::Device::CUDA:ct::Device::CPU;
    if(cuda&&ct::get_device_count(device)<1)throw std::runtime_error("CTranslate2 reports no CUDA devices; CPU fallback disabled");
    ct::ReplicaPoolConfig pool;pool.num_threads_per_replica=4;
    p.model=std::make_unique<ct::models::Whisper>(utf8(path.wstring()),device,ct::str_to_compute_type(cfg.value("compute_type","int8_float16")),std::vector<int>{0},false,pool);
    const auto& loaded=*p.model->get_first_replica().model();if(loaded.device()!=device)throw std::runtime_error("CTranslate2 loaded on the wrong device");
    p.tokenizer=std::make_unique<Tokenizer>(path/L"tokenizer.json");
    auto vad=cfg.value("native_vad_path","");p.vad_path=vad.empty()?executable_dir()/L"models/ggml-silero-v5.1.2.bin":fs::path(wide(vad));if(cfg.value("vad_filter",true))p.vad.load(p.vad_path);
    p.gpu=Json::object();if(cuda){auto driver=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(driver){char name[256]{};size_t total=0;int version=0;
        proc<int(__stdcall*)(char*,int,int)>(driver,"cuDeviceGetName")(name,256,0);proc<int(__stdcall*)(size_t*,int)>(driver,"cuDeviceTotalMem_v2")(&total,0);proc<int(__stdcall*)(int*)>(driver,"cuDriverGetVersion")(&version);
        p.gpu={{"name",name},{"vram_bytes",total},{"cuda_devices",ct::get_device_count(device)},{"driver_cuda_version",version}};FreeLibrary(driver);}}
    p.metadata={{"backend","ctranslate2"},{"backend_version","4.8.2"},{"model",cfg["model"]},{"model_path",utf8(path.wstring())},{"device",cuda?"cuda":"cpu"},{"compute_type",ct::compute_type_to_str(loaded.effective_compute_type())},{"cuda_backend_verified",cuda},{"cuda_devices",p.gpu.value("cuda_devices",0)},{"gpu",p.gpu.value("name","CPU")},{"python_required",false}};
    log("CTranslate2 native model loaded: "+p.metadata.dump());
}
// faster-whisper encodes the prompt with a leading space; so does this.
void Ct2Backend::set_initial_prompt(const std::string& prompt){p_->initial_prompt=prompt.empty()?std::vector<size_t>{}:p_->tokenizer->encode(" "+prompt);}
Json Ct2Backend::health() const {auto result=p_->metadata;result["status"]="ok";return result;}
Json Ct2Backend::gpu_info() const{return p_->gpu;}
Result Ct2Backend::transcribe(const std::vector<float>& input,std::string language,int beam,bool word_times,std::atomic<bool>* cancel,std::string_view committed_context){
    auto& p=*p_;auto cfg=p.cfg.snapshot();auto began=std::chrono::steady_clock::now();auto stopped=[&]{return cancel&&cancel->load();};
    Result out;out.json=p.metadata;out.json["text"]="";out.json["duration"]=double(input.size())/16000;out.json["processing_time"]=0;
    if(language.empty())language=cfg.value("language","auto");if(language=="detect"||language=="autodetect")language="auto";out.json["language"]=language;
    if(input.size()>size_t(INT_MAX))throw std::runtime_error("Audio is too long");if(input.size()<1600||stopped())return out;
    std::vector<std::pair<size_t,size_t>> ranges;
    if(cfg.value("vad_filter",true)){p.vad.load(p.vad_path);ranges=p.vad.speech(input);}else ranges.emplace_back(0,input.size());
    std::vector<float> audio;for(auto [a,b]:ranges)audio.insert(audio.end(),input.begin()+a,input.begin()+b);
    auto original_time=[&](double t,bool ending){double offset=0;for(auto[a,b]:ranges){double n=double(b-a)/16000;if(t<offset+n||(ending&&t<=offset+n))return double(a)/16000+std::max(0.0,t-offset);offset+=n;}return ranges.empty()?0.0:double(ranges.back().second)/16000;};
    if(audio.empty())return out;
    auto features=log_mel(audio,int(p.model->n_mels()));size_t total_frames=features.frames?features.frames-1:0;size_t seek=0;
    auto& tok=*p.tokenizer;const size_t eot=tok.id("<|endoftext|>"),sot=tok.id("<|startoftranscript|>"),task=tok.id("<|transcribe|>"),timestamps=tok.id("<|notimestamps|>")+1;
    std::vector<size_t> previous=p.initial_prompt;
    // Live dictation discards committed audio. Retain its text as decoder
    // context so a final isolated word is interpreted in its original sentence.
    // This prompt is not returned as new text and cannot rewrite delivered text.
    if(!committed_context.empty()) {
        // Only the final 223 tokens enter Whisper's prompt. Bound tokenization
        // work for long dictations while preserving a complete UTF-8 boundary.
        if(committed_context.size()>4096) {
            committed_context.remove_prefix(committed_context.size()-4096);
            while(!committed_context.empty() && (static_cast<unsigned char>(committed_context.front())&0xc0)==0x80)
                committed_context.remove_prefix(1);
        }
        auto context=p.tokenizer->encode(" "+std::string(committed_context));
        previous.insert(previous.end(),context.begin(),context.end());
    }
    std::string complete;
    while(seek<total_frames&&!stopped()){
        const size_t count=std::min(size_t(3000),total_frames-seek);std::vector<float> chunk(features.n_mels*3000,0);
        for(size_t m=0;m<features.n_mels;++m)std::copy_n(features.data.begin()+m*features.frames+seek,count,chunk.begin()+m*3000);
        ct::StorageView view({1,ct::dim_t(features.n_mels),3000},chunk);auto encoded=p.model->encode(view,false).get();if(stopped())break;
        if(language=="auto"){auto probabilities=p.model->detect_language(encoded)[0].get();if(probabilities.empty())throw std::runtime_error("Language detection returned no result");language=probabilities.front().first; if(language.starts_with("<|"))language=language.substr(2,language.size()-4);out.json["language"]=language;}
        std::vector<size_t> start_sequence{sot,tok.id("<|"+language+"|>"),task},prompt;
        if(!previous.empty()){prompt.push_back(tok.id("<|startofprev|>"));size_t n=std::min(size_t(223),previous.size());prompt.insert(prompt.end(),previous.end()-n,previous.end());}prompt.insert(prompt.end(),start_sequence.begin(),start_sequence.end());
        ct::models::WhisperOptions opts;opts.beam_size=beam?beam:cfg.value("beam_size",5);opts.return_scores=true;opts.return_no_speech_prob=true;
        ct::models::WhisperGenerationResult result;double avg=-100;float temperature=0;
        for(int attempt=0;attempt<=5&&!stopped();++attempt){temperature=attempt*.2f;if(attempt){opts.beam_size=1;opts.num_hypotheses=5;opts.sampling_topk=0;opts.sampling_temperature=temperature;}
            result=p.model->generate(encoded,std::vector<std::vector<size_t>>{prompt},opts)[0].get();if(result.sequences_ids.empty())break;
            size_t len=result.sequences_ids[0].size();avg=result.scores.empty()?0:result.scores[0]*len/double(len+1);if(avg>=-1||result.no_speech_prob>.6)break;
        }
        if(stopped())break;if(result.sequences_ids.empty()||(result.no_speech_prob>.6&&avg<-1)){seek+=count;continue;}
        const auto& tokens=result.sequences_ids[0];std::vector<size_t> boundaries;for(size_t i=1;i<tokens.size();++i)if(tokens[i-1]>=timestamps&&tokens[i]>=timestamps)boundaries.push_back(i);
        const bool single_end=tokens.size()>=2&&tokens[tokens.size()-2]<timestamps&&tokens.back()>=timestamps;
        size_t next=seek+count;std::vector<size_t> accepted;
        // A segment Whisper made up over silence (subtitle credits) is neither typed nor
        // kept as context, where it would make the next window repeat it.
        auto keep=[&](size_t from,size_t to){std::vector<size_t> part;for(size_t i=from;i<to;++i)if(tokens[i]<eot)part.push_back(tokens[i]);
            if(!part.empty()&&!is_hallucination(tok.decode(part)))accepted.insert(accepted.end(),part.begin(),part.end());};
        if(!boundaries.empty()){
            if(single_end)boundaries.push_back(tokens.size());size_t last=0;
            for(size_t boundary:boundaries){keep(last,boundary);last=boundary;}
            if(!single_end){size_t frames=(tokens[boundaries.back()-1]-timestamps)*2;next=seek+std::clamp(frames,size_t(1),count);}
        }else keep(0,tokens.size());
        std::string decoded=tok.decode(accepted);complete+=decoded;
        if(word_times&&!accepted.empty()){
            auto aligned=p.model->align(encoded,start_sequence,std::vector<std::vector<size_t>>{accepted},std::vector<size_t>{count},7)[0].get();if(stopped())break;
            std::vector<double> jumps;ct::dim_t prev=-1;for(auto[index,frame]:aligned.alignments){if(index!=prev){jumps.push_back(double(frame)/50);prev=index;}}
            struct Group{std::string text;size_t begin,end;};std::vector<Group> groups;
            for(size_t i=0;i<accepted.size();++i){auto piece=tok.token_bytes(accepted[i]);if(groups.empty()||(!piece.empty()&&piece.front()==' '))groups.push_back({piece,i,i+1});else{groups.back().text+=piece;groups.back().end=i+1;}}
            for(auto&g:groups){if(g.end>=jumps.size())continue;double a=original_time(seek*.01+jumps[g.begin],false),b=original_time(seek*.01+jumps[g.end],true);if(a<b)out.hypothesis.words.push_back({wide(g.text),a,b});}
        }
        previous.insert(previous.end(),accepted.begin(),accepted.end());if(temperature>.5)previous.clear();seek=next;
    }
    if(stopped()){out.json["text"]="";out.hypothesis={};return out;}
    out.hypothesis.text=trim(wide(complete));out.json["text"]=utf8(out.hypothesis.text);out.json["language"]=language;out.json["processing_time"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();return out;
}
}
