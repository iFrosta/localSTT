#include "commands.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <set>
#include <sstream>
#include <stdexcept>

namespace localstt {
namespace {
using Microsoft::WRL::ComPtr;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool word(wchar_t c) {
    WORD kind = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &kind);
    return c == L'_' || (kind & (C1_ALPHA | C1_DIGIT));
}
std::wstring normalize(std::string_view text, bool pattern = true) {
    auto s = wide(text);
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    std::replace(s.begin(), s.end(), L'ё', L'е');
    for (size_t pos = 0; (pos = s.find(L"black out", pos)) != std::wstring::npos;)
        s.replace(pos, 9, L"blackout");
    std::wstring out;
    for (wchar_t c : s) {
        const bool keep = word(c) || (pattern && (c == L'*' || c == L'.' || c == L'-'));
        if (keep) out += c;
        else if (!out.empty() && out.back() != L' ') out += L' ';
    }
    return trim(out);
}
std::wstring expand(std::wstring text) {
    DWORD size = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (!size) return text;
    std::wstring result(size, L'\0');
    if (!ExpandEnvironmentStringsW(text.c_str(), result.data(), size)) return text;
    result.resize(size - 1);
    if (result == L"~" || result.starts_with(L"~\\") || result.starts_with(L"~/")) {
        wchar_t profile[32768];
        auto count = GetEnvironmentVariableW(L"USERPROFILE", profile, 32768);
        if (count && count < 32768) result.replace(0, 1, profile);
    }
    return result;
}
fs::path resolve(const std::string& raw) {
    auto text = expand(trim(wide(raw)));
    if (text.empty()) return {};
    fs::path path(text);
    if (path.is_absolute()) return path;
    wchar_t buffer[32768];
    DWORD size = SearchPathW(nullptr, text.c_str(), path.has_extension() ? nullptr : L".exe", 32768, buffer, nullptr);
    return size && size < 32768 ? fs::path(buffer) : fs::path();
}
std::wstring quote(const std::wstring& s) {
    // Microsoft CRT argument quoting, including trailing backslashes.
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
bool process(const fs::path& executable, const std::vector<std::wstring>& args, bool hidden,
             const fs::path& cwd = {}, DWORD wait_ms = 0) {
    if (executable.empty()) return false;
    std::wstring line = quote(executable.wstring());
    for (const auto& arg : args) line += L" " + quote(arg);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE,
                        hidden ? CREATE_NO_WINDOW : 0, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &info)) return false;
    Handle thread{info.hThread}, child{info.hProcess};
    if (!wait_ms) return true;
    if (WaitForSingleObject(child.value, wait_ms) != WAIT_OBJECT_0) {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 5000);
        return false;
    }
    DWORD exit_code = 1;
    return GetExitCodeProcess(child.value, &exit_code) && exit_code == 0;
}
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
Json enumerate_apps() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct CoScope { bool owns; ~CoScope() { if (owns) CoUninitialize(); } } scope{SUCCEEDED(init)};
    ComPtr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) return Json::array();
    ComPtr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) return Json::array();
    Json apps = Json::array();
    ComPtr<IShellItem> item;
    while (items->Next(1, &item, nullptr) == S_OK) {
        PWSTR display = nullptr, id = nullptr;
        ComPtr<IShellItem2> properties;
        if (SUCCEEDED(item.As(&properties))) properties->GetString(PKEY_AppUserModel_ID, &id);
        if (!id || !*id) {
            CoTaskMemFree(id); id = nullptr;
            item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &id);
        }
        item->GetDisplayName(SIGDN_NORMALDISPLAY, &display);
        if (display && id) {
            std::wstring appid(id);
            const auto slash = appid.find_last_of(L'\\');
            if (appid.starts_with(L"::{") && slash != std::wstring::npos) appid.erase(0, slash + 1);
            if (appid.starts_with(L"shell:AppsFolder\\")) appid.erase(0, 17);
            if (!appid.empty()) apps.push_back({{"name", utf8(display)}, {"appid", utf8(appid)}});
        }
        CoTaskMemFree(display);
        CoTaskMemFree(id);
        item.Reset();
    }
    return apps;
}
// Shorthand nobody spells out the way the Start menu does, seeded into
// app-aliases.json on first use and editable from Settings. Speech in a language not
// written in the Latin alphabet is handled by transliteration below, not by entries
// here. The same list as localstt/app_index.py.
Json default_aliases() {
    return {{"calc", "Calculator"}, {"explorer", "File Explorer"}, {"files", "File Explorer"},
        {"file manager", "File Explorer"}, {"my computer", "File Explorer"}, {"cmd", "Command Prompt"},
        {"command line", "Command Prompt"}, {"powershell", "Windows PowerShell"}, {"store", "Microsoft Store"},
        {"chrome", "Google Chrome"}, {"vs code", "Visual Studio Code"}, {"vscode", "Visual Studio Code"},
        {"docker", "Docker Desktop"}, {"photoshop", "Adobe Photoshop"}, {"task man", "Task Manager"}};
}
std::wstring transliterate(const std::wstring& text) {
    static const std::wstring alphabet = L"абвгдежзийклмнопрстуфхцчшщъыьэюя";
    static const std::vector<std::wstring> latin = {L"a",L"b",L"v",L"g",L"d",L"e",L"zh",L"z",L"i",L"y",
        L"k",L"l",L"m",L"n",L"o",L"p",L"r",L"s",L"t",L"u",L"f",L"h",L"c",L"ch",L"sh",L"sch",L"",L"y",L"",L"e",L"yu",L"ya"};
    std::wstring result;
    for (auto c : text) {
        auto pos = alphabet.find(c);
        result += pos == std::wstring::npos ? std::wstring(1, c) : latin[pos];
    }
    return result;
}
// Ratcliff/Obershelp matching blocks, the same core ratio used by SequenceMatcher.
size_t matching_chars(std::wstring_view a, std::wstring_view b) {
    size_t best = 0, ai = 0, bi = 0;
    std::vector<size_t> previous(b.size() + 1), next(b.size() + 1);
    for (size_t i = 0; i < a.size(); ++i) {
        std::fill(next.begin(), next.end(), 0);
        for (size_t j = 0; j < b.size(); ++j) if (a[i] == b[j]) {
            next[j + 1] = previous[j] + 1;
            if (next[j + 1] > best) { best = next[j + 1]; ai = i + 1 - best; bi = j + 1 - best; }
        }
        previous.swap(next);
    }
    return best ? best + matching_chars(a.substr(0, ai), b.substr(0, bi)) +
        matching_chars(a.substr(ai + best), b.substr(bi + best)) : 0;
}
double ratio(const std::wstring& a, const std::wstring& b) {
    return a.empty() && b.empty() ? 1 : 2.0 * matching_chars(a, b) / (a.size() + b.size());
}
std::set<std::wstring> words(const std::wstring& text) {
    std::wistringstream in(text);
    std::set<std::wstring> result;
    for (std::wstring word; in >> word;) result.insert(word);
    return result;
}
double score(const std::wstring& query, const std::wstring& name) {
    if (query.empty() || name.empty()) return 0;
    if (query == name) return 1;
    if (name.starts_with(query)) return .92;
    const auto q = words(query), n = words(name);
    if (std::includes(n.begin(), n.end(), q.begin(), q.end())) return .88;
    auto best = ratio(query, name);
    const auto latin = transliterate(query);
    if (latin != query) best = std::max(best, name.starts_with(latin) ? .9 : ratio(latin, name));
    return best;
}
}

std::optional<std::string> Commands::match_pattern(const std::string& raw_text, const std::string& raw_pattern) {
    const auto text = normalize(raw_text), pattern = normalize(raw_pattern);
    if (pattern.empty() || text.empty()) return std::nullopt;
    auto star = pattern.find(L'*');
    if (star == std::wstring::npos) {
        if (text.starts_with(pattern) && (text.size() == pattern.size() || !word(text[pattern.size()]))) return "";
        return std::nullopt;
    }
    if (pattern.find(L'*', star + 1) != std::wstring::npos) return std::nullopt;
    if (star == 0) {
        auto needle = trim(pattern.substr(1));
        if (needle.empty()) return std::nullopt;
        for (size_t pos = 0; (pos = text.find(needle, pos)) != std::wstring::npos; ++pos) {
            auto end = pos + needle.size();
            if ((!pos || !word(text[pos - 1])) && (end == text.size() || !word(text[end]))) return "";
        }
        return std::nullopt;
    }
    const auto head = trim(pattern.substr(0, star)), tail = trim(pattern.substr(star + 1));
    if (!text.starts_with(head) || text.size() <= head.size() || text[head.size()] != L' ') return std::nullopt;
    auto capture = trim(text.substr(head.size()));
    if (tail.empty()) return capture.empty() ? std::nullopt : std::optional(utf8(capture));
    // Python's tail permits punctuation after the suffix, but not another word.
    for (size_t pos = 0; (pos = capture.find(tail, pos)) != std::wstring::npos; ++pos) {
        if (!pos || capture[pos - 1] != L' ') continue;
        const auto rest = capture.substr(pos + tail.size());
        if (std::any_of(rest.begin(), rest.end(), word)) continue;
        auto result = trim(capture.substr(0, pos));
        if (!result.empty()) return utf8(result);
    }
    return std::nullopt;
}

Json Commands::load_commands() {
    const auto path = config_.user_file(L"commands.json");
    Json document;
    try {
        document = read_json(path);
        if (!document.is_object()) throw std::runtime_error("commands.json must be an object");
    } catch (const std::exception& error) {
        log(std::string("Voice commands unavailable: ") + error.what());
        commands_ = Json::array();
        commands_signature_.clear();
        probes_.clear();
        return commands_;
    }
    auto signature = document.dump();
    if (signature != commands_signature_) {
        commands_signature_ = signature;
        commands_ = document.value("commands", Json::array());
        if (!commands_.is_array()) commands_ = Json::array();
        probes_.clear();
    }
    return commands_;
}

bool Commands::probe(const std::string& requirement) {
    if (auto it = probes_.find(requirement); it != probes_.end()) return it->second;
    bool result = false;
    const auto colon = requirement.find(':');
    const auto kind = colon == std::string::npos ? "" : requirement.substr(0, colon);
    const auto rest = colon == std::string::npos ? requirement : requirement.substr(colon + 1);
    if (kind == "appx") {
        auto family = wide(rest);
        auto split = family.rfind(L'_');
        if (split != std::wstring::npos && split && split + 1 < family.size()) {
            auto prefix = family.substr(0, split) + L"_", suffix = L"__" + family.substr(split + 1);
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
                0, KEY_READ, &key) == ERROR_SUCCESS) {
                for (DWORD i = 0;; ++i) {
                    wchar_t name[1024]; DWORD size = 1024;
                    const auto code = RegEnumKeyExW(key, i, name, &size, nullptr, nullptr, nullptr, nullptr);
                    if (code == ERROR_NO_MORE_ITEMS) break;
                    if (code != ERROR_SUCCESS && code != ERROR_MORE_DATA) break;
                    if (code == ERROR_SUCCESS) {
                        std::wstring value(name, size);
                        if (value.starts_with(prefix) && value.ends_with(suffix)) { result = true; break; }
                    }
                }
                RegCloseKey(key);
            }
        }
    } else if (kind == "wsl") {
        const auto delimiter = rest.find(':');
        if (delimiter != std::string::npos && delimiter && delimiter + 1 < rest.size())
            result = process(resolve("wsl.exe"), {L"-d", wide(rest.substr(0, delimiter)), L"--", L"sh", L"-lc",
                L"command -v -- \"$1\" >/dev/null 2>&1", L"localstt-probe", wide(rest.substr(delimiter + 1))}, true, {}, 20000);
    } else {
        auto target = resolve(kind == "path" || kind == "exe" ? rest : requirement);
        std::error_code ec;
        result = !target.empty() && fs::exists(target, ec);
    }
    probes_[requirement] = result;
    return result;
}

std::string Commands::unavailable_reason(const Json& command) {
    if (!command.is_object()) return "invalid command";
    if (!command.value("enabled", true)) return "disabled in commands.json";
    const auto requirements = command.value("requires", Json::array());
    if (requirements.is_array()) for (const auto& spec : requirements)
        if (!spec.is_string() || !probe(spec.get<std::string>())) return "requirement not met: " + spec.dump();
    const auto type = command.value("type", "");
    if (type != "process") return type == "localstt" || type == "microsoft_todo" || type == "app_launch" ? "" : "unsupported command type";
    auto target = resolve(command.value("command", ""));
    std::error_code ec;
    if (target.empty() || !fs::is_regular_file(target, ec)) return "executable not installed: " + command.value("command", "");
    auto extension = target.extension().wstring();
    CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    if (extension != L".exe" && extension != L".cmd" && extension != L".bat" && extension != L".ps1" && extension != L".vbs") return "unsupported executable extension";
    if (extension == L".ps1" && resolve("pwsh.exe").empty()) return "pwsh.exe not installed";
    if (extension == L".vbs" && resolve("wscript.exe").empty()) return "wscript.exe not installed";
    if (command.contains("args") && !command["args"].is_array()) return "args must be an array";
    if (command.contains("args")) for (const auto& arg : command["args"])
        if (!arg.is_string()) return "arguments must be strings";
    return "";
}

Json Commands::statuses() {
    std::lock_guard lock(mutex_);
    probes_.clear();
    Json result = Json::array();
    for (const auto& command : load_commands()) {
        try {
            auto reason = unavailable_reason(command);
            result.push_back({{"command", command}, {"available", reason.empty()}, {"reason", reason}});
        } catch (const std::exception& error) {
            result.push_back({{"command", command}, {"available", false}, {"reason", error.what()}});
        }
    }
    return result;
}

std::optional<CommandMatch> Commands::match(const std::string& text) {
    std::lock_guard lock(mutex_);
    if (!config_.snapshot().value("commands_enabled", true)) return std::nullopt;
    for (const auto& command : load_commands()) {
        try {
            if (!unavailable_reason(command).empty()) continue;
            const auto patterns = command.value("patterns", Json::array());
            if (!patterns.is_array()) continue;
            for (const auto& pattern : patterns) if (pattern.is_string()) {
                if (auto capture = match_pattern(text, pattern.get<std::string>())) return CommandMatch{command, *capture};
            }
        } catch (const std::exception& error) { log(std::string("Skipping invalid voice command: ") + error.what()); }
    }
    return std::nullopt;
}

void set_command_enabled(const Config& config, const std::string& name, bool enabled) {
    // Under an all-users install the shipped file is read-only; the edit then lands on
    // a copy next to config.json that every later read prefers.
    const auto path = config.editable_file(L"commands.json");
    auto document = read_json(path);
    bool found = false;
    if (document.is_object() && document.contains("commands") && document["commands"].is_array())
        for (auto& command : document["commands"]) if (command.is_object() && command.value("name", "") == name) {
            // Enabled is the default, so the file only ever records the exception.
            if (enabled) command.erase("enabled"); else command["enabled"] = false;
            found = true;
        }
    if (!found) throw std::runtime_error("Unknown command: " + name);
    write_json(path, document);
}

void Commands::set_enabled(const std::string& name, bool enabled) {
    std::lock_guard lock(mutex_);
    set_command_enabled(config_, name, enabled);
    commands_signature_.clear();
}

void Commands::load_index(bool refresh) {
    if (!refresh && !apps_.empty() && now_seconds() - index_time_ < 86400) return;
    const auto path = config_.state_dir / "app-index.json";
    Json cached = Json::object();
    try {
        cached = read_json(path);
        if (!cached.is_object() || (cached.contains("ts") && !cached["ts"].is_number())) cached = Json::object();
    } catch (const std::exception& error) { log(std::string("Invalid app cache: ") + error.what()); }
    if (cached.contains("apps") && cached["apps"].is_array()) apps_ = cached["apps"];
    index_time_ = cached.value("ts", 0.0);
    if (!refresh && !apps_.empty() && now_seconds() - index_time_ < 86400) return;
    auto rebuilt = enumerate_apps();
    if (!rebuilt.empty()) {
        apps_ = rebuilt;
        index_time_ = now_seconds();
        write_json(path, {{"ts", index_time_}, {"apps", apps_}});
    }
}

void Commands::rebuild_index() {
    std::lock_guard lock(mutex_);
    probes_.clear();
    load_index(true);
}

std::optional<Json> Commands::find_app(const std::string& query) {
    load_index(false);
    const auto alias_path = config_.state_dir / "app-aliases.json";
    if (!fs::exists(alias_path)) write_json(alias_path, default_aliases());
    Json aliases = default_aliases();
    try { aliases = read_json(alias_path, aliases); }
    catch (const std::exception& error) { log(std::string("Invalid app aliases: ") + error.what()); }
    auto candidate = normalize(query, false);
    if (candidate.size() > 512) candidate.resize(512);
    while (candidate.size() >= 2) {
        if (aliases.is_object()) for (auto it = aliases.begin(); it != aliases.end(); ++it) {
            if (!it.value().is_string() || normalize(it.key(), false) != candidate) continue;
            auto target = normalize(it.value().get<std::string>(), false);
            for (const auto& app : apps_) if (app.is_object() && normalize(app.value("name", ""), false) == target) return app;
        }
        std::optional<Json> best;
        double best_score = 0;
        for (const auto& app : apps_) if (app.is_object()) {
            auto value = score(candidate, normalize(app.value("name", ""), false));
            if (value > best_score) { best = app; best_score = value; }
        }
        if (best && best_score >= .72) return best;
        const auto end = candidate.rfind(L' ');
        if (end == std::wstring::npos) break;
        candidate.resize(end);
    }
    return std::nullopt;
}

CommandOutcome Commands::execute(const std::string& text, const CommandMatch& matched,
    const std::function<CommandOutcome(const Json&, const std::string&)>& app_handler,
    const std::function<bool()>& cancelled) {
    std::lock_guard lock(mutex_);
    const auto& command = matched.command;
    const auto name = command.value("name", "unnamed");
    CommandOutcome result{false, "No command matched", name};
    const auto check_cancelled = [&] {
        if (cancelled && cancelled()) throw std::runtime_error("Voice command cancelled");
    };
    try {
        check_cancelled();
        // Reject stale or fabricated matches, including commands disabled during inference.
        probes_.clear();
        auto current = match(text);
        check_cancelled();
        if (!current || current->command != command || current->capture != matched.capture)
            throw std::runtime_error("Command no longer matches the enabled allowlist");
        const auto type = command.value("type", "");
        if (type == "localstt") {
            check_cancelled();
            result = app_handler ? app_handler(command, matched.capture) : CommandOutcome{false, "App command needs the tray application", name};
        } else if (type == "microsoft_todo") {
            auto title = trim(wide(matched.capture));
            const auto first = title.find_first_not_of(L" .,;:!?\"'");
            if (first == std::wstring::npos) throw std::runtime_error("Todo title was empty");
            title = title.substr(first, title.find_last_not_of(L" .,;:!?\"'") - first + 1);
            check_cancelled();
            append_json(config_.state_dir / "todo-queue.jsonl", {{"ts", timestamp()}, {"title", utf8(title)},
                {"list", command.value("list", "Tasks")}, {"status", "queued_locally"}});
            result = {true, "Todo queued locally: " + utf8(title), name};
        } else if (type == "app_launch") {
            auto app = find_app(matched.capture);
            if (!app) throw std::runtime_error("No installed application matched: " + matched.capture);
            auto target = L"shell:AppsFolder\\" + wide(app->at("appid").get<std::string>());
            check_cancelled();
            if (!process(resolve("explorer.exe"), {target}, false)) throw std::runtime_error("Application launch failed");
            result = {true, "Opened: " + app->at("name").get<std::string>(), name};
        } else if (type == "process") {
            auto target = resolve(command.value("command", ""));
            auto extension = target.extension().wstring();
            CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
            std::vector<std::wstring> args;
            if (extension == L".ps1") args = {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File", target.wstring()};
            if (extension == L".vbs") args = {target.wstring()};
            for (const auto& arg : command.value("args", Json::array())) args.push_back(expand(wide(arg.get<std::string>())));
            auto exe = extension == L".ps1" ? resolve("pwsh.exe") : extension == L".vbs" ? resolve("wscript.exe") : target;
            const bool hidden = !command.value("show_console", false) &&
                (extension == L".ps1" || extension == L".cmd" || extension == L".bat");
            if (extension == L".cmd" || extension == L".bat") {
                // Only trusted, fixed commands.json arguments enter this command line.
                // Captured speech is deliberately never substituted into process actions.
                std::wstring script = quote(target.wstring());
                for (const auto& arg : args) script += L" " + quote(arg);
                auto cmd = resolve("cmd.exe");
                std::wstring line = quote(cmd.wstring()) + L" /d /s /c \"" + script + L"\"";
                STARTUPINFOW si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                check_cancelled();
                if (!CreateProcessW(cmd.c_str(), line.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0,
                                    nullptr, target.parent_path().c_str(), &si, &pi)) throw std::runtime_error("Script launch failed");
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            } else {
                check_cancelled();
                if (!process(exe, args, hidden, target.parent_path())) throw std::runtime_error("Process launch failed");
            }
            result = {true, "Executed: " + name, name};
        } else throw std::runtime_error("Unsupported command type: " + type);
    } catch (const std::exception& error) { result = {false, error.what(), name}; }
    append_json(config_.state_dir / "command-history.jsonl", {{"ts", timestamp()}, {"text", text},
        {"matched", name}, {"executed", result.executed}, {"message", result.message}});
    return result;
}
}
