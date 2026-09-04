"""Renders the settings window and saves the images the README uses.

Run it on Windows to get exactly what a user sees. It also runs anywhere else with Tk
and the Segoe fonts available -- which is how the committed images were made, under a
virtual X server -- because every Windows-only call in `winui` already no-ops off
Windows.

    python docs/capture-screenshots.py [--out docs/images] [--scale 1.5] [--light]
"""

from __future__ import annotations

import argparse
import logging
import queue
import sys
import time
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import ImageGrab

from localstt import settings_window, winui
from localstt.config import AppConfig

# Section key -> file name. Health, Voice commands and AI cleanup are left out: each
# reports the machine it ran on -- which commands resolve, which Ollama models are
# installed -- so a committed screenshot of one machine would document nothing. Pass
# --all to capture them anyway when writing about those pages.
SECTIONS = {
    "general": "settings-general.png",
    "hotkeys": "settings-hotkeys.png",
    "delivery": "settings-delivery.png",
}

MACHINE_SPECIFIC_SECTIONS = {
    "commands": "settings-commands.png",
    "cleanup": "settings-cleanup.png",
    "health": "settings-health.png",
}


def on_ui(ui: winui.UiThread, work: Callable[[], Any], timeout: float = 30.0) -> Any:
    """Run something on the thread that owns Tk and bring the result back.

    Everything has to go through here. Tk is single-threaded, and a widget built on one
    interpreter while its variables live on another silently renders empty.
    """
    answer: queue.Queue = queue.Queue(maxsize=1)

    def task() -> None:
        try:
            answer.put(("ok", work()))
        except Exception as exc:  # reported below, on the calling thread
            answer.put(("error", exc))

    ui.submit(task)
    status, value = answer.get(timeout=timeout)
    if status == "error":
        raise value
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "images")
    parser.add_argument("--scale", type=float, default=1.5,
                        help="render at this display scale; 1.5 keeps text crisp in the README")
    parser.add_argument("--light", action="store_true", help="capture the light theme")
    parser.add_argument("--all", action="store_true",
                        help="also capture the pages that describe this machine")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    # Both are read while the window is being built, so they have to be in place before
    # the UI thread starts.
    winui.system_dpi = lambda: round(96 * args.scale)
    if args.light:
        winui.is_dark_mode = lambda: False

    logger = logging.getLogger("capture")
    settings_window.open_settings(AppConfig(), logger)

    ui = winui.UiThread.instance()
    deadline = time.monotonic() + 30
    while settings_window._WINDOW is None and time.monotonic() < deadline:
        time.sleep(0.05)
    window = settings_window._WINDOW
    if window is None:
        print("the settings window did not open", file=sys.stderr)
        return 1

    sections = dict(SECTIONS)
    if args.all:
        sections.update(MACHINE_SPECIFIC_SECTIONS)

    suffix = "-light" if args.light else ""
    for key, name in sections.items():
        on_ui(ui, lambda k=key: window._show_section(k))
        # The cards measure themselves once they are on screen, so give the mainloop a
        # moment before asking where anything is.
        time.sleep(0.6)
        box = on_ui(ui, lambda: (
            window.window.winfo_rootx(),
            window.window.winfo_rooty(),
            window.window.winfo_rootx() + window.window.winfo_width(),
            window.window.winfo_rooty() + window.window.winfo_height(),
        ))
        path = args.out / name.replace(".png", f"{suffix}.png")
        ImageGrab.grab(bbox=box, all_screens=True).save(path)
        print(f"{path}  {box[2] - box[0]}x{box[3] - box[1]}")

    on_ui(ui, window.close)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
