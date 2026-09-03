from __future__ import annotations

import ctypes
import os
import time
from ctypes import wintypes


# pynput sets its own argtypes on ctypes.windll.user32.SendInput, and that function
# object is shared process-wide. Loading a private handle keeps our INPUT structs and
# pynput's from clobbering each other ("expected LP_INPUT instance" TypeErrors).
if os.name == "nt":
    user32 = ctypes.WinDLL("user32", use_last_error=True)
else:
    user32 = None


INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]


class INPUTUNION(ctypes.Union):
    # The union must carry every member: SendInput rejects the call outright when
    # cbSize is not sizeof(INPUT) (40 bytes on x64), which silently returns 0.
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", INPUTUNION)]


if user32 is not None:
    user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
    user32.SendInput.restype = wintypes.UINT


last_error = 0


def type_unicode_text(text: str, interval: float = 0.001) -> bool:
    if user32 is None:
        return False

    for unit in _utf16_units(text):
        if not _send_unicode_unit(unit, key_up=False):
            return False
        if not _send_unicode_unit(unit, key_up=True):
            return False
        if interval > 0:
            time.sleep(interval)
    return True


def _utf16_units(text: str) -> list[int]:
    data = text.encode("utf-16-le", "surrogatepass")
    return [int.from_bytes(data[i : i + 2], "little") for i in range(0, len(data), 2)]


def _send_unicode_unit(unit: int, *, key_up: bool) -> bool:
    global last_error
    flags = KEYEVENTF_UNICODE | (KEYEVENTF_KEYUP if key_up else 0)
    ki = KEYBDINPUT(0, unit, flags, 0, 0)
    u = INPUTUNION()
    u.ki = ki
    event = INPUT(INPUT_KEYBOARD, u)
    ctypes.set_last_error(0)
    if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) == 1:
        return True
    last_error = ctypes.get_last_error()
    return False
