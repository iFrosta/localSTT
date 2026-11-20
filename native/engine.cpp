#include "engine.hpp"
#include "ct2.hpp"
#include "platform.hpp"
#include "vendor/whisper/whisper.h"
#include "vendor/whisper/ggml-backend.h"
#include "vendor/httplib.h"
#include <algorithm>
#include <fstream>
#include <thread>
#include <cmath>
#include <sstream>
#include <iostream>
namespace localstt {
namespace {
std::mutex clients_mutex;
std::vector<httplib::Client*> clients;
std::atomic<bool> http_stopping=false;
template<class T> T symbol(HMODULE dll,const char* name) { auto fn=GetProcAddress(dll,name); if(!fn) throw std::runtime_error(std::string("Missing native symbol: ")+name); return reinterpret_cast<T>(fn); }
double elapsed(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count(); }
Json http_json(const Json& cfg,const char* path,const Json* body=nullptr,int timeout=0) {
    std::string base=cfg.value("ollama_base_url","http://127.0.0.1:11434");
    httplib::Client client(base); client.set_connection_timeout(3); client.set_read_timeout(timeout?timeout:int(std::ceil(cfg.value("ollama_timeout_seconds",20.0)))); client.set_write_timeout(10);
    {std::lock_guard lock(clients_mutex);if(http_stopping)throw std::runtime_error("Application is stopping");clients.push_back(&client);}
    struct Registration {httplib::Client* p;~Registration(){std::lock_guard lock(clients_mutex);std::erase(clients,p);}} registration{&client};
    auto r=body?client.Post(path,body->dump(),"application/json"):client.Get(path);
    if(!r) throw std::runtime_error("Ollama unavailable: "+httplib::to_string(r.error()));
    if(r->status!=200) throw std::runtime_error("Ollama HTTP "+std::to_string(r->status)); return Json::parse(r->body);
}
}
struct Engine::Impl {
    std::unique_ptr<Ct2Backend> ct2;
    Config& config; Json loaded=Json::object(),gpu=Json::object(); Dictionary dictionary;
    mutable std::mutex mutex; HMODULE dll=nullptr,driver=nullptr; whisper_context* ctx=nullptr; std::string prompt; fs::path vad;
    std::string cuda_backend_name, cuda_selected_log, cuda_failed_log, cuda_weights_log;
    std::atomic<bool> cuda_selected{false}, cuda_failed{false}, cuda_weights{false}, capture_cuda_init{false};
#define FN(name) decltype(&name) name##_fn=nullptr
    FN(whisper_context_default_params); FN(whisper_full_default_params); FN(whisper_init_from_file_with_params); FN(whisper_free); FN(whisper_full);
    FN(whisper_full_n_segments); FN(whisper_full_get_segment_text); FN(whisper_full_get_segment_t0); FN(whisper_full_get_segment_t1);
    FN(whisper_full_n_tokens); FN(whisper_full_get_token_text); FN(whisper_full_get_token_data); FN(whisper_token_eot); FN(whisper_full_lang_id); FN(whisper_lang_str); FN(whisper_lang_id); FN(whisper_log_set);
#undef FN
    Impl(Config& c):config(c){}
    // Before every transcription, under the engine lock: a new word takes effect on the
    // next dictation, not the next restart.
    void refresh_dictionary(){
        auto path=config.user_file(L"dictionary.json");if(!dictionary.stale(path))return;
        dictionary=Dictionary::load(path);prompt=dictionary.initial_prompt();if(ct2)ct2->set_initial_prompt(prompt);
        log("dictionary loaded: "+std::to_string(dictionary.term_count())+" terms, "+std::to_string(dictionary.replacement_count())+" replacements");
        if(auto overflow=dictionary.prompt_overflow())log("the term list is "+std::to_string(overflow)+" characters past what Whisper reads; the last terms are ignored. Move the rarely-spoken ones to replacements instead.");
    }
    ~Impl(){ if(ctx&&whisper_free_fn) whisper_free_fn(ctx); if(whisper_log_set_fn) whisper_log_set_fn([](ggml_log_level,const char* text,void*) { try { log(text); } catch (...) {} },nullptr); if(dll)FreeLibrary(dll); if(driver)FreeLibrary(driver); }
};
Engine::Engine(Config& c):p_(std::make_unique<Impl>(c)){}
Engine::~Engine()=default;
void Engine::load() {
    auto& p=*p_; std::lock_guard lock(p.mutex); if(p.ctx||p.ct2) return;
    auto cfg=p.config.snapshot();
    if(cfg.value("native_backend","ctranslate2")=="ctranslate2"){
        auto backend=std::make_unique<Ct2Backend>(p.config);backend->load();p.loaded=backend->health();p.gpu=backend->gpu_info();p.ct2=std::move(backend);p.refresh_dictionary();return;
    }
    auto model=cfg.value("native_model_path","");
    std::string compute=cfg.value("compute_type","float16");
    std::string suffix;
    if(compute=="int8_float16"||compute=="int8"||compute=="q8_0")suffix="-q8_0";
    else if(compute=="q5_0")suffix="-q5_0";
    else if(compute!="float16")throw std::runtime_error("Choose float16, int8_float16 (Q8_0), q8_0 or q5_0 for the native backend.");
    fs::path path=model.empty()?executable_dir()/L"models"/wide("ggml-"+cfg.value("model","large-v3-turbo")+suffix+".bin"):fs::path(wide(model));
    if(!fs::exists(path)) throw std::runtime_error("Model missing: "+utf8(path.wstring())+". Run native/setup.ps1.");
    auto vad=cfg.value("native_vad_path",""); p.vad=vad.empty()?executable_dir()/L"models/ggml-silero-v5.1.2.bin":fs::path(wide(vad));
    if(cfg.value("vad_filter",true)&&!fs::exists(p.vad)) throw std::runtime_error("VAD model missing: "+utf8(p.vad.wstring()));
    p.refresh_dictionary();
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    p.dll=LoadLibraryExW((executable_dir()/L"whisper.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!p.dll) throw std::runtime_error("Cannot load whisper.dll / CUDA runtime; Win32 error "+std::to_string(GetLastError()));
#define LOAD(name) p.name##_fn=symbol<decltype(&name)>(p.dll,#name)
    LOAD(whisper_context_default_params); LOAD(whisper_full_default_params); LOAD(whisper_init_from_file_with_params); LOAD(whisper_free); LOAD(whisper_full);
    LOAD(whisper_full_n_segments); LOAD(whisper_full_get_segment_text); LOAD(whisper_full_get_segment_t0); LOAD(whisper_full_get_segment_t1);
    LOAD(whisper_full_n_tokens); LOAD(whisper_full_get_token_text); LOAD(whisper_full_get_token_data); LOAD(whisper_token_eot); LOAD(whisper_full_lang_id); LOAD(whisper_lang_str); LOAD(whisper_lang_id); LOAD(whisper_log_set);
#undef LOAD
    p.whisper_log_set_fn([](ggml_log_level,const char* text,void* user) {
        auto& state=*static_cast<Impl*>(user);
        const std::string_view message=text?text:"";
        if(state.capture_cuda_init.load()) {
            if(message.find(state.cuda_selected_log)!=message.npos)state.cuda_selected=true;
            if(message.find(state.cuda_failed_log)!=message.npos ||
               message.find("whisper_backend_init_gpu: no GPU found")!=message.npos)state.cuda_failed=true;
            if(message.find("whisper_model_load:")!=message.npos &&
               message.find(state.cuda_weights_log)!=message.npos)state.cuda_weights=true;
        }
        try { log(message); } catch (...) {} // Never unwind through a C callback.
    },&p);
    bool cuda=cfg.value("device","cuda")=="cuda";
    if(cuda) {
        p.driver=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!p.driver)throw std::runtime_error("NVIDIA driver is missing");
        auto init=symbol<int(__stdcall*)(unsigned)>(p.driver,"cuInit"); auto count=symbol<int(__stdcall*)(int*)>(p.driver,"cuDeviceGetCount");
        int n=0; if(init(0)||count(&n)||n<1)throw std::runtime_error("CUDA initialization failed; CPU fallback disabled");
        char name[256]{}; auto getname=symbol<int(__stdcall*)(char*,int,int)>(p.driver,"cuDeviceGetName");getname(name,256,0);
        size_t total=0; symbol<int(__stdcall*)(size_t*,int)>(p.driver,"cuDeviceTotalMem_v2")(&total,0);
        int major=0,minor=0; auto attr=symbol<int(__stdcall*)(int*,int,int)>(p.driver,"cuDeviceGetAttribute");attr(&major,75,0);attr(&minor,76,0);
        int version=0;symbol<int(__stdcall*)(int*)>(p.driver,"cuDriverGetVersion")(&version);
        p.gpu={{"name",name},{"cuda_devices",n},{"vram_bytes",total},{"compute_capability",std::to_string(major)+"."+std::to_string(minor)},{"driver_cuda_version",version}};
    }
    auto params=p.whisper_context_default_params_fn(); params.use_gpu=cuda; params.flash_attn=true;
    if(cuda) {
        // Driver presence does not prove whisper was built with CUDA. Query the
        // same backend registry whisper uses, and choose CUDA explicitly rather
        // than whichever GPU happens to occupy slot zero.
        const HMODULE registry=GetModuleHandleW(L"ggml.dll");
        const HMODULE base=GetModuleHandleW(L"ggml-base.dll");
        if(!registry||!base)throw std::runtime_error("Whisper backend libraries are missing");
        symbol<decltype(&ggml_backend_load_all_from_path)>(registry,"ggml_backend_load_all_from_path")(
            utf8(executable_dir().wstring()).c_str());
        const auto count=symbol<decltype(&ggml_backend_dev_count)>(registry,"ggml_backend_dev_count");
        const auto get=symbol<decltype(&ggml_backend_dev_get)>(registry,"ggml_backend_dev_get");
        const auto type=symbol<decltype(&ggml_backend_dev_type)>(base,"ggml_backend_dev_type");
        const auto reg=symbol<decltype(&ggml_backend_dev_backend_reg)>(base,"ggml_backend_dev_backend_reg");
        const auto reg_name=symbol<decltype(&ggml_backend_reg_name)>(base,"ggml_backend_reg_name");
        const auto dev_name=symbol<decltype(&ggml_backend_dev_name)>(base,"ggml_backend_dev_name");
        const auto describe=symbol<decltype(&ggml_backend_dev_description)>(base,"ggml_backend_dev_description");
        const auto memory=symbol<decltype(&ggml_backend_dev_memory)>(base,"ggml_backend_dev_memory");
        int gpu_index=0;
        for(size_t i=0;i<count();++i) {
            const auto device=get(i);
            const auto device_type=type(device);
            if(device_type!=GGML_BACKEND_DEVICE_TYPE_GPU && device_type!=GGML_BACKEND_DEVICE_TYPE_IGPU)continue;
            if(std::string_view(reg_name(reg(device)))=="CUDA") {
                params.gpu_device=gpu_index;
                p.cuda_backend_name=dev_name(device);
                p.gpu["name"]=describe(device);
                size_t free=0,total=0;memory(device,&free,&total);
                p.gpu["vram_bytes"]=total;p.gpu["free_vram_bytes_before_load"]=free;
                p.gpu["ggml_device"]=p.cuda_backend_name;
                break;
            }
            ++gpu_index;
        }
        if(p.cuda_backend_name.empty())throw std::runtime_error("No CUDA backend is registered in whisper.cpp; CPU fallback disabled");
        p.cuda_selected_log="whisper_backend_init_gpu: using "+p.cuda_backend_name+" backend";
        p.cuda_failed_log="whisper_backend_init_gpu: failed to initialize "+p.cuda_backend_name+" backend";
        p.cuda_weights_log=p.cuda_backend_name+" total size =";
        p.cuda_selected=false;p.cuda_failed=false;p.cuda_weights=false;
        p.capture_cuda_init=true;
    }
    p.ctx=p.whisper_init_from_file_with_params_fn(utf8(path.wstring()).c_str(),params);
    p.capture_cuda_init=false;
    if(!p.ctx) throw std::runtime_error("Whisper model initialization failed; see logs/localstt-native.log");
    // whisper.cpp 1.8.2 can successfully return a context after falling back to
    // CPU. The pinned version's initialization logs expose its actual selected
    // execution backend and weight buffers. Require both before reporting CUDA.
    if(cuda&&(!p.cuda_selected.load()||p.cuda_failed.load()||!p.cuda_weights.load())) {
        p.whisper_free_fn(p.ctx);p.ctx=nullptr;
        throw std::runtime_error("CUDA model/backend allocation was not confirmed; CPU fallback disabled. See logs/localstt-native.log");
    }
    const int ftype=symbol<decltype(&whisper_model_ftype)>(p.dll,"whisper_model_ftype")(p.ctx);
    const std::string precision=ftype==1?"float16":ftype==7?"q8_0":ftype==8?"q5_0":("ggml_ftype_"+std::to_string(ftype));
    p.loaded={{"status","ok"},{"backend","whisper.cpp"},{"backend_version","1.8.2"},{"model",cfg["model"]},{"device",cfg["device"]},{"compute_type",precision},{"configured_compute_type",compute},{"model_path",utf8(path.wstring())},{"python_required",false},{"gpu",p.gpu.value("name","CPU")},{"cuda_devices",p.gpu.value("cuda_devices",0)}};
    p.loaded["cuda_backend_verified"]=cuda;
    if(cuda)p.loaded["gpu_backend"]=p.cuda_backend_name;
    log("Native model loaded: "+p.loaded.dump());
}
std::wstring Engine::correct(std::wstring text) const { std::lock_guard lock(p_->mutex); return p_->dictionary.apply(std::move(text)); }
Result Engine::transcribe(const std::vector<float>& audio,std::string language,int beam,bool words,std::atomic<bool>* cancel,std::string_view context) {
    auto& p=*p_; std::lock_guard lock(p.mutex);
    p.refresh_dictionary();
    if(p.ct2){auto r=p.ct2->transcribe(audio,language,beam,words,cancel,context);r.hypothesis.text=p.dictionary.apply(r.hypothesis.text);r.json["text"]=utf8(r.hypothesis.text);return r;}
    if(!p.ctx)throw std::runtime_error("Model is not ready");
    const auto start=std::chrono::steady_clock::now(); auto cfg=p.config.snapshot();
    if(language.empty())language=cfg.value("language","auto"); if(language=="detect"||language=="autodetect")language="auto";
    if(language!="auto" && p.whisper_lang_id_fn(language.c_str())<0)throw std::runtime_error("Unsupported language: "+language);
    Result r; r.json=p.loaded; r.json.erase("status");r.json["language"]=language;r.json["duration"]=double(audio.size())/16000;r.json["processing_time"]=0;r.json["text"]="";
    if(cancel&&cancel->load())return r;
    if(audio.size()<1600) return r;
    double energy=0; for(float sample:audio)energy+=sample*sample;
    if(std::sqrt(energy/audio.size())<0.0001)return r;
    if(audio.size()>size_t(INT_MAX))throw std::runtime_error("Audio is too long");
    int beams=beam?beam:cfg.value("beam_size",5);
    auto params=p.whisper_full_default_params_fn(beams>1?WHISPER_SAMPLING_BEAM_SEARCH:WHISPER_SAMPLING_GREEDY);
    params.n_threads=std::clamp(int(std::thread::hardware_concurrency())/2,1,8); params.beam_search.beam_size=beams;params.greedy.best_of=1;
    params.language=language.c_str();params.no_context=true;params.token_timestamps=words;params.split_on_word=true;
    params.print_realtime=false;params.print_progress=false;params.print_timestamps=false;params.suppress_nst=true;
    auto prompt=p.prompt;if(!context.empty()){if(!prompt.empty())prompt+=' ';prompt+=context;} params.initial_prompt=prompt.empty()?nullptr:prompt.c_str();params.temperature_inc=0.2f;
    std::string vadpath=utf8(p.vad.wstring());params.vad=cfg.value("vad_filter",true)&&!words;params.vad_model_path=vadpath.c_str();
    if(params.vad&&!fs::exists(p.vad))throw std::runtime_error("VAD model is missing");
    if(cancel){params.abort_callback=[](void* user){return static_cast<std::atomic<bool>*>(user)->load();};params.abort_callback_user_data=cancel;}
    int rc=p.whisper_full_fn(p.ctx,params,audio.data(),int(audio.size()));
    if(cancel&&cancel->load())return r; if(rc)throw std::runtime_error("Whisper decoding failed: "+std::to_string(rc));
    std::string text; std::string word; double wstart=0,wend=0;
    auto flush=[&]{ if(!word.empty()){try { r.hypothesis.words.push_back({wide(word),wstart,wend}); }catch(...){} word.clear();} };
    for(int i=0;i<p.whisper_full_n_segments_fn(p.ctx);++i){
        text+=p.whisper_full_get_segment_text_fn(p.ctx,i);
        if(words)for(int j=0;j<p.whisper_full_n_tokens_fn(p.ctx,i);++j){ auto t=p.whisper_full_get_token_data_fn(p.ctx,i,j); if(t.id>=p.whisper_token_eot_fn(p.ctx))continue;
            std::string piece=p.whisper_full_get_token_text_fn(p.ctx,i,j); if(piece.empty())continue;
            if(piece.front()==' '&&!word.empty())flush(); if(word.empty())wstart=std::max(0.0,t.t0*.01);
            word+=piece;wend=std::min(double(audio.size())/16000,std::max(wstart,t.t1*.01));
        }
        flush();
    }
    r.hypothesis.text=p.dictionary.apply(trim(wide(text)));r.json["text"]=utf8(r.hypothesis.text);
    r.json["language"]=p.whisper_lang_str_fn(p.whisper_full_lang_id_fn(p.ctx));r.json["processing_time"]=elapsed(start); return r;
}
Json Engine::health() const { std::lock_guard lock(p_->mutex); auto j=p_->loaded; if(!p_->ctx&&!p_->ct2)j={{"status","loading"},{"python_required",false}}; auto c=p_->config.snapshot();j["language"]=c["language"];j["live_dictation"]=c["live_dictation"];j["version"]=version;j["copy_to_clipboard"]=c["copy_to_clipboard"];
    j["hotkeys"]={{"dictation",c["hotkey_dictation"]},{"cleanup",c["hotkey_cleanup"]},{"command",c["hotkey_command"]},{"cancel",c.value("cancel_on_escape",true)?c["hotkey_cancel"]:Json()}};return j; }
Json Engine::gpu_info() const { std::lock_guard lock(p_->mutex); return p_->gpu; }
Service::Service(Config& c):engine(c),config_(c){}
void Service::shutdown(){http_stopping=true;std::lock_guard lock(clients_mutex);for(auto client:clients)client->stop();}
Result Service::transcribe(const std::vector<float>& a,std::string l,int b,bool w,bool rec,std::atomic<bool>* cancel,std::string_view context){auto r=engine.transcribe(a,l,b,w,cancel,context);if(rec&&(!cancel||!cancel->load()))record(r);return r;}
Json Service::metrics() const {std::lock_guard lock(mutex_);return {{"transcription_count",count_},{"total_audio_duration",duration_},{"average_processing_time",count_?processing_/count_:0},{"realtime_factor",duration_?processing_/duration_:0}};}
void Service::performance(const std::string& key,Json data) { std::lock_guard lock(mutex_); try{auto path=config_.state_dir/L"performance.json";auto all=read_json(path);all[key]=std::move(data);write_json(path,all);}catch(const std::exception&e){log(e.what());} }
void Service::record(const Result& r) {
    double d=r.json.value("duration",0.0),p=r.json.value("processing_time",0.0);{std::lock_guard lock(mutex_);++count_;duration_+=d;processing_+=p;}
    auto cfg=config_.snapshot();if(cfg.value("history_enabled",false)) { auto row=r.json;row["ts"]=timestamp();append_json(config_.state_dir/L"history.jsonl",row); }
    if(cfg.value("performance_tracking",true))performance("transcription",{{"ts",timestamp()},{"audio_seconds",d},{"processing_seconds",p},{"realtime_factor",d?p/d:0},{"chars",r.hypothesis.text.size()},{"model",r.json["model"]},{"compute_type",r.json["compute_type"]},{"device",r.json["device"]},{"beam_size",cfg["beam_size"]},{"session",metrics()}});
}
Json Service::cleanup_recommendation() {
    auto cfg=config_.snapshot();auto gpu=engine.gpu_info();double total=gpu.value("vram_bytes",uint64_t(0))/1073741824.0;
    const bool compact=cfg.value("compute_type","int8_float16").find("int8")!=std::string::npos;
    double whisper=cfg.value("model","large-v3-turbo")=="large-v3"?(compact?3.2:4.5):(compact?1.4:2.5);
    double budget=std::max(0.0,total-whisper-0.5);std::string recommended=budget>=3.5?"qwen3:4b":budget>=1.8?"qwen3:1.7b":"qwen3.5:0.8b";
    Json r={{"budget_gb",budget},{"pull",recommended},{"best_installed",nullptr},{"reachable",false},{"installed",Json::array()},{"note","VRAM estimate; Ollama may offload to CPU."}};
    try{auto models=http_json(cfg,"/api/tags",nullptr,5).value("models",Json::array());r["reachable"]=true;
        for(const auto&m:models){std::string name=m.value("name","");if(name.find("qwen")==std::string::npos||name.ends_with(":cloud")||m.contains("remote_host")||m.contains("remote_model"))continue;
            double needed=m.value("size",uint64_t(0))/1073741824.0+0.6;r["installed"].push_back({{"name",name},{"estimated_gb",needed},{"fits",needed<=budget}});if(r["best_installed"].is_null()&&needed<=budget)r["best_installed"]=name;
        }
    }catch(const std::exception&e){r["note"]=e.what();}return r;
}
void Service::pull_cleanup(std::string requested) { auto cfg=config_.snapshot();auto rec=cleanup_recommendation();std::string model=!requested.empty()?requested:cfg["ollama_model"].is_string()?cfg["ollama_model"].get<std::string>():rec["pull"].get<std::string>();Json body={{"name",model},{"stream",false}};http_json(cfg,"/api/pull",&body,3600);config_.update({{"ollama_model",model}}); }
std::string Service::cleanup(const std::string& text) {
    auto cfg=config_.snapshot();try{
        std::string model=cfg["ollama_model"].is_string()?cfg["ollama_model"].get<std::string>():"";
        if(model.empty()){auto rec=cleanup_recommendation();if(rec["best_installed"].is_string())model=rec["best_installed"].get<std::string>();}
        if(model.empty())throw std::runtime_error("No installed cleanup model fits available VRAM");
        const auto prompt=load_cleanup_prompt(config_.user_file(L"cleanup-prompt.txt"));
        Json body={{"model",model},{"stream",false},{"think",false},{"messages",Json::array({{{"role","system"},{"content",prompt}},{{"role","user"},{"content",text}}})},{"options",{{"temperature",0.1},{"num_predict",512},{"num_ctx",4096}}}};
        auto start=std::chrono::steady_clock::now();auto r=http_json(cfg,"/api/chat",&body);double seconds=r.value("eval_duration",0.0)/1e9;int tokens=r.value("eval_count",0);
        if(cfg.value("performance_tracking",true))performance("cleanup",{{"ts",timestamp()},{"model",model},{"tokens",tokens},{"generate_seconds",seconds},{"tokens_per_second",seconds?tokens/seconds:0},{"total_seconds",elapsed(start)},{"prompt_tokens",r.value("prompt_eval_count",0)}});
        auto cleaned=utf8(trim(wide(r.value("message",Json::object()).value("content",""))));return cleaned.empty()?text:cleaned;
    }catch(const std::exception&e){log(std::string("Cleanup skipped: ")+e.what());return text;}
}
Json Service::preflight() {
    Json checks=Json::array();auto health=engine.health();checks.push_back({{"name","Native engine"},{"ok",health.value("status","")=="ok"},{"detail",health}});
    auto devices=Recorder::devices();checks.push_back({{"name","Microphone"},{"ok",!devices.empty()},{"detail",devices}});
    checks.push_back({{"name","CUDA driver and VRAM"},{"ok",health.value("device","")=="cpu"||engine.gpu_info().value("cuda_devices",0)>0},{"detail",engine.gpu_info()}});
    auto rec=cleanup_recommendation();checks.push_back({{"name","Ollama cleanup"},{"ok",rec["reachable"]},{"detail",rec}});
    Json r={{"ts",timestamp()},{"native",true},{"checks",checks},{"ok",health.value("status","")=="ok"}};write_json(config_.state_dir/L"preflight.json",r);return r;
}
}
