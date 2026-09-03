"""Windows 11 chrome for the windows LocalSTT shows.

Tkinter draws a Windows 95 dialog unless it is told otherwise, so everything visual is
concentrated here: the system's own dark/light choice and accent colour read from the
registry, DWM's rounded corners and immersive dark title bar, and the DPI scaling that a
250%-scaled laptop screen makes non-negotiable.

Tk also owns its thread: widgets may only be touched from the thread running the
mainloop, and the tray icon's callbacks arrive on pystray's message loop. `UiThread`
bridges the two.
"""

from __future__ import annotations

import ctypes
import math
import queue
import threading
from ctypes import wintypes
from dataclasses import dataclass
from typing import Any, Callable

IS_WINDOWS = hasattr(ctypes, "windll")

# DwmSetWindowAttribute
DWMWA_USE_IMMERSIVE_DARK_MODE = 20
DWMWA_WINDOW_CORNER_PREFERENCE = 33
DWMWA_BORDER_COLOR = 34
DWMWA_CAPTION_COLOR = 35
DWMWA_SYSTEMBACKDROP_TYPE = 38

DWMWCP_ROUND = 2
DWMWCP_ROUNDSMALL = 3
DWMSBT_MAINWINDOW = 2  # Mica

_THEME_KEY = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"
_DWM_KEY = r"Software\Microsoft\Windows\DWM"


@dataclass(frozen=True)
class Palette:
    dark: bool
    window: str
    surface: str
    card: str
    border: str
    divider: str
    text: str
    text_secondary: str
    text_disabled: str
    hover: str
    pressed: str
    accent: str
    accent_hover: str
    on_accent: str
    danger: str


def is_dark_mode() -> bool:
    if not IS_WINDOWS:
        return True
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _THEME_KEY) as key:
            return int(winreg.QueryValueEx(key, "AppsUseLightTheme")[0]) == 0
    except (OSError, ValueError):
        return True


def accent_color() -> str:
    """The user's accent colour; the registry stores it as 0xAABBGGRR."""
    if not IS_WINDOWS:
        return "#0078d4"
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _DWM_KEY) as key:
            raw = int(winreg.QueryValueEx(key, "AccentColor")[0]) & 0xFFFFFFFF
    except (OSError, ValueError):
        return "#0078d4"
    return "#{:02x}{:02x}{:02x}".format(raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF)


def _mix(color: str, other: str, amount: float) -> str:
    a = [int(color[i : i + 2], 16) for i in (1, 3, 5)]
    b = [int(other[i : i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * amount):02x}" for x, y in zip(a, b))


def _luminance(color: str) -> float:
    r, g, b = (int(color[i : i + 2], 16) / 255 for i in (1, 3, 5))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def palette() -> Palette:
    dark = is_dark_mode()
    accent = accent_color()
    on_accent = "#ffffff" if _luminance(accent) < 0.55 else "#000000"

    if dark:
        return Palette(
            dark=True,
            window="#202020",
            surface="#2c2c2c",
            card="#272727",
            border="#1d1d1d",
            divider="#3a3a3a",
            text="#ffffff",
            text_secondary="#c7c7c7",
            text_disabled="#7a7a7a",
            hover="#383838",
            pressed="#333333",
            accent=accent,
            accent_hover=_mix(accent, "#ffffff", 0.15),
            on_accent=on_accent,
            danger="#ff99a4",
        )
    return Palette(
        dark=False,
        window="#f3f3f3",
        surface="#f9f9f9",
        card="#ffffff",
        border="#d1d1d1",
        divider="#e5e5e5",
        text="#1b1b1b",
        text_secondary="#5d5d5d",
        text_disabled="#a0a0a0",
        hover="#ededed",
        pressed="#e5e5e5",
        accent=accent,
        accent_hover=_mix(accent, "#000000", 0.12),
        on_accent=on_accent,
        danger="#c42b1c",
    )


DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4


def enable_dpi_awareness() -> bool:
    """Must run before any window exists, or Windows stretches every bitmap we draw.

    Python ships DPI-unaware, so on a 250%-scaled screen Tk renders at a third of the
    real resolution and the result is upscaled and blurry.
    """
    if not IS_WINDOWS:
        return False
    try:
        if ctypes.windll.user32.SetProcessDpiAwarenessContext(
            ctypes.c_void_p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        ):
            return True
    except (AttributeError, OSError):
        pass
    try:  # Windows 8.1 fallback
        return ctypes.windll.shcore.SetProcessDpiAwareness(2) == 0
    except (AttributeError, OSError):
        return False


def system_dpi() -> int:
    if not IS_WINDOWS:
        return 96
    try:
        return int(ctypes.windll.user32.GetDpiForSystem())
    except (AttributeError, OSError):
        pass
    try:
        dc = ctypes.windll.user32.GetDC(0)
        dpi = int(ctypes.windll.gdi32.GetDeviceCaps(dc, 88))  # LOGPIXELSX
        ctypes.windll.user32.ReleaseDC(0, dc)
        return dpi or 96
    except (AttributeError, OSError):
        return 96


class Scale:
    """Turns design pixels into screen pixels, and points into Tk's idea of points."""

    def __init__(self, dpi: int | None = None) -> None:
        self.dpi = dpi or system_dpi()
        self.factor = self.dpi / 96.0

    def px(self, value: float) -> int:
        return max(1, round(value * self.factor))

    def apply_to(self, tk_widget) -> None:
        # Tk sizes fonts in points using this ratio, so it must follow the real DPI.
        tk_widget.tk.call("tk", "scaling", self.dpi / 72.0)


def _dwm_set(hwnd: int, attribute: int, value: int) -> bool:
    if not IS_WINDOWS or not hwnd:
        return False
    try:
        result = ctypes.windll.dwmapi.DwmSetWindowAttribute(
            wintypes.HWND(hwnd),
            ctypes.c_uint(attribute),
            ctypes.byref(ctypes.c_int(value)),
            ctypes.sizeof(ctypes.c_int),
        )
        return result == 0
    except (AttributeError, OSError):
        return False


def apply_window_chrome(hwnd: int, colors: Palette, *, rounded: bool = True, small: bool = False) -> None:
    """Immersive dark title bar and Windows 11 rounded corners."""
    _dwm_set(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, 1 if colors.dark else 0)
    if rounded:
        _dwm_set(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUNDSMALL if small else DWMWCP_ROUND)
    bgr = _to_colorref(colors.window)
    _dwm_set(hwnd, DWMWA_CAPTION_COLOR, bgr)
    _dwm_set(hwnd, DWMWA_BORDER_COLOR, _to_colorref(colors.border))


def _to_colorref(color: str) -> int:
    r, g, b = (int(color[i : i + 2], 16) for i in (1, 3, 5))
    return (b << 16) | (g << 8) | r


def round_region(hwnd: int, width: int, height: int, radius: int) -> None:
    """Clip a borderless popup to rounded corners; DWM does not round WS_POPUP windows."""
    if not IS_WINDOWS or not hwnd:
        return
    try:
        region = ctypes.windll.gdi32.CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius)
        ctypes.windll.user32.SetWindowRgn(wintypes.HWND(hwnd), region, True)
    except (AttributeError, OSError):
        return


def hwnd_of(window) -> int:
    """The real top-level handle of a Tk window, not its intermediate wrapper."""
    try:
        handle = int(window.winfo_id())
    except Exception:
        return 0
    if not IS_WINDOWS:
        return handle
    try:
        parent = ctypes.windll.user32.GetParent(wintypes.HWND(handle))
        return int(parent) if parent else handle
    except (AttributeError, OSError):
        return handle


def work_area() -> tuple[int, int, int, int]:
    """Desktop area excluding the taskbar, in physical pixels."""
    if not IS_WINDOWS:
        return (0, 0, 1920, 1080)
    rect = wintypes.RECT()
    try:
        ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0)  # SPI_GETWORKAREA
    except (AttributeError, OSError):
        return (0, 0, 1920, 1080)
    return (rect.left, rect.top, rect.right, rect.bottom)


def cursor_position() -> tuple[int, int]:
    if not IS_WINDOWS:
        return (0, 0)
    point = wintypes.POINT()
    try:
        ctypes.windll.user32.GetCursorPos(ctypes.byref(point))
    except (AttributeError, OSError):
        return (0, 0)
    return (point.x, point.y)


FONT_FAMILY = "Segoe UI Variable Text"
FONT_FAMILY_DISPLAY = "Segoe UI Variable Display"
FONT_FALLBACK = "Segoe UI"


def _arc_points(cx: float, cy: float, r: float, start: float, end: float, steps: int) -> list[float]:
    points: list[float] = []
    for index in range(steps + 1):
        angle = math.radians(start + (end - start) * index / steps)
        points.extend((cx + r * math.cos(angle), cy + r * math.sin(angle)))
    return points


def rounded_points(x1: float, y1: float, x2: float, y2: float, r: float) -> list[float]:
    """The outline of a rounded rectangle, sampled along its corners.

    Tk's `smooth=True` fits a spline through the corner points, which visibly falls short
    of a real quarter circle -- and at r = height/2 it produces a lozenge rather than a
    pill. Sampling the arcs gives the true shape for both fills and outlines.
    """
    r = max(0.0, min(r, (x2 - x1) / 2, (y2 - y1) / 2))
    if r <= 0:
        return [x1, y1, x2, y1, x2, y2, x1, y2]

    steps = max(4, min(24, int(r / 1.5)))
    return (
        _arc_points(x2 - r, y1 + r, r, -90, 0, steps)
        + _arc_points(x2 - r, y2 - r, r, 0, 90, steps)
        + _arc_points(x1 + r, y2 - r, r, 90, 180, steps)
        + _arc_points(x1 + r, y1 + r, r, 180, 270, steps)
    )


def round_rect(canvas, x1: float, y1: float, x2: float, y2: float, r: float, **kwargs):
    return canvas.create_polygon(rounded_points(x1, y1, x2, y2, r), smooth=False, **kwargs)


def pill(canvas, x1: float, y1: float, x2: float, y2: float, **kwargs):
    """A stadium shape: the toggle track, and anything else fully rounded."""
    return round_rect(canvas, x1, y1, x2, y2, (y2 - y1) / 2.0, **kwargs)


class UiThread:
    """Owns the Tk mainloop so tray callbacks can open windows from any thread."""

    _instance: "UiThread | None" = None
    _lock = threading.Lock()

    def __init__(self) -> None:
        self._queue: queue.Queue[Callable[[], Any]] = queue.Queue()
        self._ready = threading.Event()
        self._root: Any = None
        self._thread = threading.Thread(target=self._run, name="localstt-ui", daemon=True)
        self._thread.start()
        self._ready.wait(timeout=10)

    @classmethod
    def instance(cls) -> "UiThread":
        with cls._lock:
            if cls._instance is None:
                cls._instance = UiThread()
            return cls._instance

    @property
    def root(self):
        return self._root

    def _run(self) -> None:
        import tkinter as tk

        self._root = tk.Tk()
        self._root.withdraw()
        Scale().apply_to(self._root)
        self._ready.set()
        self._pump()
        self._root.mainloop()

    def _pump(self) -> None:
        while True:
            try:
                task = self._queue.get_nowait()
            except queue.Empty:
                break
            try:
                task()
            except Exception:  # a broken window must not take the UI thread down
                import logging

                logging.getLogger("localstt").exception("UI task failed")
        self._root.after(30, self._pump)

    def submit(self, task: Callable[[], Any]) -> None:
        self._queue.put(task)
