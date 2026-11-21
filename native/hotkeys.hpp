#pragma once
#include "common.hpp"
#include <algorithm>
#include <optional>
#include <set>
#include <utility>

// Hotkey chords: one naming of a key for config.json, the keyboard hook and the
// settings window. The names are the ones localstt/hotkeys.py writes, so the Python
// and native applications read each other's bindings from the shared config.json.
namespace localstt {
using KeyChord = std::set<std::string>;

// "Control_L" -> "ctrl", "Escape" -> "esc": sides and aliases fold into one name.
std::string normalise_key(std::string_view name);
// Named after the physical key, never what it typed: Ctrl+D is the control
// character 0x04 and a Cyrillic layout turns the same key into another letter.
std::optional<std::string> key_name(unsigned virtual_key);
// "ctrl+shift+win" -> {"ctrl","shift","win"}. Blank means the binding is off.
KeyChord parse_chord(std::string_view text);
// The form written back to config.json: modifiers first, in Windows' order.
std::string format_chord(const KeyChord& keys);
// The form shown in the UI and the log: "Ctrl + Shift + Win", or "Not set".
std::string chord_label(const KeyChord& keys);
std::string chord_label(std::string_view text);

struct Bindings {
    // dictation, cleanup, command; an empty chord is a binding the user cleared.
    std::vector<std::pair<std::string, KeyChord>> modes;
    KeyChord cancel;
    static Bindings from_config(const Json& config);
    // The most specific binding fully held. Bindings overlap on purpose (cleanup is
    // dictation plus Shift), so the longest chord wins.
    std::optional<std::string> match(const KeyChord& held) const;
    bool cancels(const KeyChord& held) const { return !cancel.empty() && std::includes(held.begin(), held.end(), cancel.begin(), cancel.end()); }
    // Every key that is part of some binding: delivery waits until these are up.
    KeyChord keys() const;
    // Whether pressing Win now continues a binding, so the Start menu must not see it.
    bool continues_with_win(const KeyChord& held) const;
    bool operator==(const Bindings&) const = default;
};

// Two actions on one chord would leave whichever loses the tie unreachable.
// Returns the message to show, or nullopt when the settings can be saved.
std::optional<std::string> hotkey_conflict(const Json& values);

// Records one chord: every key pressed until all of them are released again.
class ChordRecorder {
public:
    void press(const std::string& key) { pressed_.insert(key); seen_.insert(key); }
    // True once the last key is up: Ctrl+Shift+Win is three presses and three releases.
    bool release(const std::string& key) { pressed_.erase(key); return !seen_.empty() && pressed_.empty(); }
    const KeyChord& seen() const { return seen_; }
    void clear() { pressed_.clear(); seen_.clear(); }
private:
    KeyChord pressed_, seen_;
};

// While a settings field records a chord the keyboard hook forwards every key to this
// window as capture_message(wParam = virtual key, lParam = 1 when pressed) and acts on
// none of them, or recording Ctrl+Win would start a dictation.
constexpr UINT capture_message = WM_APP + 40;
void set_capture_window(HWND window);
HWND capture_window();
}
