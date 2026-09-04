from __future__ import annotations

import os


if os.name == "nt":
    import ctypes

    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
else:
    user32 = None
    kernel32 = None


def get_foreground_window() -> int | None:
    if user32 is None:
        return None
    hwnd = int(user32.GetForegroundWindow())
    return hwnd or None


def set_foreground_window(hwnd: int | None) -> bool:
    if user32 is None or not hwnd:
        return False
    if user32.SetForegroundWindow(hwnd):
        return True

    # Windows only hands the foreground to a thread that owns the current input, which
    # a tray popup does not; attaching to the foreground thread is the documented way in.
    active = user32.GetForegroundWindow()
    if not active:
        return False
    other = user32.GetWindowThreadProcessId(active, None)
    mine = kernel32.GetCurrentThreadId()
    if not other or other == mine:
        return False
    user32.AttachThreadInput(other, mine, True)
    try:
        return bool(user32.SetForegroundWindow(hwnd))
    finally:
        user32.AttachThreadInput(other, mine, False)
