#include "common.hpp"
#include "engine.hpp"
#include "platform.hpp"
#include "commands.hpp"
#include "ui.hpp"
#include "hotkeys.hpp"
#include "updates.hpp"
#include "vendor/httplib.h"
#include <shellapi.h>
#include <shlobj.h>
#include <thread>
#include <future>
#include <atomic>
#include <fstream>
#include <iostream>
#include <array>
#include <condition_variable>
using namespace localstt;
using namespace std::chrono_literals;
namespace {
std::array<std::atomic<bool>,256> keys{},chord_keys{}; std::array<bool,256> swallowed{},captured{}; std::atomic<bool> cancel_requested=false;
// Read by the hook and tick(), which both run on the UI thread; delivery workers only
// read chord_keys.
Bindings bindings;
KeyChord held(){KeyChord result;for(unsigned i=0;i<keys.size();++i)if(keys[i])if(auto name=key_name(i))result.insert(*name);return result;}
// Delivery waits for these: typing while a chord key is still down would mix the two.
bool chord_held(){for(size_t i=0;i<keys.size();++i)if(keys[i]&&chord_keys[i])return true;return false;}
LRESULT CALLBACK keyboard(int code,WPARAM wp,LPARAM lp){
    if(code==HC_ACTION){auto* k=reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        // Only LocalSTT's own SendInput is skipped, as localstt does it: injected input
        // from other tools (remappers, remote desktop) can still use the hotkeys.
        const bool own=(k->flags&LLKHF_INJECTED)&&k->dwExtraInfo==input_marker;
        if(!own&&k->vkCode<keys.size()){
        const auto vk=k->vkCode;bool down=wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN;bool was_down=keys[vk];keys[vk]=down;
        // A settings field is recording a chord: the keys are its, not ours or the
        // focused window's. Releases are swallowed only for presses that were.
        HWND recorder=capture_window();
        if(!down&&captured[vk]){captured[vk]=false;if(recorder)PostMessageW(recorder,capture_message,vk,0);return 1;}
        if(recorder&&GetForegroundWindow()==GetAncestor(recorder,GA_ROOT)){PostMessageW(recorder,capture_message,vk,down);if(down){captured[vk]=true;return 1;}}
        else if(!recorder){
            if(down&&!was_down&&bindings.cancels(held()))cancel_requested=true;
            // Win completing a binding must not reach the shell, or releasing it opens Start.
            if(vk==VK_LWIN||vk==VK_RWIN){if(down){if(!was_down){auto keys_held=held();auto mode=bindings.match(keys_held);swallowed[vk]=false;if(mode)for(auto& [name,chord]:bindings.modes)if(name==*mode)swallowed[vk]=chord.contains("win");}if(swallowed[vk])return 1;}else{bool suppress=swallowed[vk];swallowed[vk]=false;if(suppress)return 1;}}
        }
    }}return CallNextHookEx(nullptr,code,wp,lp);
}
class Api {
    httplib::Server server_;std::thread thread_;Service& service_;std::atomic<bool>& ready_;
public:
    Api(Service& s,std::atomic<bool>& ready):service_(s),ready_(ready){
        server_.set_payload_max_length(128*1024*1024);server_.set_read_timeout(30);server_.set_write_timeout(30);
        server_.new_task_queue=[](){return new httplib::ThreadPool(3);};
        server_.Get("/health",[this](const auto&,auto& res){if(!ready_){res.status=503;res.set_content(Json({{"status","loading"},{"python_required",false}}).dump(),"application/json");return;}res.set_content(service_.engine.health().dump(),"application/json");});
        server_.Get("/metrics",[this](const auto&,auto& res){res.set_content(service_.metrics().dump(),"application/json");});
        server_.Post("/v1/audio/transcriptions",[this](const httplib::Request& req,httplib::Response& res){
            auto error=[&](int status,std::string msg){res.status=status;res.set_content(Json({{"error",msg}}).dump(),"application/json");};
            if(!ready_){error(503,"Model is not ready");return;}
            auto field=[&](const char* name,std::string fallback=""){return req.has_file(name)?req.get_file_value(name).content:req.has_param(name)?req.get_param_value(name):fallback;};
            auto format=field("response_format","json");if(format!="json"&&format!="text"){error(400,"unsupported response_format: "+format);return;}
            if(!req.has_file("file")){error(422,"Missing audio file");return;}
            auto file=req.get_file_value("file");if(file.content.empty()){error(400,"Empty audio file");return;}
            wchar_t dir[MAX_PATH],tmp[MAX_PATH];if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"lst",0,tmp)){error(500,"Cannot create temporary audio file");return;}
            struct Temp{fs::path p;~Temp(){std::error_code ec;fs::remove(p,ec);}} temp{tmp};
            try{ {std::ofstream f(temp.p,std::ios::binary|std::ios::trunc);f.write(file.content.data(),file.content.size());if(!f)throw std::runtime_error("Cannot save upload");}
                auto audio=decode_audio(temp.p);if(audio.empty()){error(400,"Audio contains no samples");return;}
                auto r=service_.transcribe(audio,field("language"));
                res.set_content(format=="text"?r.json["text"].get<std::string>():r.json.dump(),format=="text"?"text/plain; charset=utf-8":"application/json");
            }catch(const std::exception&e){error(400,e.what());}
        });
        server_.set_exception_handler([](const auto&,auto& res,std::exception_ptr e){try{if(e)std::rethrow_exception(e);}catch(const std::exception& ex){log(ex.what());}res.status=500;res.set_content("{\"error\":\"Internal service error\"}","application/json");});
    }
    bool start(const Json& cfg){if(!server_.bind_to_port(cfg.value("api_host","127.0.0.1"),cfg.value("api_port",7777)))return false;thread_=std::thread([this]{server_.listen_after_bind();});return true;}
    ~Api(){server_.stop();if(thread_.joinable())thread_.join();}
};
struct Session {
    Recorder recorder;std::atomic<bool> stop=false,cancel=false,done=false,processing=false;std::mutex mutex;std::condition_variable changed;
    std::unique_ptr<LiveDictation> live=std::make_unique<LiveDictation>();std::thread worker;Target target;Json config;std::string mode;bool focus_lost=false,mode_ready=false;
    ~Session(){if(worker.joinable())worker.join();}
    void finish(bool cancelled){if(cancelled)cancel=true;recorder.stop();{std::lock_guard lock(mutex);stop=true;}if(live){if(cancelled)live->cancel();else live->stop();}changed.notify_all();}
};
class App {
    Config& cfg_;Service service_;Commands commands_;std::atomic<bool> ready_=false;Api api_;Ui ui_;
    std::shared_ptr<Session> session_;std::future<void> init_,maintenance_;std::atomic<bool> quitting_=false;
    std::mutex status_mutex_,text_mutex_;Json status_={{"state","loading"},{"ready",false}};std::wstring last_;
    std::optional<std::string> matched_;KeyChord seen_;std::optional<Release> update_;bool bindings_logged_=false;
    bool pending_=false,started_press_=false;std::chrono::steady_clock::time_point pressed_;std::string pending_mode_;Target pending_target_{};bool restart_=false;
    void state(std::string value){std::lock_guard lock(status_mutex_);status_["state"]=value;status_["ready"]=ready_.load();}
    // Windows already writes "LocalSTT" across the top of the toast, so a title would
    // only repeat it. Errors keep one, because it changes how the line below is read.
    void notify(std::string text){ui_.notify("",std::move(text));}
    void notify_error(std::string text){ui_.notify("Error",std::move(text));}
    void check_updates(bool forced){
        auto cfg=cfg_.snapshot();
        if(!forced&&(!cfg.value("update_check_enabled",true)||!check_due(cfg,cfg_.state_dir,unix_now())))return;
        auto release=check_for_update(cfg,cfg_.state_dir);
        {std::lock_guard lock(status_mutex_);if(release)status_["update"]={{"version",release->version},{"url",release->url},{"published",release->published},{"label",release->label()}};else if(forced)status_.erase("update");}
        // Only the automatic check announces itself; "Check now" answers in Settings.
        if(release&&!forced)notify(release->label()+". Open the tray menu to get it.");
    }
    void cancel_session(){if(session_&&!session_->done&&!session_->cancel){session_->finish(true);notify("Recording cancelled");}}
    void log_bindings(const char* headline,const Json& cfg){
        log(std::string(headline)+": "+chord_label(cfg.value("hotkey_dictation",std::string()))+"=dictation, "+chord_label(cfg.value("hotkey_cleanup",std::string()))+"=cleanup, "+chord_label(cfg.value("hotkey_command",std::string()))+"=command; any of them stops the active recording, "+chord_label(bindings.cancel)+" cancels it");
    }
    void remember(std::wstring text){std::lock_guard lock(text_mutex_);last_=std::move(text);std::ofstream f(cfg_.state_dir/L"last-transcript.txt",std::ios::binary|std::ios::trunc);f<<utf8(last_);}
    void deliver(Target target,std::wstring text,const Json& cfg,std::atomic<bool>* cancelled=nullptr){
        for(int i=0;i<100&&(modifiers_down()||chord_held());++i){if(cancelled&&*cancelled)return;Sleep(20);}
        if(cancelled&&*cancelled)return;
        bool sent=false;
        // Paste borrows the clipboard and gives the old contents back unless the
        // transcript is meant to stay there; typing touches it only in that case.
        const bool keep=cfg.value("copy_to_clipboard",false);
        if(target_matches(target)&&!modifiers_down()&&!chord_held()){
            if(cfg.value("delivery_method","typewrite")=="paste")sent=paste_text(target,text,!keep,cfg.value("paste_restore_delay_seconds",0.8));
            else {size_t pos=0;while(pos<text.size()&&(!cancelled||!cancelled->load())){auto n=std::min(size_t(8),text.size()-pos);if(n&&text[pos+n-1]>=0xd800&&text[pos+n-1]<=0xdbff&&pos+n<text.size())++n;auto count=type_text(target,std::wstring_view(text).substr(pos,n),cfg.value("typewrite_interval_seconds",.001));pos+=count;if(count<n)break;}sent=pos==text.size();if(sent&&keep)clipboard_text(text);}
        }
        if(!sent&&(!cancelled||!cancelled->load())){clipboard_text(text);notify("Focus changed or input blocked. Transcript copied to clipboard.");}
    }
    CommandOutcome app_command(const Json& command,const std::string&,Target target){
        auto action=command.value("action","");auto name=command.value("name","");auto value=command.value("value","");
        if(action=="language"){auto allowed=cfg_.snapshot()["allowed_languages"];if(std::find(allowed.begin(),allowed.end(),value)==allowed.end())return {false,"Language not allowed",name};cfg_.update({{"language",value}});return {true,"Language: "+value,name};}
        if(action=="delivery"&&(value=="paste"||value=="typewrite")){cfg_.update({{"delivery_method",value}});return {true,"Delivery: "+value,name};}
        if(action=="repeat"){std::wstring text;{std::lock_guard lock(text_mutex_);text=last_;}deliver(target,text,cfg_.snapshot());return {!text.empty(),text.empty()?"No previous text":"Repeated last text",name};}
        return {false,"Unsupported LocalSTT action",name};
    }
    void run_session(std::shared_ptr<Session> s){
        try{
            Result result;
            if(s->mode=="dictation"&&s->config.value("live_dictation",false)){
                double processing=0;Json metadata=service_.engine.health();
                LiveIO io;
                io.capture=[s](double offset,double maximum){auto samples=s->recorder.snapshot(offset,maximum);bool tail=samples.size()<size_t(maximum*16000);return AudioWindow{std::move(samples),16000,s->recorder.tail_silence(s->config.value("command_silence_rms",.008),1)>=.65,tail};};
                std::string live_language=s->config.value("language","auto");
                io.recognize=[&,s](const AudioWindow& a){
                    const auto& committed=s->live->text();
                    size_t begin=committed.size()>2000?committed.size()-2000:0;
                    if(begin<committed.size()&&committed[begin]>=0xDC00&&committed[begin]<=0xDFFF)++begin;
                    auto context=utf8(std::wstring_view(committed).substr(begin));
                    auto r=service_.transcribe(a.mono,live_language,1,true,false,&s->cancel,context);
                    if((live_language.empty()||live_language=="auto")&&!r.hypothesis.text.empty())live_language=r.json.value("language",live_language);
                    processing+=r.json.value("processing_time",0.0);metadata=r.json;return r.hypothesis;
                };
                io.correct=[this](std::wstring text){return service_.engine.correct(std::move(text));};
                io.deliver=[s](std::wstring_view text)->size_t{if(s->cancel)return 0;if(!target_matches(s->target)){s->focus_lost=true;return 0;}if(s->focus_lost||modifiers_down()||chord_held())return 0;
                    auto n=std::min(size_t(16),text.size());if(n<text.size()&&text[n-1]>=0xd800&&text[n-1]<=0xdbff)++n;return type_text(s->target,text.substr(0,n),s->config.value("typewrite_interval_seconds",.001));};
                s->live->run(io);if(s->cancel)return;
                result.json=metadata;result.hypothesis.text=s->live->text();result.json["text"]=utf8(result.hypothesis.text);result.json["duration"]=s->recorder.duration();result.json["processing_time"]=processing;
                if(s->live->delivered()<result.hypothesis.text.size()){
                    if(s->focus_lost){clipboard_text(result.hypothesis.text);notify("Focus changed. Full transcript copied to clipboard.");}
                    else deliver(s->target,result.hypothesis.text.substr(s->live->delivered()),s->config,&s->cancel);
                }else if(s->config.value("copy_to_clipboard",false)&&!result.hypothesis.text.empty())clipboard_text(result.hypothesis.text);
            }else{
                std::optional<CommandMatch> match;
                if(s->mode=="command"&&s->config.value("command_auto_stop",true)){
                    while(!s->stop&&!s->cancel){
                        std::unique_lock lock(s->mutex);s->changed.wait_for(lock,std::chrono::duration<double>(s->config.value("command_poll_seconds",.9)),[&]{return s->stop.load()||s->cancel.load();});lock.unlock();if(s->stop||s->cancel)break;
                        double duration=s->recorder.duration();double silence=s->recorder.tail_silence(s->config.value("command_silence_rms",.008),30);
                        if(duration>=s->config.value("command_min_audio_seconds",.7)){
                            result=service_.transcribe(s->recorder.snapshot(),s->config.value("language","auto"),1,false,false,&s->cancel);if(s->cancel)break;
                            match=commands_.match(result.json.value("text",""));
                            if(match&&(match->capture.empty()||silence>=s->config.value("command_capture_silence_seconds",.9))){s->finish(false);break;}match.reset();
                        }
                        if(duration>=s->config.value("command_listen_timeout_seconds",15.0)||silence>=s->config.value("command_silence_timeout_seconds",2.5)){s->finish(false);break;}
                    }
                }else{std::unique_lock lock(s->mutex);s->changed.wait(lock,[&]{return s->stop.load()||s->cancel.load();});}
                if(s->cancel)return;s->processing=true;state("transcribing");
                if(!match)result=service_.transcribe(s->recorder.snapshot(),s->config.value("language","auto"),0,false,false,&s->cancel);
                if(s->cancel)return;
                if(s->mode=="command"){
                    if(!match)match=commands_.match(result.json.value("text",""));
                    if(match){auto out=commands_.execute(result.json.value("text",""),*match,[&](const Json& c,const std::string& capture){return app_command(c,capture,s->target);},[&]{return s->cancel.load();});notify(out.message);}
                    else {append_json(cfg_.state_dir/L"command-history.jsonl",{{"ts",timestamp()},{"text",result.json.value("text","")},{"matched",nullptr},{"executed",false},{"message","No command matched"}});notify("No command matched");}
                    return;
                }
                if(s->mode=="cleanup"){state("cleanup");auto text=service_.cleanup(result.json.value("text",""));result.json["text"]=text;result.hypothesis.text=wide(text);}
                if(s->cancel)return;deliver(s->target,result.hypothesis.text,s->config,&s->cancel);
            }
            if(!s->cancel){remember(result.hypothesis.text);service_.record(result);}
        }catch(const std::exception&e){log(e.what());notify_error(e.what());}
    }
    void start(std::string mode,Target target,bool grace=false){
        if(!ready_){notify("Model is not ready yet");return;}if(session_&&!session_->done){notify("Finish or cancel the current recording first");return;}
        if(session_){if(session_->worker.joinable())session_->worker.join();session_.reset();}auto cfg=cfg_.snapshot();if(mode=="command"&&!cfg.value("commands_enabled",true)){notify("Voice commands are disabled");return;}
        auto s=std::make_shared<Session>();s->mode=mode;s->config=cfg;s->target=target;
        try{s->recorder.start(cfg);}catch(const std::exception&e){notify_error(e.what());return;}
        session_=s;state(mode=="command"?"command":"recording");notify("Recording started ("+mode+")");
        s->worker=std::thread([this,s,grace]{
            {std::unique_lock lock(s->mutex);if(grace)s->changed.wait_for(lock,std::chrono::duration<double>(s->config.value("hotkey_mode_grace_seconds",.2)),[&]{return s->stop.load()||s->cancel.load();});s->mode_ready=true;}
            if(!s->cancel)run_session(s);try{s->recorder.stop();}catch(const std::exception&e){log(e.what());notify_error(e.what());}s->done=true;state("ready");});
    }
    void maintain(std::string action){
        if(maintenance_.valid()&&maintenance_.wait_for(0s)!=std::future_status::ready){notify("Maintenance task is already running");return;}
        maintenance_=std::async(std::launch::async,[this,action]{try{
            if(action=="selftest"){service_.preflight();notify("Self-test saved");}
            if(action=="rebuild-index"){commands_.rebuild_index();notify("Application index rebuilt");}
            if(action=="pull-model"){service_.pull_cleanup();notify("Cleanup model downloaded");}
            if(action=="check-updates"){check_updates(true);ui_.refresh_settings();}
            auto commands=commands_.statuses();auto rec=service_.cleanup_recommendation();std::lock_guard lock(status_mutex_);status_["commands"]=commands;status_["cleanup_recommendation"]=rec;
        }catch(const std::exception&e){notify_error(e.what());log(e.what());}});
    }
    void action(std::string action){
        if(action=="start"||action=="cleanup"||action=="command")start(action=="start"?"dictation":action,capture_target());
        else if(action=="cancel")cancel_session();
        else if(action=="stop"){if(session_&&!session_->done)session_->finish(false);}
        else if(action=="repeat") {if(session_&&!session_->done)return;auto target=capture_target();std::wstring text;{std::lock_guard lock(text_mutex_);text=last_;}if(!text.empty()){if(maintenance_.valid()&&maintenance_.wait_for(0s)!=std::future_status::ready)return;maintenance_=std::async(std::launch::async,[this,target,text]{deliver(target,text,cfg_.snapshot());});}}
        else if(action=="quit"||action=="restart"){quitting_=true;if(action=="restart")restart_=true;if(session_&&!session_->done)session_->finish(true);if(action=="restart")PostMessageW(FindWindowW(L"LocalSTT.Native.Host",nullptr),WM_CLOSE,0,0);}
        else maintain(action);
    }
    void tick(){
        if(quitting_)return;if(session_&&session_->done){if(session_->worker.joinable())session_->worker.join();session_.reset();}
        auto cfg=cfg_.snapshot();
        // Bindings follow config.json on every tick, so an edit applies without a restart.
        if(auto next=Bindings::from_config(cfg);!(next==bindings)||!bindings_logged_){
            bindings=std::move(next);auto chord=bindings.keys();for(unsigned i=0;i<chord_keys.size();++i){auto name=key_name(i);chord_keys[i]=name&&chord.contains(*name);}
            log_bindings(bindings_logged_?"hotkeys rebound":"global hotkeys ready",cfg);bindings_logged_=true;
        }
        // A settings field is recording a chord; nothing pressed meanwhile is a hotkey.
        if(capture_window()){matched_.reset();started_press_=false;cancel_requested=false;return;}
        if(cancel_requested.exchange(false)){pending_=false;cancel_session();}
        auto keys_held=held();auto matched=bindings.match(keys_held);auto now=std::chrono::steady_clock::now();
        // Only a freshly completed chord starts or stops a recording. Keys pressed
        // afterwards (the Shift that turns dictation into cleanup) extend the chord
        // already held and do not count as a second press.
        if(matched&&!matched_){pressed_=now;started_press_=false;if(session_&&!session_->stop){session_->finish(false);pending_=false;}else if(!session_){seen_=keys_held;start(*matched,capture_target(),true);started_press_=true;}}
        // The mode is whatever the keys seen during the grace period complete.
        if(matched&&started_press_&&session_){seen_.insert(keys_held.begin(),keys_held.end());std::lock_guard lock(session_->mutex);if(!session_->mode_ready)session_->mode=bindings.match(seen_).value_or("dictation");}
        if(!matched&&matched_&&started_press_&&session_&&!session_->stop&&std::chrono::duration<double>(now-pressed_).count()>=cfg.value("hotkey_tap_seconds",.45))session_->finish(false);
        matched_=matched;
    }
public:
    App(Config& c):cfg_(c),service_(c),commands_(c),api_(service_,ready_),ui_(c,{[this]{tick();},[this](std::string a){action(std::move(a));},[this]{std::lock_guard lock(status_mutex_);return status_;}}){}
    int run(){
        std::ifstream last(cfg_.state_dir/L"last-transcript.txt",std::ios::binary);if(last){std::string text((std::istreambuf_iterator<char>(last)),{});last_=wide(text);}
        init_=std::async(std::launch::async,[this]{try{service_.engine.load();ready_=true;auto health=service_.engine.health();auto devices=Recorder::devices();auto commands=commands_.statuses();auto rec=service_.cleanup_recommendation();{
                std::lock_guard lock(status_mutex_);status_=health;status_["ready"]=true;status_["state"]="ready";status_["microphones"]=devices;status_["commands"]=commands;status_["cleanup_recommendation"]=rec;
            }if(!api_.start(cfg_.snapshot()))notify("API port is busy. Dictation is available; choose a free port in Settings.");
            service_.preflight();auto dictate=parse_chord(cfg_.snapshot().value("hotkey_dictation",std::string()));notify(dictate.empty()?"Ready":"Ready: "+chord_label(dictate)+" to dictate");
            // After the app is usable, never before: a slow network must not delay the
            // first dictation, and being offline must not look like a failed start.
            try{check_updates(false);}catch(const std::exception&e){log(e.what());}
        }catch(const std::exception&e){state("error");notify_error(e.what());log(e.what());}});
        for(size_t i=0;i<keys.size();++i)keys[i]=(GetAsyncKeyState(int(i))&0x8000)!=0;
        keys[VK_CONTROL]=false;keys[VK_SHIFT]=false;keys[VK_MENU]=false;
        HHOOK hook=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard,GetModuleHandleW(nullptr),0);if(!hook)notify_error("Global hotkeys unavailable; use tray controls");
        int code=ui_.run();if(hook)UnhookWindowsHookEx(hook);quitting_=true;service_.shutdown();if(session_){session_->finish(true);if(session_->worker.joinable())session_->worker.join();session_.reset();}if(init_.valid())init_.wait();if(maintenance_.valid())maintenance_.wait();return restart_?42:code;
    }
};
fs::path default_state(){PWSTR p=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,0,nullptr,&p)))throw std::runtime_error("Cannot find AppData");fs::path path=fs::path(p)/L"LocalSTT";CoTaskMemFree(p);return path;}
int entry(int argc,wchar_t** argv){
    std::map<std::wstring,std::wstring> options;for(int i=1;i<argc;++i){std::wstring key=argv[i];if(!key.starts_with(L"--"))throw std::runtime_error("Unknown argument");options[key]=(i+1<argc&&!std::wstring(argv[i+1]).starts_with(L"--"))?argv[++i]:L"";}
    if(options.contains(L"--help")){std::cout<<"LocalSTT native\n--transcribe FILE [--language ru|en|auto] [--output JSON] [--cleanup]\n--serve [--port 7777] [--serve-seconds N]\n--self-test | --devices | --microphone-test | --version\n--cleanup-model [NAME] [--pull] [--apply] [--timeout SECONDS]\n--state-dir DIR --root DIR --model-path GGML_FILE --beam-size N\nWithout options: tray application.\n";return 0;}
    if(options.contains(L"--version")){std::cout<<"LocalSTT "<<version<<" (native, CTranslate2 4.8.2; no Python)\n";return 0;}
    fs::path state=options.contains(L"--state-dir")?fs::path(options[L"--state-dir"]):default_state();
    fs::path root=options.contains(L"--root")?fs::path(options[L"--root"]):executable_dir();fs::create_directories(state);set_log_dir(state/L"logs");Config cfg(state,root);
    Json override=Json::object();if(options.contains(L"--port"))override["api_port"]=std::stoi(options[L"--port"]);if(options.contains(L"--language"))override["language"]=utf8(options[L"--language"]);if(options.contains(L"--model-path"))override["native_model_path"]=utf8(options[L"--model-path"]);if(options.contains(L"--beam-size"))override["beam_size"]=std::stoi(options[L"--beam-size"]);if(!override.empty())cfg.update(override);
    if(options.contains(L"--devices")){std::cout<<Recorder::devices().dump(2)<<'\n';return 0;}
    if(options.contains(L"--microphone-test")){Recorder recorder;recorder.start(cfg.snapshot());Sleep(500);recorder.stop();double seconds=recorder.duration();std::cout<<Json({{"microphone_ok",seconds>.1},{"captured_seconds",seconds}}).dump()<<'\n';return seconds>.1?0:1;}
    if(options.contains(L"--transcribe")||options.contains(L"--serve")||options.contains(L"--self-test")||options.contains(L"--cleanup-model")){
        Service s(cfg);s.engine.load();
        if(options.contains(L"--cleanup-model")){auto rec=s.cleanup_recommendation();std::string model=utf8(options[L"--cleanup-model"]);if(model.empty())model=rec["best_installed"].is_string()?rec["best_installed"].get<std::string>():rec.value("pull","");if(model.empty())throw std::runtime_error("No cleanup model could be selected");if(options.contains(L"--pull")||options.contains(L"--apply")){Json change={{"ollama_model",model}};if(options.contains(L"--timeout"))change["ollama_timeout_seconds"]=std::stod(options[L"--timeout"]);if(options.contains(L"--pull"))s.pull_cleanup(model);cfg.update(change);}rec["selected"]=model;std::cout<<rec.dump(2)<<'\n';return 0;}
        if(options.contains(L"--transcribe")){auto r=s.transcribe(decode_audio(options[L"--transcribe"]));if(options.contains(L"--cleanup"))r.json["text"]=s.cleanup(r.json.value("text",""));auto text=r.json.dump(2);if(options.contains(L"--output")){std::ofstream f(fs::path(options[L"--output"]),std::ios::binary);f<<text;if(!f)throw std::runtime_error("Cannot write output");}std::cout<<text<<'\n';return 0;}
        if(options.contains(L"--self-test")){auto r=s.preflight();Commands commands(cfg);r["commands"]=commands.statuses();write_json(state/L"preflight.json",r);std::cout<<r.dump(2)<<'\n';return r.value("ok",false)?0:1;}
        std::atomic<bool> ready=true;Api api(s,ready);if(!api.start(cfg.snapshot()))throw std::runtime_error("API port is busy");std::cout<<s.engine.health().dump()<<std::endl;
        int seconds=options.contains(L"--serve-seconds")?std::stoi(options[L"--serve-seconds"]):INT_MAX;for(int i=0;i<seconds;++i)Sleep(1000);return 0;
    }
    HANDLE singleton=CreateMutexW(nullptr,FALSE,L"LocalSTT.Native.Tray");if(!singleton)throw std::runtime_error("Cannot create app mutex");if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(singleton);return 0;}
    // Before any window exists: the shell reads the AppUserModelID as one is created.
    {auto icon=executable_dir()/L"localstt.ico";std::error_code ec;apply_shell_identity(fs::exists(icon,ec)?icon:state/L"localstt.ico");}
    int result;{App app(cfg);result=app.run();}CloseHandle(singleton);
    if(result==42){std::wstring exe=(executable_dir()/L"LocalSTT.exe").wstring();std::wstring args=L"--root \""+fs::absolute(root).wstring()+L"\" --state-dir \""+fs::absolute(state).wstring()+L"\"";ShellExecuteW(nullptr,L"open",exe.c_str(),args.c_str(),executable_dir().c_str(),SW_SHOWNORMAL);}return 0;
}
}
int wmain(int argc,wchar_t** argv){try{SetConsoleOutputCP(CP_UTF8);return entry(argc,argv);}catch(const std::exception&e){std::cerr<<e.what()<<'\n';log(e.what());return 1;}}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){int argc;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);int rc=0;try{rc=entry(argc,argv);}catch(const std::exception&e){log(e.what());MessageBoxW(nullptr,wide(e.what()).c_str(),L"LocalSTT",MB_ICONERROR);rc=1;}LocalFree(argv);return rc;}
