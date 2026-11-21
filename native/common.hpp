#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <mutex>
#include <functional>
#include <vector>
#include <optional>
// Generated from localstt/__init__.py by CMakeLists.txt, so both applications report
// and compare one version.
#include "version.h"
namespace localstt {
inline constexpr const char* version = LOCALSTT_VERSION;
using Json = nlohmann::json;
namespace fs = std::filesystem;
std::wstring wide(std::string_view s);
std::string utf8(std::wstring_view s);
std::string timestamp();
std::wstring trim(std::wstring s);
std::string trim(std::string_view s);
fs::path executable_dir();
Json read_json(const fs::path& path, const Json& fallback = Json::object());
// The whole file as UTF-8 without a byte order mark; nullopt when it cannot be read.
std::optional<std::string> read_text(const fs::path& path);
void write_json(const fs::path& path, const Json& value);
void append_json(const fs::path& path, const Json& value);
void log(std::string_view text);
void set_log_dir(const fs::path& path);
class Config {
public:
    Config(fs::path state, fs::path root);
    Json snapshot() const;
    void update(const Json& values);
    void reload();
    // commands.json, dictionary.json and cleanup-prompt.txt: the copy next to
    // config.json when there is one, else the one shipped in the install folder.
    // Resolved on every call, because an edit can create the user copy mid-session.
    fs::path user_file(std::wstring_view name) const;
    // Where an edit to one of those files goes: in place when the install folder is
    // writable (a portable install), otherwise a copy in the state folder that every
    // later user_file() call prefers.
    fs::path editable_file(std::wstring_view name) const;
    fs::path state_dir, install_dir;
private:
    mutable std::mutex mutex_;
    Json data_;
};
struct CommandOutcome { bool executed = false; std::string message; std::string command_name; };
struct CommandMatch { Json command; std::string capture; };
}
