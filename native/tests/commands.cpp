#include "commands.hpp"
#include <iostream>
#include <stdexcept>

using namespace localstt;
static void check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
int main(int argc, char** argv) {
    try {
        const auto matches = [](const std::string& text, const std::string& pattern) { return Commands::match_pattern(text, pattern); };
        check(matches("ВЫКЛЮЧИ подсветку.", "выключи подсветку") == "", "Cyrillic case normalization");
        check(matches("выключи подсветку пожалуйста", "выключи подсветку") == "", "Trailing courtesy");
        check(!matches("я сказал выключи подсветку", "выключи подсветку"), "Unanchored chatter rejected");
        check(!matches("выключи подсветкуновую", "выключи подсветку"), "Cyrillic word boundary");
        check(matches("вставь ещё раз", "вставь еще раз") == "", "Yo normalization");
        check(matches("создай задачу Купить молоко!", "создай задачу *") == "купить молоко", "Wildcard capture");
        check(!matches("создай задачу", "создай задачу *"), "Empty capture rejected");
        check(!matches("создай задачумолоко", "создай задачу *"), "Capture separator required");
        check(matches("поставь таймер на пять минут.", "поставь таймер на * минут") == "пять", "Infix capture punctuation");
        check(!matches("поставь таймер на пять минут завтра", "поставь таймер на * минут"), "Infix trailing word rejected");
        check(matches("сейчас black out пожалуйста", "*blackout") == "", "Opt-in anywhere");
        check(!matches("сейчас blackouts", "*blackout"), "Anywhere word boundary");
        check(!matches("something", "*"), "Empty wildcard rejected");
        check(!matches("создай задачу тест", "создай * *"), "Multiple wildcards rejected");
        check(!matches("anything", ""), "Empty pattern rejected");
        auto root = fs::temp_directory_path() / ("localstt-command-test-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        Config config(root / "state", root);
        Json command = {{"name", "safe"}, {"type", "localstt"}, {"patterns", {"тест"}}, {"action", "repeat"}};
        write_json(root / "commands.json", {{"commands", {command}}});
        Commands commands(config);
        auto found = commands.match("Тест.");
        check(found.has_value(), "Allowlisted match available");
        bool invoked = false;
        commands.set_enabled("safe", false);
        auto outcome = commands.execute("Тест.", *found, [&](const Json&, const std::string&) {
            invoked = true; return CommandOutcome{true, "unexpected", "safe"};
        });
        check(!outcome.executed && !invoked, "Stale disabled command never executes");
        commands.set_enabled("safe", true);
        config.update({{"commands_enabled", false}});
        check(!commands.match("тест"), "Global commands switch");
        config.update({{"commands_enabled", true}});
        auto forged = *found;
        forged.command["action"] = "arbitrary";
        check(!commands.execute("Тест.", forged).executed, "Forged action rejected");
        auto statuses = commands.statuses();
        check(statuses.size() == 1 && statuses[0]["available"] == true, "Statuses shape");
        invoked = false;
        auto cancelled = commands.execute("Тест.", *found, [&](const Json&, const std::string&) {
            invoked = true; return CommandOutcome{true, "unexpected", "safe"};
        }, [] { return true; });
        check(!cancelled.executed && !invoked, "Cancelled app command does not invoke callback");
        // Switch cancellation on after allowlist validation to cover a cancellation
        // arriving while potentially slow availability probes are running.
        int cancel_checks = 0;
        auto cancelled_late = commands.execute("Тест.", *found, [&](const Json&, const std::string&) {
            invoked = true; return CommandOutcome{true, "unexpected", "safe"};
        }, [&] { return ++cancel_checks >= 2; });
        check(!cancelled_late.executed && !invoked && cancel_checks == 2, "Cancellation after probes blocks callback");
        Json todo = {{"name", "todo"}, {"type", "microsoft_todo"}, {"patterns", {"создай задачу *"}}};
        write_json(root / "commands.json", {{"commands", {todo}}});
        auto todo_match = commands.match("создай задачу тест");
        check(todo_match.has_value(), "Todo fixture matches");
        cancel_checks = 0;
        check(!commands.execute("создай задачу тест", *todo_match, {}, [&] { return ++cancel_checks >= 3; }).executed,
              "Cancellation immediately before Todo write rejects action");
        check(!fs::exists(config.state_dir / "todo-queue.jsonl"), "Cancelled Todo never writes queue");
        if (argc > 1 && std::string(argv[1]) == "--index") {
            commands.rebuild_index();
            auto index = read_json(config.state_dir / "app-index.json");
            check(index.contains("apps") && !index["apps"].empty(), "Native AppsFolder index contains apps");
            for (const auto& app : index["apps"])
                check(app["name"].is_string() && app["appid"].is_string(), "App index schema");
            std::cout << "Indexed " << index["apps"].size() << " installed applications using COM\n";
        }
        write_json(root / "commands.json", Json::array());
        check(!commands.match("тест"), "Malformed command document fails closed");
        fs::remove_all(root);
        std::cout << "Voice command matching and allowlist tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
