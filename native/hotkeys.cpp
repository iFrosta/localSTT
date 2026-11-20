#include "hotkeys.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>

namespace localstt {
namespace {
// Written first when a chord is formatted, in the order Windows itself writes them.
constexpr const char* modifier_order[] = {"ctrl", "alt", "shift", "win"};
const std::map<std::string, std::string, std::less<>> aliases = {
    {"control", "ctrl"}, {"cmd", "win"}, {"super", "win"}, {"meta", "win"}, {"windows", "win"},
    {"escape", "esc"}, {"return", "enter"}, {"spacebar", "space"}};
const std::map<std::string, std::string, std::less<>> labels = {
    {"ctrl", "Ctrl"}, {"alt", "Alt"}, {"shift", "Shift"}, {"win", "Win"}, {"esc", "Esc"},
    {"enter", "Enter"}, {"space", "Space"}, {"tab", "Tab"}, {"backspace", "Backspace"},
    {"caps_lock", "Caps Lock"}, {"print_screen", "Print Screen"}};
// pynput's names for the keys it reports as keyboard.Key members.
const std::map<unsigned, std::string> named_keys = {
    {VK_LCONTROL, "ctrl"}, {VK_RCONTROL, "ctrl"}, {VK_CONTROL, "ctrl"},
    {VK_LMENU, "alt"}, {VK_RMENU, "alt"}, {VK_MENU, "alt"},
    {VK_LSHIFT, "shift"}, {VK_RSHIFT, "shift"}, {VK_SHIFT, "shift"},
    {VK_LWIN, "win"}, {VK_RWIN, "win"},
    {VK_ESCAPE, "esc"}, {VK_RETURN, "enter"}, {VK_SPACE, "space"}, {VK_TAB, "tab"}, {VK_BACK, "backspace"},
    {VK_CAPITAL, "caps_lock"}, {VK_SNAPSHOT, "print_screen"}, {VK_DELETE, "delete"}, {VK_INSERT, "insert"},
    {VK_HOME, "home"}, {VK_END, "end"}, {VK_PRIOR, "page_up"}, {VK_NEXT, "page_down"},
    {VK_LEFT, "left"}, {VK_RIGHT, "right"}, {VK_UP, "up"}, {VK_DOWN, "down"},
    {VK_PAUSE, "pause"}, {VK_SCROLL, "scroll_lock"}, {VK_NUMLOCK, "num_lock"}, {VK_APPS, "menu"},
    {VK_MEDIA_PLAY_PAUSE, "media_play_pause"}, {VK_MEDIA_NEXT_TRACK, "media_next"},
    {VK_MEDIA_PREV_TRACK, "media_previous"}, {VK_VOLUME_MUTE, "media_volume_mute"},
    {VK_VOLUME_DOWN, "media_volume_down"}, {VK_VOLUME_UP, "media_volume_up"},
    // Punctuation by its unshifted US character, which is what pynput reports with
    // no modifier held. "+" is left out: it separates the keys of a chord.
    {VK_OEM_1, ";"}, {VK_OEM_PLUS, "="}, {VK_OEM_COMMA, ","}, {VK_OEM_MINUS, "-"}, {VK_OEM_PERIOD, "."},
    {VK_OEM_2, "/"}, {VK_OEM_3, "`"}, {VK_OEM_4, "["}, {VK_OEM_5, "\\"}, {VK_OEM_6, "]"}, {VK_OEM_7, "'"},
    {VK_MULTIPLY, "*"}, {VK_SUBTRACT, "-"}, {VK_DIVIDE, "/"}, {VK_DECIMAL, "."}};
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}
std::atomic<HWND> capture_target{nullptr};
}

std::string normalise_key(std::string_view raw) {
    auto name = lower(trim(raw));
    std::replace(name.begin(), name.end(), ' ', '_');
    for (std::string_view suffix : {"_l", "_r", "_gr"}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            const auto base = name.substr(0, name.size() - suffix.size());
            if (base == "ctrl" || base == "alt" || base == "shift" || base == "cmd" || base == "win") { name = base; break; }
        }
    }
    const auto alias = aliases.find(name);
    return alias == aliases.end() ? name : alias->second;
}

std::optional<std::string> key_name(unsigned vk) {
    if (const auto named = named_keys.find(vk); named != named_keys.end()) return named->second;
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk - 'A' + 'a'));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "num" + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "f" + std::to_string(vk - VK_F1 + 1);
    return std::nullopt;
}

KeyChord parse_chord(std::string_view text) {
    KeyChord keys;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find('+', start), text.size());
        if (auto key = normalise_key(text.substr(start, end - start)); !key.empty()) keys.insert(std::move(key));
        start = end + 1;
    }
    return keys;
}

std::string format_chord(const KeyChord& keys) {
    std::string result;
    const auto add = [&](const std::string& key) { if (!result.empty()) result += '+'; result += key; };
    for (const char* modifier : modifier_order) if (keys.contains(modifier)) add(modifier);
    for (const auto& key : keys)
        if (std::none_of(std::begin(modifier_order), std::end(modifier_order), [&](const char* m) { return key == m; })) add(key);
    return result;
}

std::string chord_label(const KeyChord& keys) {
    if (keys.empty()) return "Not set";
    std::string result;
    const auto formatted = format_chord(keys);
    std::size_t start = 0;
    while (start < formatted.size()) {
        const auto end = std::min(formatted.find('+', start), formatted.size());
        const auto key = formatted.substr(start, end - start);
        std::string label;
        if (const auto known = labels.find(key); known != labels.end()) label = known->second;
        else if (key.size() == 1 || (key[0] == 'f' && key.size() > 1 && std::all_of(key.begin() + 1, key.end(), ::isdigit)))
            label = key, std::transform(label.begin(), label.end(), label.begin(), ::toupper);
        else {
            // "page_up" -> "Page Up", as Python's str.title() writes it.
            bool word_start = true;
            for (char c : key) {
                if (c == '_') { label += ' '; word_start = true; continue; }
                label += static_cast<char>(word_start ? std::toupper(static_cast<unsigned char>(c)) : c);
                word_start = !std::isalpha(static_cast<unsigned char>(c));
            }
        }
        if (!result.empty()) result += " + ";
        result += label;
        start = end + 1;
    }
    return result;
}
std::string chord_label(std::string_view text) { return chord_label(parse_chord(text)); }

Bindings Bindings::from_config(const Json& config) {
    Bindings result;
    for (const char* mode : {"dictation", "cleanup", "command"})
        result.modes.emplace_back(mode, parse_chord(config.value(std::string("hotkey_") + mode, std::string())));
    if (config.value("cancel_on_escape", true)) result.cancel = parse_chord(config.value("hotkey_cancel", std::string()));
    return result;
}

std::optional<std::string> Bindings::match(const KeyChord& held) const {
    std::optional<std::string> best;
    std::size_t best_size = 0;
    for (const auto& [mode, chord] : modes)
        if (!chord.empty() && chord.size() > best_size && std::includes(held.begin(), held.end(), chord.begin(), chord.end()))
            best = mode, best_size = chord.size();
    return best;
}

KeyChord Bindings::keys() const {
    KeyChord result;
    for (const auto& [mode, chord] : modes) result.insert(chord.begin(), chord.end());
    return result;
}

bool Bindings::continues_with_win(const KeyChord& held) const {
    auto next = held;
    next.erase("win");
    if (next.empty()) return false;
    next.insert("win");
    return std::any_of(modes.begin(), modes.end(), [&](const auto& binding) {
        return std::includes(binding.second.begin(), binding.second.end(), next.begin(), next.end());
    });
}

std::optional<std::string> hotkey_conflict(const Json& values) {
    const std::pair<const char*, const char*> titles[] = {
        {"hotkey_dictation", "Dictation"}, {"hotkey_cleanup", "Dictation with cleanup"},
        {"hotkey_command", "Voice command"}, {"hotkey_cancel", "Cancel"}};
    std::map<KeyChord, std::string> seen;
    for (const auto& [key, title] : titles) {
        if (std::string_view(key) == "hotkey_cancel" && !values.value("cancel_on_escape", true)) continue;
        const auto chord = parse_chord(values.value(key, std::string()));
        if (chord.empty()) continue;
        if (const auto other = seen.find(chord); other != seen.end())
            return other->second + " and " + title + " would share " + chord_label(chord);
        seen.emplace(chord, title);
    }
    return std::nullopt;
}

void set_capture_window(HWND window) { capture_target = window; }
HWND capture_window() { return capture_target; }
}
