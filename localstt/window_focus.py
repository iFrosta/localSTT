from __future__ import annotations

import os


if os.name == "nt":
    import ctypes

    user32 = ctypes.windll.user32
else:
    user32 = None


def get_foreground_window() -> int | None:
    if user32 is None:
        return None
    hwnd = int(user32.GetForegroundWindow())
    return hwnd or None


def set_foreground_window(hwnd: int | None) -> bool:
    if user32 is None or not hwnd:
        return False
    return bool(user32.SetForegroundWindow(hwnd))
