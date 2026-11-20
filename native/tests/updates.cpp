#include "updates.hpp"
#include <iostream>
#include <stdexcept>

using namespace localstt;
static void check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
int main() {
    try {
        check(parse_version("v1.2.3") == std::vector<int>{1, 2, 3}, "Leading v");
        check(parse_version(" 1.2.0-rc1 ") == std::vector<int>{1, 2, 0}, "Pre-release suffix dropped");
        check(parse_version("2.0+build.7") == std::vector<int>{2, 0}, "Build metadata dropped");
        check(parse_version("1.x.3") == std::vector<int>{1}, "Stops at the first part without digits");
        check(parse_version("").empty() && parse_version("latest").empty(), "Unparseable is empty");
        check(is_newer("1.1", "1.0.5"), "Shorter but newer");
        check(!is_newer("1.0.0", "1.0"), "Equal at equal length");
        check(is_newer("v1.0.1", "1.0.0") && !is_newer("0.9.9", "1.0.0"), "Ordering");
        check(!is_newer("", "1.0.0") && !is_newer("nightly", "0.0.1"), "Unparseable never newer");
        check(is_newer("10.0.0", "9.9.9"), "Numeric, not lexical");

        const Json release = {{"tag_name", "v1.2.0"}, {"html_url", "https://github.com/iFrosta/localSTT/releases/tag/v1.2.0"},
                              {"published_at", "2026-11-01T10:00:00Z"}, {"assets", Json::array()}};
        auto parsed = parse_release(release, "iFrosta/localSTT");
        check(parsed && parsed->tag == "v1.2.0" && parsed->version == "1.2.0", "Release tag parsed");
        check(parsed->url == "https://github.com/iFrosta/localSTT/releases/tag/v1.2.0", "Release page kept");
        check(parsed->published == "2026-11-01T10:00:00Z" && parsed->label() == "LocalSTT 1.2.0 is available", "Release details");
        auto foreign = parse_release({{"tag_name", "2.0.0"}, {"html_url", "https://example.invalid/"}}, "iFrosta/localSTT");
        check(foreign && foreign->url == "https://github.com/iFrosta/localSTT/releases/latest", "Only GitHub pages are offered");
        check(!parse_release({{"message", "Not Found"}}, "iFrosta/localSTT"), "No tag, no release");
        check(!parse_release(Json::array(), "iFrosta/localSTT"), "Not an object");

        check(releases_url({{"update_repository", "iFrosta/localSTT"}}) == "https://github.com/iFrosta/localSTT/releases/latest", "Releases URL");
        check(releases_url({{"update_repository", " "}}).empty(), "Blank repository disables updates");
        check(releases_url({{"update_repository", "owner/repo/../../x"}}).empty(), "Repository cannot escape the API path");

        auto root = fs::temp_directory_path() / ("localstt-update-test-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        Json config = {{"update_repository", "iFrosta/localSTT"}, {"update_check_interval_hours", 24.0}};
        check(!last_check(root), "Nothing checked yet");
        check(check_due(config, root, 1'000'000), "Due when never checked");
        remember_check(root, "v99.0.0", 1'000'000);
        auto last = last_check(root);
        check(last && last->latest == "v99.0.0" && last->current == version && !last->stale(), "Check remembered");
        check(last->when.size() == 16 && last->offers_update(), "Newer release offered from the stored answer");
        check(!check_due(config, root, 1'000'000 + 3600), "Not due within the interval");
        check(check_due(config, root, 1'000'000 + 24 * 3600), "Due after the interval");
        config["update_check_interval_hours"] = 0;
        check(check_due(config, root, 1'000'000), "A zero interval always checks");
        // The Python application writes the same file; read what it writes.
        write_json(root / "update-check.json", {{"checked_at", 1'000'000.5}, {"checked_at_iso", "x"}, {"latest", ""}, {"current", "0.0.1"}});
        last = last_check(root);
        check(last && last->latest.empty() && last->stale() && !last->offers_update(), "Unreachable answer from another version is stale");
        write_json(root / "update-check.json", Json::array());
        check(!last_check(root) && check_due(config, root, 0), "Malformed state is treated as never checked");
        fs::remove_all(root);
        std::cout << "Update version comparison and release parsing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
