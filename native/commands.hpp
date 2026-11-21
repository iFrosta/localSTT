#pragma once
#include "common.hpp"
#include <optional>
#include <unordered_map>

namespace localstt {
// Shared by Commands and the settings window, so there is one writer of commands.json.
void set_command_enabled(const Config& config, const std::string& name, bool enabled);
class Commands {
public:
    explicit Commands(Config& config) : config_(config) {}
    std::optional<CommandMatch> match(const std::string& text);
    CommandOutcome execute(const std::string& text, const CommandMatch& match,
        const std::function<CommandOutcome(const Json&, const std::string&)>& app_handler = {},
        const std::function<bool()>& cancelled = {});
    Json statuses();
    void rebuild_index();
    void set_enabled(const std::string& name, bool enabled);
    static std::optional<std::string> match_pattern(const std::string& text, const std::string& pattern);
private:
    Config& config_;
    std::recursive_mutex mutex_;
    std::unordered_map<std::string, bool> probes_;
    Json commands_ = Json::array();
    std::string commands_signature_;
    Json apps_ = Json::array();
    double index_time_ = 0;
    Json load_commands();
    bool probe(const std::string& requirement);
    std::string unavailable_reason(const Json& command);
    void load_index(bool refresh);
    std::optional<Json> find_app(const std::string& query);
};
}
