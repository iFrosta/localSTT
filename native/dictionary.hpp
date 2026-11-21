#pragma once
#include "common.hpp"
#include <optional>
#include <utility>

// The text files a user edits to steer recognition and cleanup: dictionary.json and
// cleanup-prompt.txt. Both are re-read when they change, so an edit takes effect on the
// next dictation rather than the next restart.
namespace localstt {
// Whisper gives the prompt half of its 448-token window and silently drops the rest.
// Roughly three characters per token for mixed Cyrillic and Latin, kept pessimistic.
constexpr std::size_t prompt_char_budget = 600;

class Dictionary {
public:
    // A missing or malformed file is an empty dictionary, never a failed start.
    static Dictionary load(const fs::path& path);
    // Whether `current` is another file, or this one changed since it was read.
    bool stale(const fs::path& current) const;
    // "Technical terms and names: ..." in the language-neutral form localstt uses.
    std::string initial_prompt() const;
    // Characters past what Whisper will actually read, or 0.
    std::size_t prompt_overflow() const;
    // Replacements, longest phrase first, case-insensitive, on whole words only.
    std::wstring apply(std::wstring text) const;
    std::size_t term_count() const { return terms_.size(); }
    std::size_t replacement_count() const { return replacements_.size(); }
private:
    using Stamp = std::pair<fs::file_time_type, std::uintmax_t>;
    static std::optional<Stamp> stamp(const fs::path& path);
    fs::path source_;
    std::optional<Stamp> stamp_;
    std::vector<std::string> terms_;
    std::vector<std::pair<std::wstring, std::wstring>> replacements_;
};

// The instructions the cleanup model is given. Read on every cleanup: the point of an
// editable prompt is trying a wording and hearing the result straight away. An
// unreadable or empty file falls back to the built-in prompt.
std::string default_cleanup_prompt();
std::string load_cleanup_prompt(const fs::path& path);

// Whisper learned from subtitled video, and over a pause or background noise it can
// write the credits it saw there: "Субтитры создал DimaTorzok", "Продолжение следует",
// "Thanks for watching". True when a whole decoded segment is one of those, so the
// segment is dropped instead of typed. Only whole segments are matched: the same words
// inside a longer dictated sentence are kept.
bool is_hallucination(std::wstring_view segment);
// The same for decoder output. A segment cut inside a UTF-8 sequence is never dropped.
bool is_hallucination(std::string_view utf8_segment);
}
