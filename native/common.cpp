#include "common.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace localstt {
namespace { std::mutex file_mutex; fs::path log_dir; }
std::wstring wide(std::string_view s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    if(!n) throw std::runtime_error("Invalid UTF-8");
    std::wstring r(n,0); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),r.data(),n); return r;
}
std::string utf8(std::wstring_view s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    std::string r(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr); return r;
}
std::wstring trim(std::wstring s) {
    auto first=s.find_first_not_of(L" \t\r\n"); if(first==s.npos) return {};
    return s.substr(first,s.find_last_not_of(L" \t\r\n")-first+1);
}
std::string trim(std::string_view s) {
    auto first=s.find_first_not_of(" \t\r\n"); if(first==s.npos) return {};
    return std::string(s.substr(first,s.find_last_not_of(" \t\r\n")-first+1));
}
std::string timestamp() { SYSTEMTIME t; GetLocalTime(&t); char b[40]; sprintf_s(b,"%04u-%02u-%02uT%02u:%02u:%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond); return b; }
fs::path executable_dir() { std::wstring s(32768,0); DWORD n=GetModuleFileNameW(nullptr,s.data(),DWORD(s.size())); if(!n||n>=s.size()) throw std::runtime_error("Cannot resolve executable path"); s.resize(n); return fs::path(s).parent_path(); }
Json read_json(const fs::path& p,const Json& fallback) {
    if(!fs::exists(p)) return fallback;
    std::ifstream f(p,std::ios::binary); if(!f) throw std::runtime_error("Cannot read "+utf8(p.wstring())); return Json::parse(f);
}
std::optional<std::string> read_text(const fs::path& p) {
    std::ifstream f(p,std::ios::binary); if(!f) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(f)),{}); if(f.bad()) return std::nullopt;
    if(text.starts_with("\xEF\xBB\xBF")) text.erase(0,3); return text;
}
void write_json(const fs::path& p,const Json& j) {
    std::lock_guard lock(file_mutex); fs::create_directories(p.parent_path());
    auto temp=p; temp+=L".tmp"; { std::ofstream f(temp,std::ios::binary|std::ios::trunc); f<<j.dump(2); if(!f) throw std::runtime_error("Cannot write JSON"); }
    if(!MoveFileExW(temp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace "+utf8(p.wstring()));
}
void append_json(const fs::path& p,const Json& j) { std::lock_guard lock(file_mutex); fs::create_directories(p.parent_path()); std::ofstream f(p,std::ios::binary|std::ios::app); f<<j.dump()<<'\n'; if(!f) throw std::runtime_error("Cannot append history"); }
void set_log_dir(const fs::path& p) { log_dir=p; fs::create_directories(p); }
void log(std::string_view text) { std::lock_guard lock(file_mutex); if(!log_dir.empty()) { std::ofstream f(log_dir/L"localstt-native.log",std::ios::binary|std::ios::app); f<<timestamp()<<" "<<text<<'\n'; } }
static Json defaults() { return {
    {"model","large-v3-turbo"},{"allowed_models",{"large-v3-turbo","large-v3","medium"}},
    {"language","auto"},{"allowed_languages",{"ru","en","auto"}},{"device","cuda"},{"compute_type","int8_float16"},
    {"vad_filter",true},{"beam_size",5},{"api_host","127.0.0.1"},{"api_port",7777},
    {"microphone",nullptr},{"sample_rate",16000},{"channels",1},{"input_normalize_peak",0.35},{"input_max_gain",25.0},
    {"history_enabled",false},{"performance_tracking",true},
    {"update_check_enabled",true},{"update_repository","iFrosta/localSTT"},{"update_check_interval_hours",24.0},{"ollama_base_url","http://127.0.0.1:11434"},{"ollama_model",nullptr},
    {"preferred_ollama_cleanup_models",{"qwen3:4b-instruct","qwen3:4b","qwen3:1.7b","qwen2.5:7b-instruct","qwen2.5:3b-instruct"}},
    {"ollama_timeout_seconds",20.0},{"paste_restore_delay_seconds",0.8},{"copy_to_clipboard",false},
    {"live_dictation",false},{"delivery_method","typewrite"},{"typewrite_interval_seconds",0.001},
    {"hotkey_dictation","ctrl+win"},{"hotkey_cleanup","ctrl+shift+win"},{"hotkey_command","ctrl+alt+win"},{"hotkey_cancel","esc"},
    {"hotkey_tap_seconds",0.45},{"hotkey_mode_grace_seconds",0.2},{"cancel_on_escape",true},{"commands_enabled",true},
    {"command_auto_stop",true},{"command_poll_seconds",0.9},{"command_min_audio_seconds",0.7},
    {"command_listen_timeout_seconds",15.0},{"command_silence_timeout_seconds",2.5},
    {"command_capture_silence_seconds",0.9},{"command_silence_rms",0.008},
    {"native_backend","ctranslate2"},{"native_ct2_model_path",""},{"native_model_path",""},{"native_vad_path",""},{"native_microphone",nullptr}
}; }
static void validate(const Json& j) {
    const auto d=defaults();
    for(auto it=d.begin();it!=d.end();++it) {
        if(it->is_null()) continue;
        const auto& v=j.at(it.key());
        if(it->is_number()) { if(!v.is_number()||!std::isfinite(v.get<double>())) throw std::runtime_error("Invalid number: "+it.key()); }
        else if(v.type()!=it->type()) throw std::runtime_error("Invalid setting: "+it.key());
    }
    if(!j["microphone"].is_null() && (!j["microphone"].is_number_integer()||j["microphone"].get<int>()<0)) throw std::runtime_error("Invalid microphone");
    if(!j["native_microphone"].is_null() && (!j["native_microphone"].is_number_integer()||j["native_microphone"].get<int>()<0)) throw std::runtime_error("Invalid native microphone");
    for(const char* key:{"api_port","beam_size","sample_rate","channels"})if(!j[key].is_number_integer())throw std::runtime_error(std::string("Expected integer: ")+key);
    if(!j["ollama_model"].is_null()&&!j["ollama_model"].is_string()) throw std::runtime_error("Invalid Ollama model");
    for(const char* key:{"allowed_models","allowed_languages","preferred_ollama_cleanup_models"}) for(const auto& v:j[key]) if(!v.is_string()) throw std::runtime_error("Expected string list");
    if(j["update_check_interval_hours"].get<double>()<0) throw std::runtime_error("Out of range: update_check_interval_hours");
    if(j["api_port"].get<int>()<1||j["api_port"].get<int>()>65535) throw std::runtime_error("Port must be 1..65535");
    if(j["sample_rate"]!=16000||j["channels"]!=1) throw std::runtime_error("Native capture requires 16000 Hz mono");
    if(j["beam_size"].get<int>()<1||j["beam_size"].get<int>()>20) throw std::runtime_error("Beam size must be 1..20");
    if(j["device"]!="cuda" && j["device"]!="cpu") throw std::runtime_error("Device must be cuda or cpu");
    if(j["native_backend"]!="ctranslate2"&&j["native_backend"]!="whisper.cpp")throw std::runtime_error("Unknown native recognition backend");
    if(j["delivery_method"]!="paste"&&j["delivery_method"]!="typewrite") throw std::runtime_error("Invalid delivery method");
    for(const char* key:{"command_poll_seconds","command_min_audio_seconds","command_listen_timeout_seconds","command_silence_timeout_seconds","command_capture_silence_seconds","ollama_timeout_seconds","hotkey_tap_seconds","input_max_gain"})
        if(j[key].get<double>()<=0||j[key].get<double>()>3600) throw std::runtime_error(std::string("Out of range: ")+key);
    for(const char* key:{"hotkey_mode_grace_seconds","typewrite_interval_seconds","paste_restore_delay_seconds","command_silence_rms","input_normalize_peak"})
        if(j[key].get<double>()<0||j[key].get<double>()>10) throw std::runtime_error(std::string("Out of range: ")+key);
}
Config::Config(fs::path state,fs::path root):state_dir(std::move(state)),install_dir(std::move(root)) { reload(); }
Json Config::snapshot() const { std::lock_guard lock(mutex_); return data_; }
void Config::reload() { std::lock_guard lock(mutex_); auto next=defaults(); auto saved=read_json(state_dir/L"config.json"); if(!saved.is_object()) throw std::runtime_error("config.json must be an object"); next.update(saved); validate(next); data_=next; }
fs::path Config::user_file(std::wstring_view name) const { auto own=state_dir/name; std::error_code ec; return fs::exists(own,ec)?own:install_dir/name; }
fs::path Config::editable_file(std::wstring_view name) const {
    auto current=user_file(name);
    // Opening for writing is the only reliable test: ACLs, read-only flags and
    // Program Files virtualisation all answer it.
    if(HANDLE h=CreateFileW(current.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);h!=INVALID_HANDLE_VALUE){CloseHandle(h);return current;}
    auto own=state_dir/name; fs::create_directories(state_dir); std::error_code ec;
    if(!fs::exists(own,ec)&&fs::exists(current,ec)) fs::copy_file(current,own,fs::copy_options::skip_existing);
    return own;
}
void Config::update(const Json& values) { std::lock_guard lock(mutex_); auto next=data_; next.update(values); validate(next); write_json(state_dir/L"config.json",next); data_=std::move(next); }
}
