#include "dictionary.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace localstt;
static void check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
static void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) throw std::runtime_error("Could not write a test file");
}
int main(int argc, char** argv) {
    try {
        auto root = fs::temp_directory_path() / ("localstt-dictionary-test-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root / "state");
        const auto path = root / "dictionary.json";

        auto missing = Dictionary::load(path);
        check(missing.initial_prompt().empty() && missing.apply(L"cuda") == L"cuda", "Missing file is an empty dictionary");
        check(!missing.stale(path), "A file still missing is not stale");

        write_json(path, {{"terms", {"Whisper", "CUDA"}},
                          {"replacements", {{"kuda", "CUDA"}, {"docker compose", "Docker Compose"}, {"docker", "Docker"},
                                            {"гит хаб", "GitHub"}}}});
        check(missing.stale(path), "A file that appeared is stale");
        auto dictionary = Dictionary::load(path);
        check(dictionary.initial_prompt() == "Technical terms and names: Whisper, CUDA", "Language-neutral prompt");
        check(dictionary.term_count() == 2 && dictionary.replacement_count() == 4, "Counts");
        check(dictionary.apply(L"Kuda and docker compose up") == L"CUDA and Docker Compose up", "Longest phrase first, any case");
        check(dictionary.apply(L"kudalike pre-kuda kuda.") == L"kudalike pre-kuda CUDA.", "Whole words only, hyphen joins words");
        check(dictionary.apply(L"открой Гит Хаб") == L"открой GitHub", "Cyrillic replacements");
        check(!dictionary.stale(path), "Unchanged file is current");

        write_json(path, {{"terms", {"Whisper", "CUDA", "Ollama"}}, {"replacements", Json::object()}});
        check(dictionary.stale(path), "An edit makes it stale");
        check(dictionary.stale(root / "state" / "dictionary.json"), "Another file makes it stale");
        dictionary = Dictionary::load(path);
        check(dictionary.initial_prompt().ends_with("Ollama") && dictionary.apply(L"kuda") == L"kuda", "Reloaded contents");

        std::string terms = "[";
        for (int i = 0; i < 100; ++i) terms += std::string(i ? "," : "") + "\"Термин" + std::to_string(i) + "\"";
        write_text(path, terms + "]");
        check(Dictionary::load(path).initial_prompt().empty(), "A list instead of an object is empty, not a crash");
        write_json(path, {{"terms", Json::parse(terms + "]")}});
        dictionary = Dictionary::load(path);
        // 27 prefix characters, 100 terms of 7-8 Cyrillic letters and digits, 99 separators.
        check(dictionary.prompt_overflow() > 0 && dictionary.prompt_overflow() < dictionary.initial_prompt().size() - prompt_char_budget,
              "Overflow counts characters, not UTF-8 bytes");
        write_text(path, "{ not json");
        check(Dictionary::load(path).initial_prompt().empty(), "Malformed JSON is an empty dictionary");

        Config config(root / "state", root);
        check(config.user_file(L"dictionary.json") == path, "Bundled copy when the user has none");
        write_json(root / "state" / "dictionary.json", {{"terms", {"Mine"}}});
        check(config.user_file(L"dictionary.json") == root / "state" / "dictionary.json", "The user's copy wins");
        check(config.editable_file(L"cleanup-prompt.txt") == root / "state" / "cleanup-prompt.txt", "Nothing to edit in place");

        const auto prompt = root / "cleanup-prompt.txt";
        check(load_cleanup_prompt(prompt) == default_cleanup_prompt(), "Missing prompt uses the built-in one");
        write_text(prompt, " \r\n\t ");
        check(load_cleanup_prompt(prompt) == default_cleanup_prompt(), "Empty prompt uses the built-in one");
        write_text(prompt, "\xEF\xBB\xBF  Summarise in German.\r\nKeep names.\r\n");
        check(load_cleanup_prompt(prompt) == "Summarise in German.\nKeep names.", "BOM, CRLF and surrounding whitespace normalised");
        check(config.editable_file(L"cleanup-prompt.txt") == prompt, "A writable install folder is edited in place");
        if (argc > 1) {
            // The prompt shipped in the repository must stay what the Python app uses too.
            auto shipped = read_text(fs::path(argv[1]) / "cleanup-prompt.txt");
            check(shipped.has_value(), "cleanup-prompt.txt ships with the application");
            check(load_cleanup_prompt(fs::path(argv[1]) / "cleanup-prompt.txt") == default_cleanup_prompt(),
                  "Built-in prompt matches cleanup-prompt.txt");
            auto shipped_dictionary = Dictionary::load(fs::path(argv[1]) / "dictionary.json");
            check(shipped_dictionary.term_count() > 0 && shipped_dictionary.prompt_overflow() == 0, "Shipped dictionary fits Whisper's prompt");
        }
        fs::remove_all(root);
        std::cout << "Dictionary, cleanup prompt and user file tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
