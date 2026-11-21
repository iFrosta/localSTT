#include "updates.hpp"
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>

namespace localstt {
namespace {
struct Internet {
    HINTERNET handle = nullptr;
    ~Internet() { if (handle) WinHttpCloseHandle(handle); }
};
fs::path state_file(const fs::path& state_dir) { return state_dir / L"update-check.json"; }
bool valid_repository(std::string_view repo) {
    const auto slash = repo.find('/');
    if (slash == std::string_view::npos || !slash || slash + 1 == repo.size() || repo.find('/', slash + 1) != std::string_view::npos) return false;
    return std::all_of(repo.begin(), repo.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/';
    });
}
std::string repository(const Json& config) {
    auto repo = trim(config.value("update_repository", std::string()));
    return valid_repository(repo) ? repo : std::string();
}
// One HTTPS GET through the system's WinHTTP: no TLS library to ship or keep patched.
std::string https_get(const std::wstring& host, const std::wstring& path, int timeout_seconds) {
    const auto agent = L"LocalSTT/" + wide(version);
    Internet session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle) throw std::runtime_error("WinHTTP unavailable: " + std::to_string(GetLastError()));
    const int ms = std::clamp(timeout_seconds, 1, 120) * 1000;
    WinHttpSetTimeouts(session.handle, ms, ms, ms, ms);
    Internet connection{WinHttpConnect(session.handle, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.handle) throw std::runtime_error("Cannot connect to " + utf8(host));
    Internet request{WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.handle) throw std::runtime_error("Cannot create the update request");
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(request.handle, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
        throw std::runtime_error("No answer from " + utf8(host) + " (" + std::to_string(GetLastError()) + ")");
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) throw std::runtime_error("GitHub answered HTTP " + std::to_string(status));
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) throw std::runtime_error("Update response was cut off");
        if (!available) break;
        if (body.size() + available > 4 * 1024 * 1024) throw std::runtime_error("Update response is unexpectedly large");
        const auto old = body.size();
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, body.data() + old, available, &read)) throw std::runtime_error("Update response was cut off");
        body.resize(old + read);
    }
    return body;
}
}

double unix_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string tag_version(std::string_view tag) {
    const auto first = tag.find_first_not_of("vV");
    return first == std::string_view::npos ? std::string() : std::string(tag.substr(first));
}

std::vector<int> parse_version(std::string_view text) {
    auto cleaned = tag_version(trim(text));
    cleaned = cleaned.substr(0, cleaned.find('-'));
    cleaned = cleaned.substr(0, cleaned.find('+'));
    std::vector<int> parts;
    std::size_t start = 0;
    while (start <= cleaned.size()) {
        const auto end = std::min(cleaned.find('.', start), cleaned.size());
        std::string digits;
        for (auto c : cleaned.substr(start, end - start)) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        if (digits.empty()) break;
        parts.push_back(digits.size() > 9 ? 999999999 : std::stoi(digits));
        start = end + 1;
    }
    return parts;
}

bool is_newer(std::string_view candidate, std::string_view current) {
    auto left = parse_version(candidate), right = parse_version(current);
    if (left.empty()) return false;
    const auto width = std::max(left.size(), right.size());
    left.resize(width, 0);
    right.resize(width, 0);
    return left > right;
}

std::optional<Release> parse_release(const Json& data, const std::string& repo) {
    if (!data.is_object()) return std::nullopt;
    const auto text = [&](const char* key) { return data.contains(key) && data[key].is_string() ? data[key].get<std::string>() : std::string(); };
    auto tag = text("tag_name");
    if (tag.empty()) return std::nullopt;
    Release release;
    release.tag = tag;
    release.version = tag_version(tag);
    release.url = text("html_url");
    // Only a GitHub page is ever opened from here, whatever the response says.
    if (!release.url.starts_with("https://github.com/")) release.url = "https://github.com/" + repo + "/releases/latest";
    release.published = text("published_at");
    return release;
}

std::string releases_url(const Json& config) {
    const auto repo = repository(config);
    return repo.empty() ? std::string() : "https://github.com/" + repo + "/releases/latest";
}

std::optional<LastCheck> last_check(const fs::path& state_dir) {
    Json data;
    try { data = read_json(state_file(state_dir), Json()); } catch (...) { return std::nullopt; }
    if (!data.is_object() || !data.contains("checked_at") || !data["checked_at"].is_number()) return std::nullopt;
    LastCheck result;
    result.checked_at = data["checked_at"].get<double>();
    const auto seconds = static_cast<std::time_t>(result.checked_at);
    std::tm local{};
    if (result.checked_at < 0 || localtime_s(&local, &seconds)) return std::nullopt;
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    result.when = buffer;
    result.latest = data.contains("latest") && data["latest"].is_string() ? data["latest"].get<std::string>() : "";
    result.current = data.contains("current") && data["current"].is_string() ? data["current"].get<std::string>() : "";
    return result;
}

void remember_check(const fs::path& state_dir, const std::string& latest, double now) {
    try {
        const auto seconds = static_cast<std::time_t>(now);
        std::tm utc{};
        gmtime_s(&utc, &seconds);
        char iso[40]{};
        std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S+00:00", &utc);
        write_json(state_file(state_dir), {{"checked_at", now}, {"checked_at_iso", iso}, {"latest", latest}, {"current", version}});
    } catch (const std::exception& error) {
        // Losing the timestamp only means checking again sooner than necessary.
        log(std::string("Cannot record the update check: ") + error.what());
    }
}

bool check_due(const Json& config, const fs::path& state_dir, double now) {
    const double hours = std::max(0.0, config.value("update_check_interval_hours", 24.0));
    if (hours <= 0) return true;
    double last = 0;
    try {
        const auto data = read_json(state_file(state_dir), Json());
        if (!data.is_object() || !data.contains("checked_at") || !data["checked_at"].is_number()) return true;
        last = data["checked_at"].get<double>();
    } catch (...) { return true; }
    return now - last >= hours * 3600;
}

std::optional<Release> check_for_update(const Json& config, const fs::path& state_dir, int timeout_seconds) {
    const auto repo = repository(config);
    if (repo.empty()) return std::nullopt;
    std::optional<Release> release;
    try {
        release = parse_release(Json::parse(https_get(L"api.github.com", L"/repos/" + wide(repo) + L"/releases/latest", timeout_seconds)), repo);
    } catch (const std::exception& error) {
        log(std::string("update check failed: ") + error.what());
        remember_check(state_dir, "", unix_now());
        return std::nullopt;
    }
    const auto tag = release ? release->tag : std::string();
    remember_check(state_dir, tag, unix_now());
    if (!release || !is_newer(release->version, version)) {
        log("update check: " + std::string(version) + " is current (latest published is " + (tag.empty() ? "none" : tag) + ")");
        return std::nullopt;
    }
    log("update available: " + release->version + " (running " + version + ")");
    return release;
}
}
