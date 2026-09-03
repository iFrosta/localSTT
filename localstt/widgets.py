"""Fluent controls for the settings window.

Tk ships nothing that looks like Windows 11, so the handful of controls the settings
window needs -- a card, a toggle, a dropdown, a text field, a button -- are drawn here
on canvases against the palette in `winui`.
"""

from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont
from typing import Callable, Sequence

from . import winui
from .tray_menu import ICON_FONT, ICON_FONT_FALLBACK, MenuItem, Win11Menu

GLYPH_CHEVRON_DOWN = "\ue70d"  # ChevronDown, Segoe Fluent Icons


class Theme:
    """Palette, DPI scale and fonts, resolved once and shared by every control."""

    def __init__(self, master: tk.Misc) -> None:
        self.colors = winui.palette()
        self.scale = winui.Scale()
        families = set(tkfont.families(master))
        family = winui.FONT_FAMILY if winui.FONT_FAMILY in families else winui.FONT_FALLBACK
        display = (
            winui.FONT_FAMILY_DISPLAY if winui.FONT_FAMILY_DISPLAY in families else family
        )
        self.font = tkfont.Font(root=master, family=family, size=10)
        self.font_small = tkfont.Font(root=master, family=family, size=9)
        self.font_bold = tkfont.Font(root=master, family=family, size=10, weight="bold")
        self.font_title = tkfont.Font(root=master, family=display, size=18, weight="bold")
        self.font_section = tkfont.Font(root=master, family=display, size=12, weight="bold")
        self.font_mono = tkfont.Font(root=master, family="Cascadia Mono", size=9)
        icon_family = ICON_FONT if ICON_FONT in families else ICON_FONT_FALLBACK
        self.font_icon = tkfont.Font(root=master, family=icon_family, size=10)
        self.font_icon_small = tkfont.Font(root=master, family=icon_family, size=8)

    def px(self, value: float) -> int:
        return self.scale.px(value)


class Card(tk.Frame):
    """A rounded Fluent surface that other widgets are packed into."""

    def __init__(self, parent: tk.Misc, theme: Theme, *, padding: int = 14) -> None:
        self.theme = theme
        self.radius = theme.px(6)
        self._canvas = tk.Canvas(
            parent, bg=theme.colors.window, highlightthickness=0, bd=0, height=theme.px(56)
        )
        super().__init__(self._canvas, bg=theme.colors.card)
        self._pad = theme.px(padding)
        self._window = self._canvas.create_window(
            self._pad, self._pad, window=self, anchor="nw"
        )
        self.bind("<Configure>", self._resize)

    def _resize(self, _event=None) -> None:
        outer = self._canvas.winfo_width()
        if outer <= 1:
            return
        # The frame lives in a canvas window, which does not follow the canvas width.
        self._canvas.itemconfigure(self._window, width=outer - self._pad * 2)
        height = self.winfo_reqheight() + self._pad * 2
        self._canvas.configure(height=height)
        self._canvas.delete("bg")
        winui.round_rect(
            self._canvas, 1, 1, outer - 1, height - 1, self.radius,
            fill=self.theme.colors.card, outline=self.theme.colors.divider, tags="bg",
        )
        self._canvas.tag_lower("bg")

    def pack_card(self, **kwargs) -> None:
        self._canvas.pack(fill="x", **kwargs)
        self._canvas.bind("<Configure>", self._resize)


class ToggleSwitch(tk.Canvas):
    """The Windows 11 pill switch.

    Tk's canvas does not anti-alias, and a rounded polygon at this size reads as a
    lozenge with stepped edges. Pillow renders the same shape supersampled instead, so
    the track and knob come out as smooth as the system's own.
    """

    # Windows 11 proportions: a 40x20 track with a 12px knob that grows on hover.
    TRACK_WIDTH = 40
    TRACK_HEIGHT = 20
    KNOB_RADIUS = 6
    KNOB_RADIUS_HOVER = 7
    SUPERSAMPLE = 4

    def __init__(self, parent: tk.Misc, theme: Theme, value: bool, on_change: Callable[[bool], None]):
        self.theme = theme
        self.value = bool(value)
        self.on_change = on_change
        self._hover = False
        self._photo = None
        self._width = theme.px(self.TRACK_WIDTH)
        self._height = theme.px(self.TRACK_HEIGHT)
        super().__init__(
            parent, width=self._width, height=self._height,
            bg=parent["bg"], highlightthickness=0, bd=0, cursor="hand2",
        )
        self.bind("<Button-1>", self._toggle)
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self._draw()

    def _toggle(self, _event=None) -> None:
        self.value = not self.value
        self._draw()
        self.on_change(self.value)

    def set(self, value: bool) -> None:
        self.value = bool(value)
        self._draw()

    def _set_hover(self, hover: bool) -> None:
        self._hover = hover
        self._draw()

    def _draw(self) -> None:
        self.delete("all")
        photo = self._render()
        if photo is not None:
            self._photo = photo  # Tk keeps only a weak reference to the image
            self.create_image(0, 0, image=photo, anchor="nw")
            return
        self._draw_vector()

    def _geometry(self) -> tuple[str, str | None, str, float, float]:
        """track outline, track fill, knob colour, knob centre x, knob radius."""
        colors = self.theme.colors
        knob_radius = self.theme.px(
            self.KNOB_RADIUS_HOVER if self._hover else self.KNOB_RADIUS
        )
        track_radius = self._height / 2
        if self.value:
            return colors.accent, colors.accent, colors.on_accent, self._width - track_radius, knob_radius
        return colors.text_secondary, None, colors.text_secondary, track_radius, knob_radius

    def _render(self):
        try:
            from PIL import Image, ImageDraw, ImageTk
        except ImportError:
            return None

        scale = self.SUPERSAMPLE
        width, height = self._width * scale, self._height * scale
        outline, fill, knob, knob_cx, knob_r = self._geometry()

        image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        border = max(1, round(self.theme.px(1) * scale))
        draw.rounded_rectangle(
            [border / 2, border / 2, width - border / 2 - 1, height - border / 2 - 1],
            radius=height / 2,
            fill=fill,
            outline=outline,
            width=border,
        )
        cx, cy, r = knob_cx * scale, height / 2, knob_r * scale
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=knob)

        image = image.resize((self._width, self._height), Image.LANCZOS)
        try:
            return ImageTk.PhotoImage(image, master=self)
        except Exception:
            return None

    def _draw_vector(self) -> None:
        """Fallback for a machine where Pillow is not installed yet."""
        outline, fill, knob, knob_cx, knob_r = self._geometry()
        winui.pill(
            self, 1, 1, self._width - 1, self._height - 1,
            fill=fill or self["bg"], outline=outline,
        )
        cy = self._height / 2
        self.create_oval(
            knob_cx - knob_r, cy - knob_r, knob_cx + knob_r, cy + knob_r, fill=knob, outline=""
        )


class Button(tk.Canvas):
    def __init__(
        self,
        parent: tk.Misc,
        theme: Theme,
        text: str,
        command: Callable[[], None],
        *,
        primary: bool = False,
        width: int | None = None,
        background: str | None = None,
    ) -> None:
        self.theme = theme
        self.text = text
        self.command = command
        self.primary = primary
        self._hover = False
        self._bg = background or theme.colors.window
        self._width = width or (theme.font.measure(text) + theme.px(32))
        self._height = theme.px(32)
        super().__init__(
            parent, width=self._width, height=self._height,
            bg=self._bg, highlightthickness=0, bd=0, cursor="hand2",
        )
        self.bind("<Button-1>", lambda e: self.command())
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self._draw()

    def _set_hover(self, hover: bool) -> None:
        self._hover = hover
        self._draw()

    def _draw(self) -> None:
        self.delete("all")
        colors = self.theme.colors
        if self.primary:
            fill = colors.accent_hover if self._hover else colors.accent
            outline, text_color = fill, colors.on_accent
        else:
            fill = colors.hover if self._hover else colors.card
            outline, text_color = colors.divider, colors.text
        winui.round_rect(
            self, 1, 1, self._width - 1, self._height - 1, self.theme.px(4),
            fill=fill, outline=outline,
        )
        self.create_text(
            self._width / 2, self._height / 2, text=self.text,
            font=self.theme.font, fill=text_color, anchor="center",
        )


class Dropdown(tk.Canvas):
    """A combo box that opens the same Fluent popup the tray menu uses."""

    def __init__(
        self,
        parent: tk.Misc,
        theme: Theme,
        value: str,
        options: Sequence[str],
        on_change: Callable[[str], None],
        *,
        labels: dict[str, str] | None = None,
        width: int = 200,
    ) -> None:
        self.theme = theme
        self.value = value
        self.options = list(options)
        self.labels = labels or {}
        self.on_change = on_change
        self._hover = False
        self._width = theme.px(width)
        self._height = theme.px(32)
        super().__init__(
            parent, width=self._width, height=self._height,
            bg=theme.colors.card, highlightthickness=0, bd=0, cursor="hand2",
        )
        self.bind("<Button-1>", self._open)
        self.bind("<Enter>", lambda e: self._set_hover(True))
        self.bind("<Leave>", lambda e: self._set_hover(False))
        self._draw()

    def _label(self, value: str) -> str:
        return self.labels.get(value, value)

    def _set_hover(self, hover: bool) -> None:
        self._hover = hover
        self._draw()

    def set_options(self, options: Sequence[str], value: str | None = None) -> None:
        self.options = list(options)
        if value is not None:
            self.value = value
        self._draw()

    def _draw(self) -> None:
        self.delete("all")
        colors = self.theme.colors
        winui.round_rect(
            self, 1, 1, self._width - 1, self._height - 1, self.theme.px(4),
            fill=colors.hover if self._hover else colors.window,
            outline=colors.divider,
        )
        self.create_text(
            self.theme.px(12), self._height / 2, text=self._label(self.value),
            font=self.theme.font, fill=colors.text, anchor="w",
        )
        self.create_text(
            self._width - self.theme.px(12), self._height / 2, text=GLYPH_CHEVRON_DOWN,
            font=self.theme.font_icon_small, fill=colors.text_secondary, anchor="e",
        )

    def _open(self, _event=None) -> None:
        items = [
            MenuItem(
                label=self._label(option),
                action=(lambda value=option: self._choose(value)),
                checked=option == self.value,
                radio=True,
            )
            for option in self.options
        ] or [MenuItem(label="Nothing available", enabled=False)]

        menu = Win11Menu(items, master=self.winfo_toplevel())
        menu.show_at(self.winfo_rootx(), self.winfo_rooty() + self._height + self.theme.px(2))

    def _choose(self, value: str) -> None:
        self.value = value
        self._draw()
        self.on_change(value)


class TextField(tk.Frame):
    """A flat entry with the Fluent focus underline."""

    def __init__(
        self,
        parent: tk.Misc,
        theme: Theme,
        value: str,
        on_change: Callable[[str], None],
        *,
        width: int = 200,
    ) -> None:
        self.theme = theme
        self.on_change = on_change
        colors = theme.colors
        super().__init__(parent, bg=colors.divider, padx=1, pady=1)

        self.var = tk.StringVar(value=value)
        self.entry = tk.Entry(
            self,
            textvariable=self.var,
            font=theme.font,
            bg=colors.window,
            fg=colors.text,
            insertbackground=colors.text,
            relief="flat",
            highlightthickness=0,
            bd=0,
            width=1,
        )
        self.entry.pack(fill="both", expand=True, padx=theme.px(8))
        # Propagation off means the frame keeps the size we ask for, so it needs both.
        self.configure(width=theme.px(width), height=theme.px(32))
        self.pack_propagate(False)

        self.var.trace_add("write", lambda *_: self.on_change(self.var.get()))
        self.entry.bind("<FocusIn>", lambda e: self.configure(bg=colors.accent))
        self.entry.bind("<FocusOut>", lambda e: self.configure(bg=colors.divider))

    def set(self, value: str) -> None:
        self.var.set(value)


class SettingRow(tk.Frame):
    """Title and description on the left, the control on the right."""

    def __init__(
        self,
        parent: tk.Misc,
        theme: Theme,
        title: str,
        description: str = "",
        *,
        background: str | None = None,
    ) -> None:
        colors = theme.colors
        super().__init__(parent, bg=background or colors.card)
        self.theme = theme

        text = tk.Frame(self, bg=self["bg"])
        text.pack(side="left", fill="x", expand=True)
        tk.Label(
            text, text=title, font=theme.font, bg=self["bg"], fg=colors.text, anchor="w",
            justify="left",
        ).pack(anchor="w")
        if description:
            tk.Label(
                text, text=description, font=theme.font_small, bg=self["bg"],
                fg=colors.text_secondary, anchor="w", justify="left", wraplength=theme.px(420),
            ).pack(anchor="w", pady=(theme.px(2), 0))

        self.control_area = tk.Frame(self, bg=self["bg"])
        self.control_area.pack(side="right", padx=(theme.px(16), 0))
