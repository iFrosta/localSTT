#include "ui.hpp"
#include "commands.hpp"
#include "hotkeys.hpp"
#include "updates.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <fstream>
#include <sstream>
#include <map>
#include <deque>
#include <algorithm>
#include <cmath>
#include <optional>
#include <atomic>
#include <iomanip>
#include <gdiplus.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace localstt {
namespace {
constexpr UINT tray_message = WM_APP + 31, notification_message = WM_APP + 32, refresh_message = WM_APP + 33;
constexpr COLORREF background = RGB(32,32,32), foreground = RGB(255,255,255), field_background = RGB(32,32,32);
constexpr COLORREF surface=RGB(44,44,44), card_color=RGB(39,39,39), divider=RGB(58,58,58), secondary=RGB(199,199,199), muted=RGB(122,122,122), hover_color=RGB(56,56,56);
COLORREF accent_color() { DWORD value=0,size=sizeof(value);return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\DWM",L"AccentColor",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS?value&0xffffff:RGB(0,120,212); }
Gdiplus::Color color(COLORREF c) {return Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c));}
void rounded(HDC dc,RECT rect,float radius,COLORREF fill,std::optional<COLORREF> border={}) {
    Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::GraphicsPath path;
    float x=static_cast<float>(rect.left)+.5f,y=static_cast<float>(rect.top)+.5f,w=static_cast<float>(rect.right-rect.left)-1,h=static_cast<float>(rect.bottom-rect.top)-1,d=std::min({radius*2,w,h});
    if(w<=0||h<=0)return;if(d<=0)path.AddRectangle(Gdiplus::RectF(x,y,w,h));else{path.AddArc(x,y,d,d,180,90);path.AddArc(x+w-d,y,d,d,270,90);path.AddArc(x+w-d,y+h-d,d,d,0,90);path.AddArc(x,y+h-d,d,d,90,90);path.CloseFigure();}
    Gdiplus::SolidBrush brush(color(fill));g.FillPath(&brush,&path);if(border){Gdiplus::Pen pen(color(*border),1);g.DrawPath(&pen,&path);}
}
void text_at(HDC dc,HFONT font,COLORREF c,std::wstring_view text,RECT rect,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS) {
    auto previous=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,c);DrawTextW(dc,text.data(),static_cast<int>(text.size()),&rect,flags|DT_NOPREFIX);SelectObject(dc,previous);
}
HICON microphone_icon(COLORREF color) {
    // The same 64-unit mark and 1.29 glyph scale as branding.py, supersampled.
    Gdiplus::Bitmap bitmap(256,256,PixelFormat32bppARGB);Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.Clear(Gdiplus::Color(0,0,0,0));
    auto coordinate=[](float value){return ((value-32.f)*1.29f+32.f)*4.f;};
    Gdiplus::SolidBrush fill(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color))),white(Gdiplus::Color::White);
    Gdiplus::Pen outline(Gdiplus::Color(255,32,32,32),3.f*1.29f*4.f),arc(Gdiplus::Color::White,4.f*1.29f*4.f);
    float start=coordinate(8),diameter=coordinate(56)-start;g.FillEllipse(&fill,start,start,diameter,diameter);g.DrawEllipse(&outline,start,start,diameter,diameter);
    g.FillRectangle(&white,coordinate(29),coordinate(18),coordinate(35)-coordinate(29),coordinate(43)-coordinate(18));
    g.DrawArc(&arc,coordinate(21),coordinate(30),coordinate(43)-coordinate(21),coordinate(52)-coordinate(30),0.f,180.f);
    HICON result=nullptr;bitmap.GetHICON(&result);return result;
}
enum : int { nav_id=100, save_id=101, close_id=102, refresh_id=103, startup_id=104,
    selftest_id=110, rebuild_id=111, commands_file_id=112, aliases_id=113, dictionary_id=114,
    history_file_id=115, config_file_id=116, logs_id=117, pull_id=118, toggle_command_id=119,
    command_list_id=120, history_clear_id=121, command_history_id=122, report_id=123,
    start_id=210, cleanup_id=211, command_id=212, stop_id=213, cancel_id=214, repeat_id=215,
    settings_id=216, restart_id=217, quit_id=218, live_id=219, russian_id=220, english_id=221, auto_id=222,
    typewrite_id=223, paste_id=224, copy_clipboard_id=225, update_id=226,
    check_updates_id=124, get_update_id=125, prompt_id=126, version_link_id=127, field_id=1000 };
struct Field {
    const char* key; const wchar_t* label; const char* kind;
    double minimum=0, maximum=1e9;
    const wchar_t* options=L"";
    const wchar_t* description=L"";
};
enum Section : int { general_section, audio_section, hotkeys_section, delivery_section, commands_section, cleanup_section,
    history_section, performance_section, api_section, health_section, advanced_section };
const std::vector<std::wstring> section_names = {L"General",L"Audio",L"Hotkeys",L"Delivery",L"Voice commands",L"AI cleanup",L"History",L"Performance",L"API",L"Health",L"Advanced"};
const std::vector<wchar_t> section_icons={0xe713,0xe720,0xe8d7,0xe765,0xe90a,0xe945,0xe81c,0xe916,0xe774,0xe95e,0xe115};
const std::vector<std::vector<Field>> fields = {
    {{"model",L"Whisper model","choice",0,0,L"large-v3-turbo|large-v3|medium",L"Larger models are more accurate and need more VRAM. Restart required."},{"compute_type",L"Compute type","choice",0,0,L"float16|int8_float16|int8|float32",L"int8_float16 halves the memory the weights take. Restart required."},{"language",L"Language","choice",0,0,L"ru|en|auto",L"Language Whisper is told to expect."},{"beam_size",L"Beam size","int",1,20,L"",L"Higher is slightly more accurate and slower."},{"vad_filter",L"Voice activity filter","bool",0,0,L"",L"Drops silence before transcription."},{"update_check_enabled",L"Check for updates","bool",0,0,L"",L"Asks GitHub once a day whether a newer release exists. One request, and nothing about this machine is sent, but GitHub does see that it was made."}},
    {{"native_microphone",L"Microphone","microphone",-1,65535,L"",L"Choose an input device, or use the Windows default."},{"sample_rate",L"Sample rate","int",16000,16000,L"",L"Whisper uses 16000 Hz mono audio."},{"input_normalize_peak",L"Normalise peak","number",0,1,L"",L"Quiet microphones are amplified up to this peak. 0 disables it."},{"input_max_gain",L"Maximum gain","number",1,100,L"",L"Ceiling for that amplification."}},
    {{"hotkey_dictation",L"Dictation","hotkey",0,0,L"",L"Hold to talk, or tap to start and tap again to stop. Whichever chord started a recording, any of them stops it."},{"hotkey_cleanup",L"Dictation with cleanup","hotkey",0,0,L"",L"Runs the transcript through the local Ollama model before delivering it."},{"hotkey_command",L"Voice command","hotkey",0,0,L"",L"Nothing is typed; what was said is matched against commands.json."},{"cancel_on_escape",L"A key cancels a recording","bool",0,0,L"",L"Nothing is transcribed, delivered or run."},{"hotkey_cancel",L"Cancel key","hotkey"},{"hotkey_tap_seconds",L"Tap threshold","number",0.1,3,L"",L"Holding the hotkey longer than this makes it push-to-talk."},{"hotkey_mode_grace_seconds",L"Chord grace period","number",0,1,L"",L"How long to wait for the rest of the chord before deciding the mode. A chord that extends another one has to be complete within this."}},
    {{"live_dictation",L"Type while speaking","bool",0,0,L"",L"Tap the dictation shortcut to start and again to finish. Text appears in small chunks. Applies immediately. Cancel stops future output; text already typed stays. AI cleanup waits until the end."},{"delivery_method",L"How text is delivered","choice",0,0,L"paste|typewrite",L"Paste is instant; typewrite works in fields that block paste."},{"copy_to_clipboard",L"Leave the transcript in the clipboard","bool",0,0,L"",L"Off by default: typing the text needs no clipboard, so whatever you had copied stays there. Paste delivery borrows the clipboard either way and puts the old contents back."},{"paste_restore_delay_seconds",L"Clipboard restore delay","number",0,10,L"",L"How long to wait before putting the previous clipboard back."},{"typewrite_interval_seconds",L"Typing interval","number",0,1,L"",L"Delay between characters, in seconds."}},
    {{"commands_enabled",L"Voice commands","bool"},{"command_auto_stop",L"Stop when a command is recognised","bool"},{"command_poll_seconds",L"Recognition interval","number",0.2,10},{"command_listen_timeout_seconds",L"Listening timeout","number",1,300},{"command_silence_timeout_seconds",L"Silence timeout","number",0.2,60},{"command_capture_silence_seconds",L"Silence before a capture ends","number",0.1,10},{"command_min_audio_seconds",L"Minimum audio length","number",0.1,10},{"command_silence_rms",L"Silence threshold (RMS)","number",0.00001,1}},
    {{"ollama_model",L"Cleanup model","nullable",0,0,L"",L"Runs on the same GPU as Whisper, so it has to fit in what is left. Empty selects automatically."},{"ollama_base_url",L"Ollama address","text"},{"ollama_timeout_seconds",L"Request timeout","number",1,600,L"",L"Maximum time to wait for cleanup, in seconds."}},
    {{"history_enabled",L"Keep a history of dictations","bool",0,0,L"",L"Records every transcript with its date and time. Off by default."}},
    {{"performance_tracking",L"Measure performance","bool",0,0,L"",L"Records how long the last dictation and cleanup took."}},
    {{"api_host",L"Host","text",0,0,L"",L"Restart required."},{"api_port",L"Port","int",1,65535,L"",L"OpenAI-compatible endpoint for other tools. Restart required."}},
    {},
    {{"native_backend",L"Recognition backend","choice",0,0,L"ctranslate2|whisper.cpp",L"CTranslate2 uses the original recognition engine. Restart required."},{"native_ct2_model_path",L"CTranslate2 model folder","text",0,0,L"",L"Leave empty to use the selected model. Restart required."},{"device",L"Compute device","choice",0,0,L"cuda|cpu",L"Restart required."},{"native_model_path",L"Model file override","text",0,0,L"",L"Leave empty to use the selected Whisper model. Restart required."},{"native_vad_path",L"Voice activity model override","text",0,0,L"",L"Leave empty to use the included Silero model."},{"allowed_models",L"Allowed models","array",0,0,L"",L"Comma-separated model names."},{"allowed_languages",L"Allowed languages","array",0,0,L"",L"Comma-separated language codes."},{"preferred_ollama_cleanup_models",L"Preferred cleanup models","array",0,0,L"",L"Tried in this order when cleanup model is automatic."},{"channels",L"Audio channels","int",1,1,L"",L"Native capture uses mono audio."}}
};
std::wstring window_text(HWND window) {
    int count = GetWindowTextLengthW(window);
    std::wstring result(count+1, L'\0');
    GetWindowTextW(window, result.data(), count+1); result.resize(count); return result;
}
std::wstring display_value(const Json& value) {
    if (value.is_null()) return {};
    if (value.is_string()) return wide(value.get<std::string>());
    if (value.is_array()) { std::wstring result; for (const auto& item:value) { if (!result.empty()) result += L", "; result += item.is_string()?wide(item.get<std::string>()):wide(item.dump()); } return result; }
    return wide(value.dump());
}
std::wstring file_text(const fs::path& file, size_t limit=512*1024) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return L"No records yet.";
    input.seekg(0,std::ios::end); const std::streamoff length=input.tellg();
    input.seekg(length>static_cast<std::streamoff>(limit)?length-static_cast<std::streamoff>(limit):std::streamoff(0));
    std::string content((std::istreambuf_iterator<char>(input)),{});
    if(length>static_cast<std::streamoff>(limit)) { auto p=content.find('\n'); if(p!=std::string::npos) content.erase(0,p+1); }
    std::wstring result=wide(content), normalized;
    for(auto c:result) { if(c==L'\n' && (normalized.empty()||normalized.back()!=L'\r')) normalized+=L'\r'; normalized+=c; }
    return normalized;
}
fs::path startup_path() {
    PWSTR folder=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup,0,nullptr,&folder))) throw std::runtime_error("Cannot locate Windows Startup folder");
    fs::path path=fs::path(folder)/L"LocalSTT.lnk"; CoTaskMemFree(folder); return path;
}
std::wstring quote_argument(std::wstring_view value) {
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){result.append(slashes*2+1,L'\\');result+=c;}else{result.append(slashes,L'\\');result+=c;}slashes=0;}
    result.append(slashes*2,L'\\');return result+L"\"";
}
void set_startup(bool enabled, const fs::path& state_dir, const fs::path& install_dir) {
    const auto path=startup_path();
    if (!enabled) { std::error_code error; fs::remove(path,error); if(error) throw std::runtime_error("Cannot remove Startup shortcut: "+error.message()); return; }
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
    IShellLinkW* link=nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))) throw std::runtime_error("Cannot create Startup shortcut");
    link->SetPath(exe); link->SetWorkingDirectory(executable_dir().c_str()); link->SetDescription(L"LocalSTT native dictation"); link->SetIconLocation(exe,0);
    const auto arguments=L"--root "+quote_argument(fs::absolute(install_dir).wstring())+L" --state-dir "+quote_argument(fs::absolute(state_dir).wstring());link->SetArguments(arguments.c_str());
    IPersistFile* persist=nullptr; HRESULT hr=link->QueryInterface(IID_PPV_ARGS(&persist));
    if(SUCCEEDED(hr)) {
        if(fs::exists(path)) { std::error_code error; fs::create_directories(state_dir,error); fs::copy_file(path,state_dir/L"autostart.previous.lnk",fs::copy_options::overwrite_existing,error); }
        hr=persist->Save(path.c_str(),TRUE); persist->Release();
    }
    link->Release(); if(FAILED(hr)) throw std::runtime_error("Cannot save Startup shortcut");
}
void open_file(HWND owner,const fs::path& path) {
    HINSTANCE result=ShellExecuteW(owner,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    if(reinterpret_cast<INT_PTR>(result)<=32) {
        std::wstring args=L"\""+path.wstring()+L"\"";
        ShellExecuteW(owner,L"open",L"notepad.exe",args.c_str(),nullptr,SW_SHOWNORMAL);
    }
}
}



struct Ui::Impl {
    Config& config; UiCallbacks callbacks;
    HWND host=nullptr,settings=nullptr,panel=nullptr,popup=nullptr;
    HFONT font=nullptr,small_font=nullptr,title_font=nullptr,section_font=nullptr,icon_font=nullptr;
    HBRUSH brush=CreateSolidBrush(background),field_brush=CreateSolidBrush(field_background);
    ULONG_PTR gdiplus=0; int dpi=96; COLORREF accent=accent_color();
    NOTIFYICONDATAW tray{}; UINT taskbar_created=0;
    HICON ready_icon=nullptr,recording_icon=nullptr,working_icon=nullptr;
    HWND last_external_window=nullptr,last_external_control=nullptr;
    ULONGLONG last_status_update=0; std::string last_state;
    int section=0,scroll=0,content_height=0,content_width=720; bool dragging_scroll=false; int scroll_drag_y=0,scroll_drag_value=0;
    Json pending=Json::object(); std::wstring footer_message;
    struct Binding { Field field; HWND control; std::vector<Json> choices; };
    struct Position { HWND handle; int x,y,width,height; };
    struct Card { RECT rect; std::wstring title,description; int text_width; COLORREF title_color=foreground; };
    struct InputFrame { HWND edit; RECT rect; };
    // A Hotkey keeps its chord in config.json form in `text`; clear_x is where the last
    // click landed, so a click on its clear glyph empties it instead of recording.
    struct Widget { enum Kind {Button,Toggle,Choice,Nav,Link,Hotkey} kind=Button; std::wstring text; std::vector<std::wstring> options; int index=0,click_x=-1; bool checked=false,hover=false,pressed=false,primary=false; wchar_t icon=0; };
    struct PopupRow { std::wstring text,hint; wchar_t icon=0; int action=0; bool checked=false,separator=false; };
    std::vector<Binding> bindings; std::vector<HWND> children,nav_buttons; std::vector<Position> positions;
    std::vector<Card> cards; std::vector<InputFrame> input_frames; std::map<HWND,Widget> widgets;
    std::vector<std::string> command_names; std::vector<PopupRow> popup_rows; HWND popup_choice=nullptr;
    int popup_hover=-1,popup_scroll=0,popup_width=340; POINT popup_origin{};
    HWND save_button=nullptr,discard_button=nullptr,config_link=nullptr,version_link=nullptr;
    // The hotkey field recording a chord, if any; the keyboard hook feeds it.
    HWND capturing=nullptr; ChordRecorder recorder; std::string version_url;
    std::mutex notify_mutex;std::deque<std::pair<std::string,std::string>> notifications;std::atomic<HWND> notification_target{nullptr};
    explicit Impl(Config& c,UiCallbacks cb):config(c),callbacks(std::move(cb)){}
    ~Impl(){notification_target=nullptr;for(auto f:{font,small_font,title_font,section_font,icon_font})if(f)DeleteObject(f);for(auto i:{ready_icon,recording_icon,working_icon})if(i)DestroyIcon(i);DeleteObject(brush);DeleteObject(field_brush);if(gdiplus)Gdiplus::GdiplusShutdown(gdiplus);}
    int px(int value)const{return MulDiv(value,dpi,96);} int logical(int value)const{return MulDiv(value,96,dpi);}
    RECT scaled(RECT r,int offset=0)const{return {px(r.left),px(r.top-offset),px(r.right),px(r.bottom-offset)};}
    Json status(){try{return callbacks.status?callbacks.status():Json::object();}catch(...){return Json::object();}}
    void action(std::string value){if(callbacks.action)callbacks.action(std::move(value));}
    void error(const std::exception& e){log(std::string("Settings: ")+e.what());footer_message=wide(e.what());if(settings)InvalidateRect(settings,nullptr,FALSE);notification("Error",e.what());}
    void notification(std::string title,std::string message){{std::lock_guard lock(notify_mutex);notifications.emplace_back(std::move(title),std::move(message));}if(auto h=notification_target.load())PostMessageW(h,notification_message,0,0);}
    void show_notifications(){std::deque<std::pair<std::string,std::string>> queue;{std::lock_guard lock(notify_mutex);queue.swap(notifications);}for(auto& [title,message]:queue){tray.uFlags=NIF_INFO;tray.dwInfoFlags=NIIF_INFO;wcsncpy_s(tray.szInfoTitle,wide(title).c_str(),_TRUNCATE);wcsncpy_s(tray.szInfo,wide(message).c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&tray);}}
    void make_fonts(){for(auto f:{font,small_font,title_font,section_font,icon_font})if(f)DeleteObject(f);auto make=[&](int size,int weight,const wchar_t* face){return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);};font=make(13,FW_NORMAL,L"Segoe UI Variable Text");small_font=make(12,FW_NORMAL,L"Segoe UI Variable Text");title_font=make(24,FW_SEMIBOLD,L"Segoe UI Variable Display");section_font=make(16,FW_SEMIBOLD,L"Segoe UI Variable Display");icon_font=make(14,FW_NORMAL,L"Segoe Fluent Icons");}
    HWND widget(HWND parent,int id,Widget value,int x,int y,int width,int height){
        HWND h=CreateWindowExW(0,L"LocalSTT.Native.Fluent",value.text.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP,px(x),px(y),px(width),px(height),parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),this);
        if(!h)throw std::runtime_error("Cannot create settings control");widgets.emplace(h,std::move(value));if(parent==panel){children.push_back(h);positions.push_back({h,x,y,width,height});}return h;
    }
    HWND button(const wchar_t* title,int id,int y,int x=0,int width=220,bool primary=false){Widget w;w.text=title;w.primary=primary;return widget(panel,id,std::move(w),x,y,width,32);}
    void clear_panel(){stop_capture();for(auto h:children)DestroyWindow(h);children.clear();positions.clear();bindings.clear();cards.clear();input_frames.clear();command_names.clear();}
    int measure(std::wstring_view text,int width,HFONT face){if(text.empty())return 0;HDC dc=GetDC(panel?panel:settings);auto old=SelectObject(dc,face);RECT r{0,0,px(width),0};DrawTextW(dc,text.data(),static_cast<int>(text.size()),&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,old);ReleaseDC(panel?panel:settings,dc);return logical(r.bottom)+1;}
    int add_card(std::wstring title,std::wstring description,int& y,int control_width=0,COLORREF title_color=foreground){int text_width=content_width-28-(control_width?control_width+20:0);int text_height=measure(title,text_width,font)+(description.empty()?0:4+measure(description,text_width,small_font));int height=std::max(64,text_height+28);cards.push_back({{0,y,content_width,y+height},std::move(title),std::move(description),text_width,title_color});int old=y;y+=height+6;return old;}
    void info(std::wstring title,std::wstring description,int& y,COLORREF c=foreground){add_card(std::move(title),std::move(description),y,0,c);}
    void stop_capture(){if(!capturing)return;HWND field=capturing;capturing=nullptr;set_capture_window(nullptr);recorder.clear();if(IsWindow(field))InvalidateRect(field,nullptr,FALSE);}
    void start_capture(HWND field){if(capturing)stop_capture();capturing=field;recorder.clear();set_capture_window(field);InvalidateRect(field,nullptr,FALSE);}
    void captured_key(HWND field,unsigned vk,bool down){
        if(field!=capturing)return;auto name=key_name(vk);if(!name)return;
        if(down){recorder.press(*name);InvalidateRect(field,nullptr,FALSE);return;}
        if(!recorder.release(*name))return;
        auto chord=format_chord(recorder.seen());stop_capture();widgets.at(field).text=wide(chord);field_changed();InvalidateRect(field,nullptr,FALSE);
    }
    void hotkey_clicked(HWND field){
        auto& w=widgets.at(field);RECT r{};GetClientRect(field,&r);
        if(capturing==field){stop_capture();return;}
        if(!w.text.empty()&&w.click_x>=r.right-px(30)){w.text.clear();field_changed();InvalidateRect(field,nullptr,FALSE);return;}
        start_capture(field);
    }
    void field_changed(){footer_message=L"Unsaved changes";if(settings)InvalidateRect(settings,nullptr,FALSE);}
    std::string hotkey_hint(const Json& cfg,const char* key){auto chord=parse_chord(cfg.value(key,std::string()));return chord.empty()?std::string():chord_label(chord);}
    void update_version_badge(){
        if(!version_link)return;auto& w=widgets.at(version_link);auto last=last_check(config.state_dir);
        version_url=last&&last->offers_update()?releases_url(config.snapshot()):std::string();
        w.text=version_url.empty()?L"LocalSTT "+wide(version):L"LocalSTT "+wide(tag_version(last->latest))+L" available";
        w.primary=!version_url.empty();InvalidateRect(version_link,nullptr,FALSE);
    }
    std::wstring version_summary(){
        auto last=last_check(config.state_dir);
        if(!last||last->stale())return L"Ask GitHub whether there is a newer release.";
        if(last->latest.empty())return L"Asked "+wide(last->when)+L", but GitHub could not be reached.";
        if(last->offers_update())return L"LocalSTT "+wide(tag_version(last->latest))+L" is available - seen "+wide(last->when)+L".";
        return L"Up to date as of "+wide(last->when)+L".";
    }
    std::wstring choice_label(std::string_view key,const Json& value){if(key=="language"){if(value=="ru")return L"Russian";if(value=="en")return L"English";if(value=="auto")return L"Detect automatically";}if(key=="delivery_method"){if(value=="paste")return L"Paste (Ctrl+V)";if(value=="typewrite")return L"Type character by character";}return display_value(value);}
    void add_field(const Field& f,int& y,const Json& values){
        Json value=values.contains(f.key)?values[f.key]:Json();Binding binding{f,nullptr,{}};std::string kind=f.kind;
        int control_width=kind=="bool"?40:(kind=="number"||kind=="int")?120:240;
        if(kind=="hotkey"){Widget w;w.kind=Widget::Hotkey;w.text=value.is_string()?wide(value.get<std::string>()):L"";control_width=std::min(control_width,std::max(120,content_width/2-20));int top=add_card(f.label,f.description,y,control_width),height=y-top-6;binding.control=widget(panel,field_id+static_cast<int>(bindings.size()),std::move(w),content_width-14-control_width,top+(height-32)/2,control_width,32);bindings.push_back(std::move(binding));return;}control_width=std::min(control_width,std::max(120,content_width/2-20));
        int top=add_card(f.label,f.description,y,control_width),height=y-top-6,x=content_width-14-control_width,control_y=top+(height-32)/2;
        int id=field_id+static_cast<int>(bindings.size());
        if(kind=="bool"){Widget w;w.kind=Widget::Toggle;w.checked=value.is_boolean()&&value.get<bool>();binding.control=widget(panel,id,std::move(w),x,top+(height-20)/2,40,20);}
        else if(kind=="choice"||kind=="microphone"){
            Widget w;w.kind=Widget::Choice;
            if(kind=="choice"){std::wistringstream list(f.options);std::wstring part;while(std::getline(list,part,L'|'))binding.choices.push_back(utf8(part));if(!value.is_null()&&std::find(binding.choices.begin(),binding.choices.end(),value)==binding.choices.end())binding.choices.push_back(value);for(auto& option:binding.choices)w.options.push_back(choice_label(f.key,option));}
            else{binding.choices.push_back(nullptr);w.options.push_back(L"Windows default microphone");for(auto& dev:status().value("microphones",Json::array())){if(!dev.is_object())continue;int id=dev.value("id",dev.value("index",0));binding.choices.push_back(id);w.options.push_back(wide(dev.value("name",std::string("Microphone ")+std::to_string(id))));}if(!value.is_null()&&std::find(binding.choices.begin(),binding.choices.end(),value)==binding.choices.end()){binding.choices.push_back(value);w.options.push_back(L"Device "+display_value(value));}}
            auto found=std::find(binding.choices.begin(),binding.choices.end(),value);w.index=found==binding.choices.end()?0:static_cast<int>(std::distance(binding.choices.begin(),found));binding.control=widget(panel,id,std::move(w),x,control_y,control_width,32);
        }else{
            std::wstring text=display_value(value);binding.control=CreateWindowExW(0,L"EDIT",text.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,px(x+9),px(control_y+7),px(control_width-18),px(19),panel,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(binding.control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);SendMessageW(binding.control,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);SetWindowSubclass(binding.control,edit_procedure,1,reinterpret_cast<DWORD_PTR>(this));children.push_back(binding.control);positions.push_back({binding.control,x+9,control_y+7,control_width-18,19});input_frames.push_back({binding.control,{x,control_y,x+control_width,control_y+32}});
        }
        bindings.push_back(std::move(binding));
    }
    Json field_value(const Binding& bind){std::string kind=bind.field.kind;if(kind=="bool")return widgets.at(bind.control).checked;if(kind=="hotkey")return utf8(widgets.at(bind.control).text);if(kind=="choice"||kind=="microphone"){auto selected=widgets.at(bind.control).index;return selected>=0&&static_cast<size_t>(selected)<bind.choices.size()?bind.choices[selected]:Json();}auto value=utf8(trim(window_text(bind.control)));if(kind=="nullable")return value.empty()?Json():Json(value);if(kind=="array"){Json result=Json::array();std::istringstream stream(value);std::string part;while(std::getline(stream,part,',')){part=utf8(trim(wide(part)));if(!part.empty())result.push_back(part);}return result;}if(kind=="int"||kind=="number"){size_t used=0;double number;try{number=std::stod(value,&used);}catch(...){throw std::runtime_error(std::string(bind.field.key)+": enter a number");}if(used!=value.size()||!std::isfinite(number)||number<bind.field.minimum||number>bind.field.maximum||(kind=="int"&&std::floor(number)!=number))throw std::runtime_error(std::string(bind.field.key)+": value is outside the allowed range");if(kind=="int")return static_cast<int>(number);return number;}return value;}
    void stage(){Json next=pending;for(auto& bind:bindings)next[bind.field.key]=field_value(bind);pending=std::move(next);}
    void save(){stage();auto old=config.snapshot();
        // Two actions on one chord would leave one of them unreachable: refuse to save.
        auto merged=old;merged.update(pending);if(auto conflict=hotkey_conflict(merged)){footer_message=wide(*conflict);InvalidateRect(settings,nullptr,FALSE);return;}bool restart=false;for(const char* key:{"model","native_model_path","native_ct2_model_path","native_backend","device","compute_type","api_host","api_port"})if(pending.contains(key)&&(!old.contains(key)||pending[key]!=old[key]))restart=true;config.update(pending);pending=Json::object();footer_message=restart?L"Saved · restart required":L"All changes saved";action("config-saved");InvalidateRect(settings,nullptr,FALSE);}
    std::wstring describe(const Json& value){if(value.is_string())return wide(value.get<std::string>());if(value.is_null())return {};if(value.is_number()||value.is_boolean())return wide(value.dump());std::wstring out;if(value.is_array()){for(auto& item:value){if(!out.empty())out+=L"\n";out+=describe(item);}}else if(value.is_object()){for(auto it=value.begin();it!=value.end();++it){if(it.value().is_object()||it.value().is_array())continue;if(!out.empty())out+=L"\n";out+=wide(it.key())+L": "+describe(it.value());}}return out;}
    std::wstring number(double value,int digits=2){std::wostringstream out;out<<std::fixed<<std::setprecision(digits)<<value;return out.str();}
    std::wstring date(const Json& row){auto value=wide(row.value("ts",std::string()));std::replace(value.begin(),value.end(),L'T',L' ');return value;}
    std::wstring cleanup_detail(const Json& rec){if(!rec.is_object())return describe(rec);auto chord=hotkey_hint(config.snapshot(),"hotkey_cleanup");std::wstring detail=rec.value("reachable",false)?L"Ollama is available.":L"Ollama is not responding. Cleanup dictation"+(chord.empty()?std::wstring():L" ("+wide(chord)+L")")+L" needs Ollama running. Plain dictation works without it.";if(rec.contains("pull"))detail+=L"\nSuggested model: "+wide(rec.value("pull",std::string()));if(rec.contains("budget_gb"))detail+=L"\nEstimated memory available: "+number(rec.value("budget_gb",0.0),1)+L" GB";if(rec.contains("best_installed")&&rec["best_installed"].is_string())detail+=L"\nInstalled and suitable: "+wide(rec["best_installed"].get<std::string>());return detail;}
    std::wstring health_detail(const Json& check){auto detail=check.value("detail",Json());auto name=check.value("name",std::string());if(detail.is_array()){std::wstring result;for(auto& item:detail)if(item.is_object()&&item.contains("name")){if(!result.empty())result+=L"\n";result+=wide(item["name"].get<std::string>());}return result.empty()?L"No microphone found. Check Windows microphone permissions and connect an input device.":result;}if(!detail.is_object())return describe(detail);if(name.find("Ollama")!=std::string::npos)return cleanup_detail(detail);if(detail.contains("model"))return L"Model: "+wide(detail.value("model",std::string()))+L"\n"+wide(detail.value("compute_type",std::string()))+L" on "+wide(detail.value("gpu",detail.value("device",std::string())))+L"\nEngine: "+wide(detail.value("backend",std::string()));if(detail.contains("vram_bytes"))return wide(detail.value("name",std::string("NVIDIA GPU")))+L"\nVideo memory: "+number(detail.value("vram_bytes",0.0)/1073741824.0,1)+L" GB";return describe(detail);}
    void performance_cards(const Json& report,int& y){auto transcription=report.value("transcription",Json::object()),cleanup=report.value("cleanup",Json::object());if(transcription.empty()&&cleanup.empty()){info(L"Nothing measured yet",L"Dictate once and come back to see recognition and cleanup timings.",y);return;}if(!transcription.empty()){double audio=transcription.value("audio_seconds",0.0),processing=transcription.value("processing_seconds",0.0),rtf=transcription.value("realtime_factor",0.0);std::wstring body=number(audio,1)+L" s of speech transcribed in "+number(processing)+L" s";if(rtf>0)body+=L" — "+number(1/rtf,1)+L"× faster than real time";body+=L"\nReal-time factor: "+number(rtf)+L". Lower is faster.";body+=L"\n"+std::to_wstring(transcription.value("chars",0))+L" characters · "+wide(transcription.value("model",std::string()))+L" · "+wide(transcription.value("compute_type",std::string()))+L" · beam "+std::to_wstring(transcription.value("beam_size",5));body+=L"\n"+date(transcription);info(L"Last dictation",body,y);auto session=transcription.value("session",Json::object());int count=session.value("transcription_count",0);if(count)info(L"Since LocalSTT started",std::to_wstring(count)+L" dictation(s), "+number(session.value("total_audio_duration",0.0),1)+L" s of audio\nAverage "+number(session.value("average_processing_time",0.0))+L" s per dictation · real-time factor "+number(session.value("realtime_factor",0.0)),y);}if(!cleanup.empty()){info(L"Last AI cleanup",std::to_wstring(cleanup.value("tokens",0))+L" tokens in "+number(cleanup.value("generate_seconds",0.0))+L" s — "+number(cleanup.value("tokens_per_second",0.0),1)+L" tokens per second\n"+wide(cleanup.value("model",std::string()))+L" · "+number(cleanup.value("total_seconds",0.0))+L" s end to end\n"+date(cleanup),y);}}
    void build_section(int next){
        clear_panel();section=next;scroll=0;RECT r{};GetClientRect(panel,&r);content_width=std::max(430,logical(r.right)-16);Json values=config.snapshot();values.update(pending);int y=0;
        for(auto& f:fields[section])add_field(f,y,values);
        if(section==general_section){int top=add_card(L"Start with Windows",L"Adds a shortcut to the Startup folder. Applied immediately.",y,40);Widget w;w.kind=Widget::Toggle;w.checked=fs::exists(startup_path());widget(panel,startup_id,std::move(w),content_width-54,top+(y-top-6-20)/2,40,20);
            // The version card: seeded from the stored answer, so the page says something
            // without a request; "Check now" asks again.
            auto update=status().value("update",Json());auto last=last_check(config.state_dir);std::wstring offer;std::string offer_url;
            if(update.is_object()){offer=wide(update.value("version",std::string()));offer_url=update.value("url",std::string());}
            else if(last&&last->offers_update()){offer=wide(tag_version(last->latest));offer_url=releases_url(config.snapshot());}
            std::wstring summary=update.is_object()?wide(update.value("label",std::string()))+L" - published "+wide(update.value("published",std::string()).substr(0,10)):version_summary();
            top=add_card(L"LocalSTT "+wide(version),summary,y,offer.empty()?110:228);int middle=top+(y-top-6-32)/2;button(L"Check now",check_updates_id,middle,content_width-14-110,110);
            if(!offer.empty()){version_url=offer_url;button((L"Get "+offer).c_str(),get_update_id,middle,content_width-14-228,110,true);}
            y+=10;button(L"Edit recognition dictionary",dictionary_id,y);y+=44;}
        if(section==commands_section){
            y+=10;button(L"Edit commands",commands_file_id,y,0,180);button(L"Edit app names",aliases_id,y,188,180);y+=42;button(L"Rebuild app index",rebuild_id,y,0,180);button(L"Command history",command_history_id,y,188,180);y+=52;
            auto data=read_json(config.user_file(L"commands.json"));auto state=status().value("commands",Json::array());
            for(auto& command:data.value("commands",Json::array())){auto name=command.value("name",std::string("unnamed"));std::string reason;for(auto& row:state){if(!row.is_object())continue;std::string rowname=row.value("name",std::string());if(row.contains("command")&&row["command"].is_object())rowname=row["command"].value("name",rowname);if(rowname==name&&!row.value("available",true))reason=row.value("reason",std::string("Unavailable"));}std::wstring desc;for(auto& pattern:command.value("patterns",Json::array())){if(!desc.empty())desc+=L" · ";desc+=wide(pattern.get<std::string>());if(desc.size()>170){desc+=L"…";break;}}if(!reason.empty())desc+=L"\n"+wide(reason);int top=add_card(wide(name),desc,y,40);Widget w;w.kind=Widget::Toggle;w.checked=command.value("enabled",true);widget(panel,3000+static_cast<int>(command_names.size()),std::move(w),content_width-54,top+(y-top-6-20)/2,40,20);command_names.push_back(name);}
        }
        if(section==cleanup_section){y+=10;int top=add_card(L"Cleanup instructions",L"What the model is told to do with the transcript. Edit it to change the result: ask for a different tone, another language, a summary. Takes effect on the next dictation.",y,140);button(L"Edit prompt",prompt_id,top+(y-top-6-32)/2,content_width-14-140,140);auto recommendation=status().value("cleanup_recommendation",Json());if(!recommendation.is_null()){y+=10;info(L"Recommended for this GPU",cleanup_detail(recommendation),y);}y+=10;button(L"Download cleanup model",pull_id,y,0,220);y+=44;info(L"Original text is always kept",L"If Ollama is unavailable or cleanup times out, the Whisper transcript is delivered unchanged.",y);}
        if(section==history_section){y+=10;button(L"Open history file",history_file_id,y,0,180);button(L"Clear history…",history_clear_id,y,188,180);y+=48;std::ifstream stream(config.state_dir/"history.jsonl");std::string line;std::deque<Json> rows;while(std::getline(stream,line)){try{rows.push_back(Json::parse(line));if(rows.size()>100)rows.pop_front();}catch(...){}}if(rows.empty())info(L"No dictations saved",L"Enable history above to keep transcripts with their date and time.",y);else for(auto it=rows.rbegin();it!=rows.rend();++it)info(date(*it),wide(it->value("text",std::string())),y);}
        if(section==performance_section){y+=10;auto report=read_json(config.state_dir/"performance.json");performance_cards(report,y);button(L"Refresh measurements",refresh_id,y);y+=44;}
        if(section==api_section){y+=10;info(L"Local transcription API",L"POST /v1/audio/transcriptions\nGET /health  ·  GET /metrics\nUse multipart form data with an audio file. Response formats: json or text.",y);}
        if(section==health_section){button(L"Run self-test",selftest_id,y,0,160,true);button(L"Refresh report",refresh_id,y,168,160);y+=48;auto report=read_json(config.state_dir/"preflight.json");if(report.empty())info(L"Health not checked yet",L"Run the self-test to check your model, GPU, microphone and cleanup service.",y);else{for(auto& check:report.value("checks",Json::array())){bool ok=check.value("ok",false);info(std::wstring(ok?L"✓  ":L"!  ")+wide(check.value("name",std::string("Check"))),health_detail(check),y,ok?accent:RGB(255,153,164));}if(report.contains("ts"))info(L"Last checked",wide(report["ts"].get<std::string>()),y);}y+=8;button(L"Open log folder",logs_id,y);y+=44;}
        content_height=y+16;update_version_badge();scroll_to(0);for(auto nav:nav_buttons)InvalidateRect(nav,nullptr,FALSE);InvalidateRect(settings,nullptr,FALSE);
    }
    void scroll_to(int value){if(!panel)return;RECT r{};GetClientRect(panel,&r);scroll=std::clamp(value,0,std::max(0,content_height-logical(r.bottom)));HDWP moves=BeginDeferWindowPos(static_cast<int>(positions.size()));for(auto p:positions){if(moves)moves=DeferWindowPos(moves,p.handle,nullptr,px(p.x),px(p.y-scroll),px(p.width),px(p.height),SWP_NOZORDER|SWP_NOACTIVATE);else SetWindowPos(p.handle,nullptr,px(p.x),px(p.y-scroll),px(p.width),px(p.height),SWP_NOZORDER|SWP_NOACTIVATE);}if(moves)EndDeferWindowPos(moves);InvalidateRect(panel,nullptr,FALSE);}
    void ensure_visible(HWND control){if(!panel)return;RECT r{};GetClientRect(panel,&r);int height=logical(r.bottom);for(auto& p:positions)if(p.handle==control){if(p.y<scroll)scroll_to(std::max(0,p.y-14));else if(p.y+p.height>scroll+height)scroll_to(p.y+p.height-height+14);break;}}
    RECT scroll_thumb(){RECT r{};GetClientRect(panel,&r);int height=logical(r.bottom);if(content_height<=height)return {};int thumb=std::max(36,height*height/content_height),top=(height-thumb)*scroll/std::max(1,content_height-height);return {r.right-px(7),px(top),r.right-px(3),px(top+thumb)};}
    void draw_panel(HDC dc){RECT client{};GetClientRect(panel,&client);FillRect(dc,&client,brush);for(auto& c:cards){auto rect=scaled(c.rect,scroll);if(rect.bottom<0||rect.top>client.bottom)continue;rounded(dc,rect,static_cast<float>(px(6)),card_color,divider);RECT title{px(14),rect.top+px(14),px(14+c.text_width),rect.bottom-px(14)};RECT measure=title;auto old=SelectObject(dc,font);DrawTextW(dc,c.title.c_str(),-1,&measure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,old);title.bottom=measure.bottom;text_at(dc,font,c.title_color,c.title,title,DT_LEFT|DT_WORDBREAK);if(!c.description.empty()){RECT description{title.left,title.bottom+px(4),title.right,rect.bottom-px(10)};text_at(dc,small_font,secondary,c.description,description,DT_LEFT|DT_WORDBREAK);}}
        for(auto& input:input_frames){auto r=scaled(input.rect,scroll);rounded(dc,r,static_cast<float>(px(4)),background,GetFocus()==input.edit?accent:divider);if(GetFocus()==input.edit){RECT line{r.left+px(2),r.bottom-px(2),r.right-px(2),r.bottom};rounded(dc,line,1,accent);}}
        auto thumb=scroll_thumb();if(thumb.bottom>thumb.top)rounded(dc,thumb,static_cast<float>(px(2)),muted);
    }
    void draw_settings(HDC dc){RECT r{};GetClientRect(settings,&r);FillRect(dc,&r,brush);text_at(dc,title_font,foreground,L"Settings",{px(20),px(18),px(220),px(50)});text_at(dc,section_font,foreground,section_names[section],{px(254),px(20),r.right-px(24),px(46)});RECT footer{0,r.bottom-px(56),r.right,r.bottom};HBRUSH fill=CreateSolidBrush(surface);FillRect(dc,&footer,fill);DeleteObject(fill);std::wstring health=L"Health: not checked yet";auto current=status();if(current.value("ready",false))health=L"✓  Ready";else if(current.value("state",std::string())=="error")health=L"!  Check Health";text_at(dc,small_font,secondary,footer_message.empty()?health:footer_message,{px(372),footer.top,r.right-px(235),r.bottom});}
    void draw_widget(HWND h,HDC dc){auto found=widgets.find(h);if(found==widgets.end())return;auto& w=found->second;RECT r{};GetClientRect(h,&r);HBRUSH bg=CreateSolidBrush(GetParent(h)==panel?card_color:GetDlgCtrlID(h)==save_id||GetDlgCtrlID(h)==close_id||GetDlgCtrlID(h)==config_file_id||GetDlgCtrlID(h)==version_link_id?surface:background);FillRect(dc,&r,bg);DeleteObject(bg);
        if(w.kind==Widget::Toggle){COLORREF track=w.checked?accent:card_color;rounded(dc,{0,0,r.right,r.bottom},static_cast<float>(r.bottom/2),track,w.checked?accent:secondary);int radius=px(w.hover?7:6),cx=w.checked?r.right-r.bottom/2:r.bottom/2,cy=r.bottom/2;Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::SolidBrush knob(color(w.checked?foreground:secondary));g.FillEllipse(&knob,cx-radius,cy-radius,radius*2,radius*2);}
        else if(w.kind==Widget::Nav){bool selected=GetDlgCtrlID(h)-400==section;rounded(dc,r,static_cast<float>(px(4)),selected?hover_color:w.hover?RGB(51,51,51):background);if(selected)rounded(dc,{px(2),r.bottom*28/100,px(5),r.bottom*72/100},static_cast<float>(px(2)),accent);std::wstring icon(1,w.icon);text_at(dc,icon_font,foreground,icon,{px(10),0,px(32),r.bottom},DT_CENTER|DT_VCENTER|DT_SINGLELINE);text_at(dc,font,foreground,w.text,{px(40),0,r.right-px(8),r.bottom});}
        else if(w.kind==Widget::Link){text_at(dc,font,w.primary?accent:secondary,w.text,r);}
        else if(w.kind==Widget::Hotkey){bool active=capturing==h;rounded(dc,r,static_cast<float>(px(4)),active?card_color:w.hover?hover_color:background,active?accent:divider);
            std::wstring label;COLORREF c=foreground;
            if(active){label=recorder.seen().empty()?L"Press a key combination…":wide(chord_label(recorder.seen()));c=recorder.seen().empty()?secondary:accent;}
            else{label=wide(chord_label(utf8(w.text)));if(w.text.empty())c=secondary;}
            text_at(dc,font,c,label,{px(12),0,r.right-px(34),r.bottom});
            if(!active&&!w.text.empty())text_at(dc,icon_font,secondary,L"\ue894",{r.right-px(30),0,r.right-px(8),r.bottom},DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        else {COLORREF fill=w.primary?accent:w.kind==Widget::Choice?background:card_color;if(w.hover)fill=w.primary?RGB(std::min(255,GetRValue(accent)+22),std::min(255,GetGValue(accent)+22),std::min(255,GetBValue(accent)+22)):hover_color;if(w.pressed)fill=RGB(51,51,51);rounded(dc,r,static_cast<float>(px(4)),fill,w.primary?fill:divider);if(w.kind==Widget::Choice){std::wstring label=w.index>=0&&static_cast<size_t>(w.index)<w.options.size()?w.options[w.index]:L"";text_at(dc,font,foreground,label,{px(12),0,r.right-px(32),r.bottom});text_at(dc,icon_font,secondary,L"\ue70d",{r.right-px(26),0,r.right-px(8),r.bottom},DT_CENTER|DT_VCENTER|DT_SINGLELINE);}else{text_at(dc,font,foreground,w.text,{px(8),0,r.right-px(8),r.bottom},DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);}}
        if(GetFocus()==h){RECT focus=r;InflateRect(&focus,-px(2),-px(2));HPEN pen=CreatePen(PS_DOT,1,secondary);auto old=SelectObject(dc,pen),oldbrush=SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,focus.left,focus.top,focus.right,focus.bottom);SelectObject(dc,old);SelectObject(dc,oldbrush);DeleteObject(pen);}
    }
    void paint(HWND window,HDC provided=nullptr){PAINTSTRUCT ps{};HDC target=provided?provided:BeginPaint(window,&ps);RECT r{};GetClientRect(window,&r);HDC buffer=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,r.right),std::max(1L,r.bottom));auto old=SelectObject(buffer,bitmap);if(window==settings)draw_settings(buffer);else if(window==panel)draw_panel(buffer);else if(window==popup)draw_popup(buffer);else draw_widget(window,buffer);BitBlt(target,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);if(!provided)EndPaint(window,&ps);}
    void command(int id,int event=0){
        if(id>=400&&id<400+static_cast<int>(section_names.size())){stage();build_section(id-400);return;}
        if(id>=field_id&&id<field_id+static_cast<int>(bindings.size())){auto& bind=bindings[id-field_id];if(event!=BN_CLICKED&&event!=CBN_SELCHANGE&&event!=EN_CHANGE)return;if(std::string(bind.field.kind)=="hotkey"){hotkey_clicked(bind.control);return;}footer_message=L"Unsaved changes";if(std::string(bind.field.key)=="live_dictation"){auto value=field_value(bind);config.update(Json{{"live_dictation",value}});pending["live_dictation"]=value;footer_message=L"Live dictation applied";action("config-saved");}if(settings)InvalidateRect(settings,nullptr,FALSE);return;}
        if(id>=3000&&id<3000+static_cast<int>(command_names.size())){set_command_enabled(config,command_names[id-3000],widgets.at(GetDlgItem(panel,id)).checked);action("config-saved");return;}
        switch(id){case save_id:save();break;case close_id:stop_capture();pending=Json::object();footer_message=L"Changes discarded";build_section(section);ShowWindow(settings,SW_HIDE);break;case refresh_id:stage();build_section(section);break;
        case startup_id:set_startup(widgets.at(GetDlgItem(panel,startup_id)).checked,config.state_dir,config.install_dir);footer_message=L"Startup setting applied";InvalidateRect(settings,nullptr,FALSE);break;
        case check_updates_id:footer_message=L"Asking GitHub…";InvalidateRect(settings,nullptr,FALSE);action("check-updates");break;
        case get_update_id:if(!version_url.empty())open_url(version_url);break;
        case version_link_id:if(!version_url.empty())open_url(version_url);else{stage();build_section(general_section);}break;
        case update_id:{auto update=status().value("update",Json());if(update.is_object())open_url(update.value("url",std::string()));break;}
        case prompt_id:{auto path=config.editable_file(L"cleanup-prompt.txt");open_file(settings,path);footer_message=L"Editing "+path.wstring();InvalidateRect(settings,nullptr,FALSE);break;}
        case commands_file_id:open_file(settings,config.editable_file(L"commands.json"));break;case aliases_id:open_file(settings,config.state_dir/"app-aliases.json");break;case dictionary_id:open_file(settings,config.editable_file(L"dictionary.json"));break;case config_file_id:open_file(settings,config.state_dir/"config.json");break;case logs_id:open_file(settings,config.state_dir/"logs");break;case history_file_id:open_file(settings,config.state_dir/"history.jsonl");break;case command_history_id:open_file(settings,config.state_dir/"command-history.jsonl");break;
        case selftest_id:action("selftest");break;case rebuild_id:action("rebuild-index");break;case pull_id:save();action("pull-model");break;case history_clear_id:if(MessageBoxW(settings,L"Delete saved transcript history?",L"LocalSTT",MB_YESNO|MB_ICONQUESTION)==IDYES){std::ofstream(config.state_dir/"history.jsonl",std::ios::trunc);build_section(section);}break;
        case start_id:action("start");break;case cleanup_id:action("cleanup");break;case command_id:action("command");break;case stop_id:action("stop");break;case cancel_id:action("cancel");break;case repeat_id:action("repeat");break;case settings_id:show_settings();break;case restart_id:action("restart");break;case quit_id:action("quit");DestroyWindow(host);break;
        case live_id:config.update(Json{{"live_dictation",!config.snapshot().value("live_dictation",false)}});action("config-saved");break;case russian_id:config.update(Json{{"language","ru"}});action("config-saved");notification("","Language: ru");break;case english_id:config.update(Json{{"language","en"}});action("config-saved");notification("","Language: en");break;case auto_id:config.update(Json{{"language","auto"}});action("config-saved");notification("","Language: auto");break;case typewrite_id:config.update(Json{{"delivery_method","typewrite"}});action("config-saved");notification("","Delivery: typewrite");break;case paste_id:config.update(Json{{"delivery_method","paste"}});action("config-saved");notification("","Delivery: paste");break;
        case copy_clipboard_id:{bool keep=!config.snapshot().value("copy_to_clipboard",false);config.update(Json{{"copy_to_clipboard",keep}});action("config-saved");notification("",std::string("Copy to clipboard: ")+(keep?"on":"off"));break;}}
    }
    void layout(bool rebuild=false){if(!settings||!panel)return;RECT r{};GetClientRect(settings,&r);int w=logical(r.right),h=logical(r.bottom);MoveWindow(panel,px(254),px(58),px(std::max(446,w-270)),px(std::max(100,h-126)),TRUE);for(size_t i=0;i<nav_buttons.size();++i)MoveWindow(nav_buttons[i],px(8),px(62+static_cast<int>(i)*40),px(214),px(38),TRUE);MoveWindow(config_link,px(24),px(h-44),px(140),px(32),TRUE);MoveWindow(version_link,px(172),px(h-44),px(192),px(32),TRUE);MoveWindow(discard_button,px(w-212),px(h-44),px(88),px(32),TRUE);MoveWindow(save_button,px(w-114),px(h-44),px(90),px(32),TRUE);if(rebuild){stage();build_section(section);}else scroll_to(scroll);InvalidateRect(settings,nullptr,FALSE);}
    void show_settings(){if(settings&&IsWindowVisible(settings)){SetForegroundWindow(settings);return;}if(!settings){RECT bounds{0,0,px(1000),px(700)};AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);settings=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_CONTROLPARENT,L"LocalSTT.Native.Settings",L"LocalSTT settings",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,GetModuleHandleW(nullptr),this);if(!settings)throw std::runtime_error("Cannot create settings window");BOOL dark=TRUE;DWORD rounded_corner=2;DwmSetWindowAttribute(settings,20,&dark,sizeof(dark));DwmSetWindowAttribute(settings,33,&rounded_corner,sizeof(rounded_corner));COLORREF chrome=background;DwmSetWindowAttribute(settings,35,&chrome,sizeof(chrome));DwmSetWindowAttribute(settings,34,&chrome,sizeof(chrome));for(size_t i=0;i<section_names.size();++i){Widget w;w.kind=Widget::Nav;w.text=section_names[i];w.icon=section_icons[i];nav_buttons.push_back(widget(settings,400+static_cast<int>(i),std::move(w),8,62+static_cast<int>(i)*40,214,38));}panel=CreateWindowExW(WS_EX_CONTROLPARENT,L"LocalSTT.Native.Panel",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,px(254),px(58),px(730),px(574),settings,nullptr,GetModuleHandleW(nullptr),this);Widget link;link.kind=Widget::Link;link.primary=true;link.text=L"Open config.json";config_link=widget(settings,config_file_id,std::move(link),24,656,140,32);Widget badge;badge.kind=Widget::Link;version_link=widget(settings,version_link_id,std::move(badge),172,656,192,32);Widget discard;discard.text=L"Discard";discard_button=widget(settings,close_id,std::move(discard),788,656,88,32);Widget save;save.text=L"Save";save.primary=true;save_button=widget(settings,save_id,std::move(save),886,656,90,32);layout();}pending=Json::object();build_section(section);show_painted(settings,SW_SHOWNORMAL);SetForegroundWindow(settings);}
    // Shown cloaked and painted before DWM composes it, so the first frame is the
    // dark window rather than an unpainted white surface.
    void show_painted(HWND window,int command){BOOL cloak=TRUE;DwmSetWindowAttribute(window,DWMWA_CLOAK,&cloak,sizeof(cloak));ShowWindow(window,command);if(!IsWindowVisible(window))ShowWindow(window,SW_SHOW);RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);cloak=FALSE;DwmSetWindowAttribute(window,DWMWA_CLOAK,&cloak,sizeof(cloak));}
    void open_url(const std::string& url){if(url.starts_with("https://"))ShellExecuteW(settings?settings:host,L"open",wide(url).c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
    void refresh_settings(){if(!settings||!IsWindowVisible(settings))return;if(footer_message==L"Asking GitHub…")footer_message.clear();try{stage();}catch(...){}build_section(section);}
    void remember_focus(){HWND current=GetForegroundWindow();wchar_t name[128]{};GetClassNameW(current,name,128);if(current&&current!=host&&current!=settings&&current!=popup&&wcscmp(name,L"Shell_TrayWnd")&&wcscmp(name,L"Shell_SecondaryTrayWnd")){last_external_window=current;GUITHREADINFO info{sizeof(info)};last_external_control=GetGUIThreadInfo(GetWindowThreadProcessId(current,nullptr),&info)?info.hwndFocus:nullptr;}}
    void restore_target(){if(!IsWindow(last_external_window))return;SetForegroundWindow(last_external_window);if(IsWindow(last_external_control)&&(last_external_control==last_external_window||IsChild(last_external_window,last_external_control))){DWORD thread=GetWindowThreadProcessId(last_external_window,nullptr),current=GetCurrentThreadId();if(thread==current)SetFocus(last_external_control);else if(AttachThreadInput(current,thread,TRUE)){SetFocus(last_external_control);AttachThreadInput(current,thread,FALSE);}}}
    int popup_row_height(const PopupRow& r)const{return r.separator?9:32;}
    int popup_total_height()const{int h=8;for(auto& r:popup_rows)h+=popup_row_height(r);return h;}
    void draw_popup(HDC dc){RECT r{};GetClientRect(popup,&r);rounded(dc,r,static_cast<float>(px(8)),surface,divider);int y=4-popup_scroll;for(size_t i=0;i<popup_rows.size();++i){auto& row=popup_rows[i];int h=popup_row_height(row);RECT item{px(4),px(y+1),r.right-px(4),px(y+h-1)};if(row.separator){RECT line{px(12),px(y+h/2),r.right-px(12),px(y+h/2)+1};HBRUSH b=CreateSolidBrush(divider);FillRect(dc,&line,b);DeleteObject(b);}else{if(static_cast<int>(i)==popup_hover&&row.action)rounded(dc,item,static_cast<float>(px(4)),hover_color);auto c=row.action?foreground:muted;std::wstring icon(1,row.checked?0xe73e:row.icon);if(icon[0])text_at(dc,icon_font,row.checked?accent:c,icon,{px(12),px(y),px(36),px(y+h)},DT_CENTER|DT_VCENTER|DT_SINGLELINE);int hint_width=row.hint.empty()?0:130;text_at(dc,font,c,row.text,{px(44),px(y),r.right-px(16+hint_width),px(y+h)});if(!row.hint.empty())text_at(dc,small_font,secondary,row.hint,{r.right-px(150),px(y),r.right-px(14),px(y+h)},DT_RIGHT|DT_VCENTER|DT_SINGLELINE);}y+=h;}}
    void close_popup(){HWND old=popup;popup=nullptr;if(old)DestroyWindow(old);}
    void open_popup(std::vector<PopupRow> rows,POINT origin,int width,HWND choice=nullptr){close_popup();popup_rows=std::move(rows);popup_choice=choice;popup_hover=-1;popup_scroll=0;popup_width=width;popup_origin=origin;MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(origin,MONITOR_DEFAULTTONEAREST),&monitor);int height=std::min(px(popup_total_height()),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top)-px(16));int x=std::clamp(static_cast<int>(origin.x),static_cast<int>(monitor.rcWork.left+px(4)),static_cast<int>(monitor.rcWork.right-px(width)-px(4))),y=std::clamp(static_cast<int>(origin.y),static_cast<int>(monitor.rcWork.top+px(4)),static_cast<int>(monitor.rcWork.bottom-height-px(4)));popup=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"LocalSTT.Native.Popup",L"",WS_POPUP,x,y,px(width),height,choice?settings:host,nullptr,GetModuleHandleW(nullptr),this);HRGN region=CreateRoundRectRgn(0,0,px(width)+1,height+1,px(16),px(16));SetWindowRgn(popup,region,TRUE);show_painted(popup,SW_SHOWNORMAL);SetForegroundWindow(popup);SetFocus(popup);}
    void open_choices(HWND control){auto& w=widgets.at(control);std::vector<PopupRow> rows;for(size_t i=0;i<w.options.size();++i)rows.push_back({w.options[i],L"",0,10000+static_cast<int>(i),static_cast<int>(i)==w.index,false});RECT r{};GetWindowRect(control,&r);open_popup(std::move(rows),{r.left,r.bottom+px(2)},std::max(240,logical(r.right-r.left)),control);}
    void menu(){remember_focus();auto cfg=config.snapshot();std::vector<PopupRow> rows;
        // Only present when there is something to say, so the menu carries no permanent update row.
        if(auto update=status().value("update",Json());update.is_object()){rows.push_back({wide(update.value("label",std::string())),L"",0xe896,update_id});rows.push_back({L"",L"",0,0,false,true});}
        auto hint=[&](const char* key){return wide(hotkey_hint(cfg,key));};
        std::vector<PopupRow> standard={{L"Dictate",hint("hotkey_dictation"),0xe720,start_id},{L"Dictate with cleanup",hint("hotkey_cleanup"),0xe945,cleanup_id},{L"Voice command",hint("hotkey_command"),0xe90a,command_id},{L"Stop recording",L"",0xe71a,stop_id},{L"Cancel",cfg.value("cancel_on_escape",true)?hint("hotkey_cancel"):L"",0xe711,cancel_id},{L"Repeat last text",L"",0xe72c,repeat_id},{L"",L"",0,0,false,true},{L"Type while speaking",L"",0xe765,live_id,cfg.value("live_dictation",false)},{L"Language",L"›",0xe774,9001},{L"Delivery",L"›",0xe765,9002},{L"",L"",0,0,false,true},{L"Settings",L"",0xe713,settings_id},{L"Run self-test",L"",0xe95e,selftest_id},{L"Restart",L"",0xe777,restart_id},{L"Exit",L"",0xe8bb,quit_id}};rows.insert(rows.end(),standard.begin(),standard.end());POINT point{};GetCursorPos(&point);open_popup(std::move(rows),point,390);}
    void activate_popup(int index){if(index<0||index>=static_cast<int>(popup_rows.size()))return;int selected=popup_rows[index].action;if(!selected)return;auto choice=popup_choice;POINT origin=popup_origin;close_popup();if(choice){auto found=widgets.find(choice);if(found!=widgets.end()){found->second.index=selected-10000;InvalidateRect(choice,nullptr,FALSE);SetForegroundWindow(settings);SetFocus(choice);PostMessageW(panel,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(choice),CBN_SELCHANGE),reinterpret_cast<LPARAM>(choice));}return;}auto cfg=config.snapshot();if(selected==9001){auto language=cfg.value("language",std::string());open_popup({{L"‹ Back",L"",0,9000},{L"Russian",L"",0,russian_id,language=="ru"},{L"English",L"",0,english_id,language=="en"},{L"Detect automatically",L"",0,auto_id,language=="auto"}},origin,260);return;}if(selected==9002){auto method=cfg.value("delivery_method",std::string());open_popup({{L"‹ Back",L"",0,9000},{L"Paste (Ctrl+V)",L"",0,paste_id,method=="paste"},{L"Type character by character",L"",0,typewrite_id,method=="typewrite"},{L"",L"",0,0,false,true},{L"Copy to clipboard",L"",0xe8c8,copy_clipboard_id,cfg.value("copy_to_clipboard",false)}},origin,290);return;}if(selected==9000){menu();return;}if(selected==start_id||selected==cleanup_id||selected==command_id||selected==repeat_id)restore_target();command(selected);}
    void update_status(){remember_focus();if(GetTickCount64()-last_status_update<500)return;last_status_update=GetTickCount64();auto state=status();auto text=state.value("state",std::string("ready"));if(text==last_state)return;last_state=text;tray.uFlags=NIF_ICON|NIF_TIP;tray.hIcon=(text.find("record")!=std::string::npos||text=="command"||text.find("listen")!=std::string::npos)?recording_icon:(text=="ready"||text=="idle")?ready_icon:working_icon;auto dictate=hotkey_hint(config.snapshot(),"hotkey_dictation");auto tip=wide("LocalSTT · "+text+(dictate.empty()?std::string():" · "+dictate));wcsncpy_s(tray.szTip,tip.c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&tray);if(settings)InvalidateRect(settings,nullptr,FALSE);}
    void add_tray(){tray={};tray.cbSize=sizeof(tray);tray.hWnd=host;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=tray_message;tray.hIcon=ready_icon;wcscpy_s(tray.szTip,L"LocalSTT");Shell_NotifyIconW(NIM_ADD,&tray);tray.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray);}
    static LRESULT CALLBACK edit_procedure(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){auto self=reinterpret_cast<Impl*>(data);if(message==WM_SETFOCUS)self->ensure_visible(window);if(message==WM_SETFOCUS||message==WM_KILLFOCUS)InvalidateRect(self->panel,nullptr,FALSE);if(message==WM_MOUSEWHEEL){self->scroll_to(self->scroll-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*48);return 0;}return DefSubclassProc(window,message,wp,lp);}
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(window,message,wp,lp);
        try{
            if(message==self->taskbar_created&&self->taskbar_created){self->add_tray();return 0;}
            auto widget=self->widgets.find(window);
            if(widget!=self->widgets.end()){
                auto& w=widget->second;
                switch(message){case WM_GETDLGCODE:return DLGC_BUTTON|DLGC_WANTARROWS|((wp==VK_RETURN||wp==VK_SPACE)?DLGC_WANTMESSAGE:0);case BM_CLICK:if(w.kind==Widget::Choice)self->open_choices(window);else{if(w.kind==Widget::Toggle)w.checked=!w.checked;PostMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),BN_CLICKED),reinterpret_cast<LPARAM>(window));InvalidateRect(window,nullptr,FALSE);}return 0;case BM_GETCHECK:return w.checked?BST_CHECKED:BST_UNCHECKED;case BM_SETCHECK:w.checked=wp==BST_CHECKED;InvalidateRect(window,nullptr,FALSE);return 0;case CB_GETCURSEL:return w.index;case CB_SETCURSEL:w.index=static_cast<int>(wp);InvalidateRect(window,nullptr,FALSE);return w.index;case WM_SETFOCUS:self->ensure_visible(window);InvalidateRect(window,nullptr,FALSE);return 0;case WM_KILLFOCUS:InvalidateRect(window,nullptr,FALSE);return 0;
                case WM_MOUSEMOVE:if(!w.hover){w.hover=true;TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);InvalidateRect(window,nullptr,FALSE);}return 0;case WM_MOUSELEAVE:w.hover=false;InvalidateRect(window,nullptr,FALSE);return 0;
                case WM_LBUTTONDOWN:w.pressed=true;SetFocus(window);SetCapture(window);InvalidateRect(window,nullptr,FALSE);return 0;
                case capture_message:self->captured_key(window,static_cast<unsigned>(wp),lp!=0);return 0;
                case WM_LBUTTONUP:{bool pressed=w.pressed;w.pressed=false;ReleaseCapture();RECT rect{};GetClientRect(window,&rect);POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};w.click_x=p.x;if(pressed&&PtInRect(&rect,p)){if(w.kind==Widget::Choice)self->open_choices(window);else{if(w.kind==Widget::Toggle)w.checked=!w.checked;PostMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),BN_CLICKED),reinterpret_cast<LPARAM>(window));}}InvalidateRect(window,nullptr,FALSE);return 0;}
                case WM_KEYDOWN:if(w.kind==Widget::Hotkey)w.click_x=-1;if(wp==VK_SPACE||wp==VK_RETURN||(w.kind==Widget::Choice&&(wp==VK_DOWN||wp==VK_F4))){if(w.kind==Widget::Choice)self->open_choices(window);else{if(w.kind==Widget::Toggle)w.checked=!w.checked;PostMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),BN_CLICKED),reinterpret_cast<LPARAM>(window));InvalidateRect(window,nullptr,FALSE);}return 0;}break;
                }
            }
            if(window==self->popup){switch(message){case WM_MOUSEMOVE:{int y=self->logical(GET_Y_LPARAM(lp))+self->popup_scroll-4,index=-1;for(size_t i=0;i<self->popup_rows.size();++i){int height=self->popup_row_height(self->popup_rows[i]);if(y>=0&&y<height){index=static_cast<int>(i);break;}y-=height;}if(index!=self->popup_hover){self->popup_hover=index;InvalidateRect(window,nullptr,FALSE);}return 0;}case WM_LBUTTONUP:self->activate_popup(self->popup_hover);return 0;case WM_KEYDOWN:if(wp==VK_ESCAPE){self->close_popup();return 0;}if(wp==VK_RETURN){self->activate_popup(self->popup_hover);return 0;}if(wp==VK_UP||wp==VK_DOWN){int count=static_cast<int>(self->popup_rows.size());if(!count)return 0;for(int step=0;step<count;++step){self->popup_hover=(self->popup_hover+(wp==VK_UP?-1:1)+count)%count;if(self->popup_rows[self->popup_hover].action)break;}InvalidateRect(window,nullptr,FALSE);return 0;}break;case WM_ACTIVATE:if(LOWORD(wp)==WA_INACTIVE){self->close_popup();return 0;}break;case WM_MOUSEWHEEL:{RECT r{};GetClientRect(window,&r);self->popup_scroll=std::clamp(self->popup_scroll-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*32,0,std::max(0,self->popup_total_height()-self->logical(r.bottom)));InvalidateRect(window,nullptr,FALSE);return 0;}}}
            switch(message){case WM_PAINT:if(window!=self->host){self->paint(window);return 0;}break;case WM_PRINTCLIENT:if(window!=self->host){self->paint(window,reinterpret_cast<HDC>(wp));return 0;}break;case WM_ERASEBKGND:if(window==self->settings||window==self->panel||window==self->popup){RECT r{};GetClientRect(window,&r);FillRect(reinterpret_cast<HDC>(wp),&r,self->brush);}return 1;
            case WM_ACTIVATE:if(window==self->settings&&LOWORD(wp)==WA_INACTIVE)self->stop_capture();break;
            case WM_COMMAND:self->command(LOWORD(wp),HIWORD(wp));return 0;case WM_TIMER:self->update_status();if(self->callbacks.tick)self->callbacks.tick();return 0;case notification_message:self->show_notifications();return 0;case refresh_message:self->refresh_settings();return 0;
            case tray_message:if(LOWORD(lp)==WM_CONTEXTMENU||LOWORD(lp)==WM_RBUTTONUP)self->menu();else if(LOWORD(lp)==NIN_SELECT||LOWORD(lp)==WM_LBUTTONUP)self->show_settings();return 0;
            case WM_SIZE:if(window==self->settings&&self->panel)self->layout(true);return 0;case WM_GETMINMAXINFO:if(window==self->settings){auto info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={self->px(760),self->px(520)};return 0;}break;
            case WM_DPICHANGED:if(window==self->settings){self->dpi=HIWORD(wp);self->make_fonts();auto rect=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);self->layout(true);return 0;}break;
            case WM_MOUSEWHEEL:self->scroll_to(self->scroll-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*48);return 0;
            case WM_LBUTTONDOWN:if(window==self->panel){auto thumb=self->scroll_thumb();POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(PtInRect(&thumb,p)){self->dragging_scroll=true;self->scroll_drag_y=p.y;self->scroll_drag_value=self->scroll;SetCapture(window);}return 0;}break;
            case WM_MOUSEMOVE:if(window==self->panel&&self->dragging_scroll){RECT r{};GetClientRect(window,&r);auto thumb=self->scroll_thumb();int travel=r.bottom-(thumb.bottom-thumb.top);if(travel>0)self->scroll_to(self->scroll_drag_value+(GET_Y_LPARAM(lp)-self->scroll_drag_y)*(self->content_height-self->logical(r.bottom))/travel);return 0;}break;
            case WM_LBUTTONUP:if(window==self->panel&&self->dragging_scroll){self->dragging_scroll=false;ReleaseCapture();return 0;}break;
            case WM_VSCROLL:if(window==self->panel){int delta=LOWORD(wp)==SB_LINEUP?-32:LOWORD(wp)==SB_LINEDOWN?32:LOWORD(wp)==SB_PAGEUP?-300:300;self->scroll_to(self->scroll+delta);return 0;}break;
            case WM_CTLCOLOREDIT:case WM_CTLCOLORSTATIC:{auto dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,foreground);SetBkColor(dc,background);return reinterpret_cast<LRESULT>(self->field_brush);}
            case WM_CLOSE:if(window==self->settings){self->stop_capture();ShowWindow(window,SW_HIDE);return 0;}if(window==self->host){self->action("quit");DestroyWindow(window);return 0;}break;
            case WM_DESTROY:if(window==self->host){self->notification_target=nullptr;KillTimer(window,1);Shell_NotifyIconW(NIM_DELETE,&self->tray);self->close_popup();if(self->settings)DestroyWindow(self->settings);PostQuitMessage(0);return 0;}break;
            case WM_NCDESTROY:self->widgets.erase(window);if(window==self->popup)self->popup=nullptr;break;
            }
        }catch(const std::exception& ex){self->error(ex);}catch(...){log("Unknown UI error");}return DefWindowProcW(window,message,wp,lp);
    }
    int run(){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);dpi=static_cast<int>(GetDpiForSystem());HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Gdiplus::GdiplusStartupInput startup;if(Gdiplus::GdiplusStartup(&gdiplus,&startup,nullptr)!=Gdiplus::Ok)throw std::runtime_error("Cannot initialize native UI drawing");INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);make_fonts();ready_icon=microphone_icon(RGB(25,164,99));recording_icon=microphone_icon(RGB(230,78,87));working_icon=microphone_icon(RGB(237,177,64));for(const wchar_t* name:{L"LocalSTT.Native.Host",L"LocalSTT.Native.Settings",L"LocalSTT.Native.Panel",L"LocalSTT.Native.Fluent",L"LocalSTT.Native.Popup"}){WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hbrBackground=wcscmp(name,L"LocalSTT.Native.Fluent")&&wcscmp(name,L"LocalSTT.Native.Host")?brush:nullptr;wc.lpszClassName=name;wc.hIcon=ready_icon;wc.style=wcscmp(name,L"LocalSTT.Native.Popup")==0?CS_DROPSHADOW:0;RegisterClassExW(&wc);}host=CreateWindowExW(WS_EX_TOOLWINDOW,L"LocalSTT.Native.Host",L"LocalSTT Native",WS_POPUP,0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),this);if(!host)throw std::runtime_error("Cannot create tray window");notification_target=host;taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");add_tray();SetTimer(host,1,25,nullptr);show_notifications();MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(settings&&IsWindowVisible(settings)&&message.hwnd!=popup&&IsDialogMessageW(settings,&message))continue;TranslateMessage(&message);DispatchMessageW(&message);}if(SUCCEEDED(com))CoUninitialize();return static_cast<int>(message.wParam);}
};
Ui::Ui(Config& config,UiCallbacks callbacks):impl_(std::make_unique<Impl>(config,std::move(callbacks))){}
Ui::~Ui()=default;
int Ui::run(){return impl_->run();}
void Ui::notify(std::string title,std::string message){impl_->notification(std::move(title),std::move(message));}
void Ui::refresh_settings(){if(auto h=impl_->notification_target.load())PostMessageW(h,refresh_message,0,0);}
}
