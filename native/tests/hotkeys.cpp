#include "hotkeys.hpp"
#include <iostream>
#include <stdexcept>

using namespace localstt;
static void check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
int main() {
    try {
        check(normalise_key(" Ctrl_L ") == "ctrl" && normalise_key("alt_gr") == "alt", "Sided modifiers fold to one name");
        check(normalise_key("control") == "ctrl", "Control alias");
        check(normalise_key("cmd_r") == "win" && normalise_key("Super") == "win", "Windows key aliases");
        check(normalise_key("Escape") == "esc" && normalise_key("Return") == "enter", "Named key aliases");
        check(normalise_key("caps lock") == "caps_lock", "Spaces become underscores");
        check(normalise_key("page_l") == "page_l", "Only modifiers lose a side suffix");

        check(parse_chord("ctrl+shift+win") == KeyChord{"ctrl", "shift", "win"}, "Chord parsing");
        check(parse_chord(" Ctrl + Win ") == KeyChord{"ctrl", "win"}, "Chord parsing tolerates spacing and case");
        check(parse_chord("").empty() && parse_chord("+").empty(), "Blank chord is off");
        check(format_chord({"win", "f8", "shift", "ctrl"}) == "ctrl+shift+win+f8", "Modifiers first, in Windows order");
        check(format_chord(parse_chord("alt+ctrl")) == "ctrl+alt", "Round trip normalises order");
        check(chord_label("ctrl+shift+win") == "Ctrl + Shift + Win", "Label of the default cleanup chord");
        check(chord_label("f8") == "F8" && chord_label("d") == "D", "Single keys and function keys upper-case");
        check(chord_label("ctrl+page_up") == "Ctrl + Page Up", "Other names are title-cased");
        check(chord_label("caps_lock") == "Caps Lock" && chord_label("num1") == "Num1", "Known labels");
        check(chord_label("") == "Not set", "Empty binding label");

        check(key_name(VK_LCONTROL) == "ctrl" && key_name(VK_RMENU) == "alt" && key_name(VK_RWIN) == "win", "Modifier names");
        check(key_name('D') == "d" && key_name('7') == "7" && key_name(VK_NUMPAD3) == "num3", "Letters, digits, numpad");
        check(key_name(VK_F12) == "f12" && key_name(VK_ESCAPE) == "esc" && key_name(VK_OEM_COMMA) == ",", "Function, escape, punctuation");
        check(!key_name(VK_ADD) && !key_name(0xff), "Unnameable keys");

        Json config = {{"hotkey_dictation", "ctrl+win"}, {"hotkey_cleanup", "ctrl+shift+win"},
                       {"hotkey_command", "ctrl+alt+win"}, {"hotkey_cancel", "esc"}, {"cancel_on_escape", true}};
        auto bindings = Bindings::from_config(config);
        check(bindings.match({"ctrl", "win"}) == "dictation", "Dictation chord");
        check(bindings.match({"ctrl", "shift", "win"}) == "cleanup", "Longest chord wins");
        check(bindings.match({"ctrl", "alt", "win"}) == "command", "Command chord");
        check(bindings.match({"ctrl", "win", "d"}) == "dictation", "Extra keys still hold the chord");
        check(!bindings.match({"ctrl"}) && !bindings.match({"shift", "win"}), "Partial chords do not match");
        check(bindings.cancels({"esc"}) && bindings.cancels({"esc", "ctrl"}) && !bindings.cancels({"ctrl"}), "Cancel chord");
        check(bindings.keys() == KeyChord{"alt", "ctrl", "shift", "win"}, "Chord keys");

        config["hotkey_dictation"] = "f8";
        config["hotkey_cleanup"] = "";
        config["cancel_on_escape"] = false;
        bindings = Bindings::from_config(config);
        check(bindings.match({"f8"}) == "dictation", "Custom push-to-talk key");
        check(!bindings.match({"ctrl", "shift", "win"}), "A cleared binding matches nothing");
        check(!bindings.cancels({"esc"}), "Cancel disabled by its switch");
        check(!(bindings == Bindings::from_config(Json{{"hotkey_dictation", "ctrl+win"}})), "Changed bindings compare unequal");

        Json values = {{"hotkey_dictation", "ctrl+win"}, {"hotkey_cleanup", "ctrl+shift+win"},
                       {"hotkey_command", "ctrl+alt+win"}, {"hotkey_cancel", "esc"}, {"cancel_on_escape", true}};
        check(!hotkey_conflict(values), "Defaults do not conflict");
        values["hotkey_command"] = "win+ctrl";
        auto conflict = hotkey_conflict(values);
        check(conflict && *conflict == "Dictation and Voice command would share Ctrl + Win", "Same chord in another order conflicts");
        values["hotkey_command"] = "";
        values["hotkey_cleanup"] = "";
        check(!hotkey_conflict(values), "Two cleared bindings do not conflict");
        values["hotkey_cancel"] = "ctrl+win";
        check(hotkey_conflict(values).has_value(), "Cancel conflicts with an action");
        values["cancel_on_escape"] = false;
        check(!hotkey_conflict(values), "A disabled cancel key cannot conflict");

        ChordRecorder recorder;
        recorder.press("ctrl");
        recorder.press("shift");
        check(!recorder.release("shift"), "Chord incomplete while a key is held");
        recorder.press("win");
        check(!recorder.release("ctrl"), "Still holding Win");
        check(recorder.release("win"), "Complete once every key is up");
        check(format_chord(recorder.seen()) == "ctrl+shift+win", "Recorder keeps every key seen");
        recorder.clear();
        check(!recorder.release("enter"), "A stray release records nothing");

        set_capture_window(reinterpret_cast<HWND>(1));
        check(capture_window() == reinterpret_cast<HWND>(1), "Capture target set");
        set_capture_window(nullptr);
        check(!capture_window(), "Capture target cleared");
        std::cout << "Hotkey naming, matching and conflict tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
