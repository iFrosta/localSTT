#include "ct2.hpp"
#include "platform.hpp"
#include "live_dictation.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

using namespace localstt;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool condition,const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}
std::wstring normalized(std::wstring_view text) {
    std::wstring result;
    for(wchar_t ch:text) {
        if(ch>=L'A'&&ch<=L'Z')ch+=L'a'-L'A';
        if((ch>=L'a'&&ch<=L'z')||(ch>=L'0'&&ch<=L'9'))result+=ch;
        else if(!result.empty()&&result.back()!=L' ')result+=L' ';
    }
    return trim(result);
}
void check_transcript(const Result& result,bool words) {
    const auto text=normalized(result.hypothesis.text);
    require(text.find(L"ask not what your country can do for you")!=text.npos,
            "JFK transcription lost the first key phrase: "+utf8(result.hypothesis.text));
    require(text.find(L"what you can do for your country")!=text.npos,
            "JFK transcription lost the final key phrase: "+utf8(result.hypothesis.text));
    require(result.json.value("text","")==utf8(result.hypothesis.text),"JSON and hypothesis text differ");
    require(result.json.value("device","")=="cuda"&&result.json.value("cuda_backend_verified",false),
            "Integration test must run on the real CUDA backend");
    if(!words)return;
    require(result.hypothesis.words.size()>=15,"Expected meaningful word alignment for the JFK sample");
    double previous=0;
    std::wstring joined;
    const double duration=result.json.value("duration",0.0);
    for(const auto& word:result.hypothesis.words) {
        require(!trim(word.text).empty(),"Empty word alignment");
        require(std::isfinite(word.start)&&std::isfinite(word.end),"Non-finite word alignment");
        require(word.start>=0&&word.start<word.end&&word.end<=duration+0.05,"Word timestamp outside audio duration");
        require(word.start+0.021>=previous,"Word timestamps overlap or move backwards");
        previous=word.end;
        joined+=word.text;
    }
    // LiveDictation emits these words instead of result.text. Dropped words here
    // would silently disappear from live dictation even if normal STT looks right.
    require(normalized(joined)==text,"Aligned words do not preserve the complete transcription: "+utf8(joined));
}
Json words_json(const Result& result) {
    auto words=Json::array();
    for(const auto& word:result.hypothesis.words)
        words.push_back({{"word",utf8(word.text)},{"start",word.start},{"end",word.end}});
    return words;
}
void check_empty(const Result& result,const char* message) {
    require(result.hypothesis.text.empty()&&result.hypothesis.words.empty()&&result.json.value("text","").empty(),message);
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc<3) {
        std::cerr<<"Usage: test_localstt_ct2.exe JFK.wav ISOLATED_STATE_ROOT [INSTALL_ROOT] [--live]\n";
        return 2;
    }
    Json report={{"ok",false},{"tests",Json::array()}};
    const auto state=fs::absolute(argv[2])/(L"ct2-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(state);set_log_dir(state/L"logs");
        const fs::path root=argc>3&&std::wstring_view(argv[3])!=L"--live"?fs::absolute(argv[3]):executable_dir();
        bool live=false;for(int i=3;i<argc;++i)if(std::wstring_view(argv[i])==L"--live")live=true;
        Config config(state,root);
        config.update({{"native_backend","ctranslate2"},{"model","large-v3-turbo"},
                       {"compute_type","int8_float16"},{"device","cuda"},{"language","en"},
                       {"vad_filter",true},{"history_enabled",false},{"performance_tracking",false}});
        const auto audio=decode_audio(fs::absolute(argv[1]));
        require(audio.size()>16000*8&&audio.size()<16000*15,"Expected the upstream 11-second JFK speech sample");
        Ct2Backend backend(config);backend.load();
        report["health"]=backend.health();
        require(report["health"].value("backend","")=="ctranslate2","Wrong native backend");
        require(report["health"].value("compute_type","")=="int8_float16","Unexpected effective compute precision");

        const auto begin=Clock::now();
        auto result=backend.transcribe(audio,"en",5,true,nullptr);
        const double seconds=std::chrono::duration<double>(Clock::now()-begin).count();
        check_transcript(result,true);
        report["transcription"]=result.json;report["words"]=words_json(result);
        report["tests"].push_back({{"name","CUDA transcription and complete monotonic word alignment"},{"seconds",seconds},{"ok",true}});
        std::cout<<"Aligned transcript: "<<utf8(result.hypothesis.text)<<" ("<<seconds<<" s)\n";

        auto greedy_before=backend.transcribe(audio,"en",1,false,nullptr);
        report["greedy_before_cancellation"]=greedy_before.json;
        check_transcript(greedy_before,false);

        std::atomic<bool> cancelled=true;
        const auto cancelled_result=backend.transcribe(audio,"en",5,true,&cancelled);
        check_empty(cancelled_result,"A pre-cancelled request returned text or words");
        report["tests"].push_back({{"name","Pre-cancelled request"},{"ok",true}});

        const auto silent=backend.transcribe(std::vector<float>(16000*3,0.0f),"en",5,true,nullptr);
        check_empty(silent,"Silence produced a hallucinated transcript");
        report["tests"].push_back({{"name","Silence rejection"},{"ok",true}});

        // Cancel while an asynchronous transcription is active. CTranslate2's
        // generate future may finish before cancellation can take effect at the
        // decoder level; the application must still discard all returned output.
        // Timing alone cannot prove the exact internal encoder/decoder phase.
        std::vector<float> cancellation_audio;
        for(int i=0;i<3;++i)cancellation_audio.insert(cancellation_audio.end(),audio.begin(),audio.end());
        cancelled=false;
        std::promise<void> entered;
        auto started=entered.get_future();
        auto running=std::async(std::launch::async,[&] {
            entered.set_value();
            return backend.transcribe(cancellation_audio,"en",5,true,&cancelled);
        });
        started.wait();
        const auto delay=std::chrono::milliseconds(static_cast<int>(std::clamp(seconds*350,200.0,1500.0)));
        std::this_thread::sleep_for(delay);
        require(running.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready,
                "Cancellation request completed before the test could cancel it");
        const auto cancel_start=Clock::now();cancelled=true;
        check_empty(running.get(),"An in-flight cancelled request leaked transcript or word timings");
        report["tests"].push_back({{"name","In-flight cancellation discards all output"},{"delay_ms",delay.count()},
                                  {"completion_after_cancel_seconds",std::chrono::duration<double>(Clock::now()-cancel_start).count()},{"ok",true}});

        auto after_cancel=backend.transcribe(audio,"en",5,true,nullptr);
        report["beam5_after_cancellation"]=after_cancel.json;
        check_transcript(after_cancel,true);
        require(after_cancel.hypothesis.text==result.hypothesis.text,
                "Cancellation changed transcription with identical beam5/word-timing parameters");
        auto greedy_after=backend.transcribe(audio,"en",1,false,nullptr);
        report["greedy_after_cancellation"]=greedy_after.json;
        check_transcript(greedy_after,false);
        require(greedy_before.hypothesis.text==greedy_after.hypothesis.text,
                "Cancellation changed transcription with identical beam1 parameters");
        report["tests"].push_back({{"name","Backend reusable after cancellation with identical beam5 and beam1 parameters"},{"ok",true}});

        if(live) {
            LiveDictation dictation;
            std::wstring delivered;
            std::size_t calls=0;
            double capture_offset=0;
            report["live_passes"]=Json::array();
            LiveIO io;
            io.capture=[&](double offset,double maximum) {
                capture_offset=offset;
                const auto begin=std::min(audio.size(),static_cast<std::size_t>(offset*16000));
                const auto end=std::min(audio.size(),begin+static_cast<std::size_t>(maximum*16000));
                return AudioWindow{{audio.begin()+begin,audio.begin()+end},16000,false,end==audio.size()};
            };
            io.recognize=[&](const AudioWindow& window) {
                const auto context=utf8(dictation.text());
                auto recognized=backend.transcribe(window.mono,"en",1,true,nullptr,context);
                report["live_passes"].push_back({{"offset",capture_offset},{"context",context},{"result",recognized.json},{"words",words_json(recognized)}});
                ++calls;
                // Two matching real hypotheses confirm a prefix; stopping after
                // the second pass forces the uncommitted tail through final STT.
                if(calls==2)dictation.stop();
                if(calls>8)throw std::runtime_error("Live recognition did not converge");
                return recognized.hypothesis;
            };
            io.correct=[](std::wstring text){return text;};
            io.deliver=[&](std::wstring_view text){delivered+=text;return text.size();};
            dictation.run(io,std::chrono::milliseconds(0));
            require(delivered==dictation.text(),"Live output duplicated or omitted delivered chunks");
            Result streamed;streamed.hypothesis.text=delivered;streamed.json=after_cancel.json;streamed.json["text"]=utf8(delivered);
            check_transcript(streamed,false);
            const auto normalized_live=normalized(delivered);
            const auto normalized_final=normalized(greedy_after.hypothesis.text);
            require(normalized_live==normalized_final,"Incremental transcription duplicated or omitted words compared with final transcription: "+utf8(delivered));
            report["tests"].push_back({{"name","Real incremental recognition and final flush"},{"passes",calls},{"text",utf8(delivered)},{"ok",true}});
        }
        report["ok"]=true;
        write_json(state/L"ct2-test-result.json",report);
        std::cout<<"CT2 integration tests passed; report: "<<utf8((state/L"ct2-test-result.json").wstring())<<'\n';
        return 0;
    }catch(const std::exception& e) {
        report["error"]=e.what();
        try {write_json(state/L"ct2-test-result.json",report);}catch(...){}
        std::cerr<<"CT2 integration test failed: "<<e.what()<<"; state: "<<utf8(state.wstring())<<'\n';
        return 1;
    }
}
