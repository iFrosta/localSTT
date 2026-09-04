"""Hotkey chords: one naming of a key for config.json, the listener and the UI.

The bindings live in config.json as text ("ctrl+shift+win"), the global listener sees
pynput key objects and the settings window records a new chord with a listener of its
own. All three have to agree on what a key is called, or a chord the user just recorded
would never fire again, so every translation happens here.
"""

from __future__ import annotations

import threading
from typing import Callable, Iterable

from pynput import keyboard


# Written first when a chord is formatted, in the order Windows itself writes them.
MODIFIER_ORDER = ("ctrl", "alt", "shift", "win")

# Keys pynput reports once per side; the side never matters for a binding.
_SIDED = {"ctrl", "alt", "shift", "cmd", "win"}

_ALIASES = {
    "control": "ctrl",
    "cmd": "win",
    "super": "win",
    "meta": "win",
    "windows": "win",
    "escape": "esc",
    "return": "enter",
    "spacebar": "space",
}

_LABELS = {
    "ctrl": "Ctrl",
    "alt": "Alt",
    "shift": "Shift",
    "win": "Win",
    "esc": "Esc",
    "enter": "Enter",
    "space": "Space",
    "tab": "Tab",
    "backspace": "Backspace",
    "caps_lock": "Caps Lock",
    "print_screen": "Print Screen",
}

EMPTY_LABEL = "Not set"


def _vk_names() -> dict[int, str]:
    """Virtual key codes, because `char` is unusable once a modifier is held.

    Ctrl+D arrives as the control character \\x04 and a Cyrillic layout turns the same
    physical key into "в", so the chord is named after the key, not what it typed.
    """
    names = {code: chr(code).lower() for code in range(0x41, 0x5B)}  # A-Z
    names.update({code: chr(code) for code in range(0x30, 0x3A)})  # 0-9
    names.update({0x60 + digit: f"num{digit}" for digit in range(10)})
    return names


_VK_NAMES = _vk_names()


def normalise(name: str) -> str:
    name = str(name).strip().lower().replace(" ", "_")
    for suffix in ("_l", "_r", "_gr"):
        if name.endswith(suffix) and name[: -len(suffix)] in _SIDED:
            name = name[: -len(suffix)]
            break
    return _ALIASES.get(name, name)


def key_name(key) -> str | None:
    """The canonical name of a pynput key, or None for a key we cannot name."""
    if isinstance(key, keyboard.Key):
        return normalise(key.name)
    vk = getattr(key, "vk", None)
    if vk in _VK_NAMES:
        return _VK_NAMES[vk]
    char = getattr(key, "char", None)
    if char and char.isprintable() and not char.isspace():
        return normalise(char)
    return None


def parse_chord(text: str | None) -> frozenset[str]:
    """"ctrl+shift+win" -> {"ctrl", "shift", "win"}. Blank means the binding is off."""
    if not text:
        return frozenset()
    return frozenset(normalise(part) for part in str(text).split("+") if part.strip())


def sort_keys(keys: Iterable[str]) -> list[str]:
    keys = set(keys)
    ordered = [name for name in MODIFIER_ORDER if name in keys]
    return ordered + sorted(keys - set(MODIFIER_ORDER))


def format_chord(keys: Iterable[str]) -> str:
    """The form written back to config.json."""
    return "+".join(sort_keys(keys))


def key_label(name: str) -> str:
    if name in _LABELS:
        return _LABELS[name]
    if len(name) == 1 or (name[0] == "f" and name[1:].isdigit()):
        return name.upper()
    return name.replace("_", " ").title()


def chord_label(chord: str | Iterable[str] | None) -> str:
    """The form shown in the UI and the log: "Ctrl + Shift + Win"."""
    keys = parse_chord(chord) if isinstance(chord, (str, type(None))) else set(chord)
    if not keys:
        return EMPTY_LABEL
    return " + ".join(key_label(name) for name in sort_keys(keys))


_suspended = threading.Event()


def suspended() -> bool:
    """True while the settings window is recording a chord.

    The tray listener keeps running throughout, so it has to ignore the keys the user
    is pressing -- otherwise recording Ctrl+Win as a binding would start a dictation.
    """
    return _suspended.is_set()


class Capture:
    """Records one chord: every key pressed until they are all released again.

    A listener of its own rather than Tk key events, so the Windows key -- which Tk
    never sees on its own -- can be part of a binding, and so a chord is named by
    exactly the code that later has to recognise it.
    """

    def __init__(self, on_change: Callable[[str], None], on_done: Callable[[str], None]) -> None:
        self.on_change = on_change
        self.on_done = on_done
        self.pressed: set[str] = set()
        self.seen: set[str] = set()
        self.done = False
        self.listener: keyboard.Listener | None = None

    def start(self) -> None:
        if self.listener is not None:
            return
        _suspended.set()
        self.listener = keyboard.Listener(on_press=self._press, on_release=self._release)
        self.listener.start()

    def stop(self) -> None:
        self.done = True
        listener, self.listener = self.listener, None
        _suspended.clear()
        if listener is not None:
            listener.stop()

    def _press(self, key) -> None:
        name = key_name(key)
        if name is None or self.done:
            return
        self.pressed.add(name)
        self.seen.add(name)
        self.on_change(format_chord(self.seen))

    def _release(self, key) -> None:
        name = key_name(key)
        if self.done:
            return
        if name is not None:
            self.pressed.discard(name)
        # The chord is whatever was held together, so it is only complete once the
        # last key is up: Ctrl+Shift+Win is three presses and three releases.
        if self.seen and not self.pressed:
            self.done = True
            self.on_done(format_chord(self.seen))
