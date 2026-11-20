#pragma once
#include "common.hpp"
#include <optional>

// Whether a newer release exists. Asked of the GitHub releases API and nothing else:
// one GET, no account, nothing about the machine. GitHub still sees the request, which
// is why `update_check_enabled` exists. Mirrors localstt/updates.py and shares its
// update-check.json, so either application can show what the other one found.
namespace localstt {
struct Release {
    std::string tag;       // as published, e.g. "v1.2.0"
    std::string version;   // without the leading "v"
    std::string url;
    std::string published; // ISO 8601, as GitHub writes it
    std::string label() const { return "LocalSTT " + version + " is available"; }
};

// "v1.2.3" -> {1,2,3}. A pre-release or build suffix is dropped rather than ordered;
// anything unparseable is empty and sorts as oldest.
std::vector<int> parse_version(std::string_view text);
// (1,1) beats (1,0,5): both sides are compared at equal length.
bool is_newer(std::string_view candidate, std::string_view current);
// "v1.2.0" -> "1.2.0": how a published tag is shown.
std::string tag_version(std::string_view tag);
// The latest-release document, or nullopt when it carries no tag.
std::optional<Release> parse_release(const Json& data, const std::string& repository);
std::string releases_url(const Json& config);

struct LastCheck {
    double checked_at = 0;
    std::string when;    // local "YYYY-MM-DD HH:MM"
    std::string latest;  // the published tag, or "" when GitHub could not be reached
    std::string current; // the version that asked
    // Answered about a version that is no longer the one running.
    bool stale() const { return !current.empty() && current != version; }
    // A newer release that this version has not yet been told about elsewhere.
    bool offers_update() const { return !stale() && is_newer(latest, version); }
};
std::optional<LastCheck> last_check(const fs::path& state_dir);
void remember_check(const fs::path& state_dir, const std::string& latest, double now);
bool check_due(const Json& config, const fs::path& state_dir, double now);
double unix_now();

// The forced check behind "Check now": one request, remembered either way. Returns a
// release only when it is newer than this build; being offline is not an error.
std::optional<Release> check_for_update(const Json& config, const fs::path& state_dir, int timeout_seconds = 6);
}
