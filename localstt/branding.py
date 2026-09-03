"""The LocalSTT mark, shared by the tray icon and the window title bars.

Windows fixes the size of the notification-area cell, so a tray icon can only look
bigger by filling more of its own bitmap. The glyph is defined on a 64px grid and scaled
about its centre until it reaches the edge; ICON_SIZE only controls how crisply that
renders once Windows scales it down to the cell.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

from .config import APPDATA_DIR

ICON_SIZE = 256
GLYPH_SCALE = 1.29
ICON_PATH = APPDATA_DIR / "localstt.ico"

# Windows picks the closest entry, so the small ones stop the tray guessing.
ICO_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def render_icon(color: str, size: int = ICON_SIZE) -> Image.Image:
    unit = size / 64.0

    def box(*points: float) -> tuple[float, ...]:
        return tuple(((value - 32.0) * GLYPH_SCALE + 32.0) * unit for value in points)

    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.ellipse(
        box(8, 8, 56, 56),
        fill=color,
        outline="#202020",
        width=max(1, round(3 * GLYPH_SCALE * unit)),
    )
    draw.rectangle(box(29, 18, 35, 43), fill="white")
    draw.arc(
        box(21, 30, 43, 52), 0, 180,
        fill="white",
        width=max(1, round(4 * GLYPH_SCALE * unit)),
    )
    return image


def icon_file(color: str = "#19a463") -> Path:
    """An .ico on disk, because Tk title bars take a file path and not a PIL image."""
    if not ICON_PATH.exists():
        ICON_PATH.parent.mkdir(parents=True, exist_ok=True)
        render_icon(color).save(ICON_PATH, format="ICO", sizes=ICO_SIZES)
    return ICON_PATH
