"""A Windows 11 style context menu for the tray icon.

pystray shows the classic Win32 menu, which on Windows 11 looks a decade old next to
every other context menu on the desktop. This draws the Fluent one instead: rounded
window, rounded hover highlight, Segoe Fluent icon glyphs, accelerator hints, and
flyout submenus.
"""

from __future__ import annotations

import tkinter as tk
from dataclasses import dataclass, field
from tkinter import font as tkfont
from typing import Callable

from . import winui
from .window_focus import get_foreground_window, set_foreground_window

# Segoe Fluent Icons code points, with the Windows 10 icon font as the fallback.
ICON_FONT = "Segoe Fluent Icons"
ICON_FONT_FALLBACK = "Segoe MDL2 Assets"

GLYPH_CHECK = "\ue73e"      # CheckMark
GLYPH_BULLET = "\ue915"     # RadioBullet
GLYPH_CHEVRON = "\ue76c"    # ChevronRight


@dataclass
class MenuItem:
    label: str = ""
    action: Callable[[], None] | None = None
    icon: str = ""
    accelerator: str = ""
    checked: bool = False
    radio: bool = False
    enabled: bool = True
    submenu: list["MenuItem"] = field(default_factory=list)
    separator: bool = False


def separator() -> MenuItem:
    return MenuItem(separator=True)


# The menu the tray icon currently shows, so a second right-click replaces it instead of
# stacking another window on top.
_visible: "Win11Menu | None" = None


class Win11Menu:
    """One popup. Submenus are further Win11Menu instances anchored to their row."""

    def __init__(
        self,
        items: list[MenuItem],
        *,
        parent: "Win11Menu | None" = None,
        master: tk.Misc | None = None,
    ) -> None:
        self.items = [i for i in items if i.label or i.separator]
        self.parent = parent
        self.colors = winui.palette()
        self.scale = winui.Scale()
        self.child: "Win11Menu | None" = None
        self.hover_index: int | None = None
        self.owner_index: int | None = None
        self._closed = False
        self._timers: set[str] = set()
        self._had_foreground = False

        root = master or (parent.window if parent else winui.UiThread.instance().root)
        self.window = tk.Toplevel(root)
        self.window.withdraw()
        self.window.overrideredirect(True)
        self.window.configure(bg=self.colors.border)
        try:
            self.window.attributes("-topmost", True)
        except tk.TclError:
            pass

        self._build()

    # ------------------------------------------------------------------ layout

    def _px(self, value: float) -> int:
        return self.scale.px(value)

    def _build(self) -> None:
        self.pad = self._px(4)
        self.radius = self._px(8)
        self.item_height = self._px(32)
        self.separator_height = self._px(9)
        self.text_x = self._px(44)
        self.icon_x = self._px(24)

        families = set(tkfont.families(self.window))
        text_family = winui.FONT_FAMILY if winui.FONT_FAMILY in families else winui.FONT_FALLBACK
        icon_family = ICON_FONT if ICON_FONT in families else ICON_FONT_FALLBACK

        self.font = tkfont.Font(root=self.window, family=text_family, size=10)
        self.font_accel = tkfont.Font(root=self.window, family=text_family, size=9)
        self.font_icon = tkfont.Font(root=self.window, family=icon_family, size=10)

        width = self._measure_width()
        height = self.pad * 2 + sum(self._row_height(i) for i in self.items)

        self.canvas = tk.Canvas(
            self.window,
            width=width,
            height=height,
            bg=self.colors.surface,
            highlightthickness=0,
            bd=0,
        )
        # A one-pixel inset of the border colour reads as the Fluent hairline border.
        self.canvas.pack(padx=1, pady=1)
        self.width, self.height = width, height

        self.canvas.bind("<Motion>", self._on_motion)
        self.canvas.bind("<Leave>", lambda e: self._set_hover(None))
        self.canvas.bind("<Button-1>", self._on_click)
        self.window.bind("<Escape>", lambda e: self.close_all())
        self.window.bind("<FocusOut>", self._on_focus_out)
        self.window.bind("<Up>", lambda e: self._move_hover(-1))
        self.window.bind("<Down>", lambda e: self._move_hover(1))
        self.window.bind("<Return>", lambda e: self._activate_hover())

        self._draw()

    def _row_height(self, item: MenuItem) -> int:
        return self.separator_height if item.separator else self.item_height

    def _measure_width(self) -> int:
        longest = 0
        for item in self.items:
            if item.separator:
                continue
            width = self.font.measure(item.label)
            if item.accelerator:
                width += self._px(24) + self.font_accel.measure(item.accelerator)
            if item.submenu:
                width += self._px(24)
            longest = max(longest, width)
        return max(self._px(240), min(self._px(460), longest + self.text_x + self._px(20)))

    # ------------------------------------------------------------------ drawing

    def _row_top(self, index: int) -> int:
        return self.pad + sum(self._row_height(i) for i in self.items[:index])

    def _draw(self) -> None:
        self.canvas.delete("all")
        for index, item in enumerate(self.items):
            top = self._row_top(index)
            if item.separator:
                y = top + self.separator_height // 2
                self.canvas.create_line(
                    self._px(12), y, self.width - self._px(12), y, fill=self.colors.divider
                )
                continue
            self._draw_item(index, item, top)

    def _draw_item(self, index: int, item: MenuItem, top: int) -> None:
        hovered = index == self.hover_index and item.enabled
        if hovered:
            winui.round_rect(
                self.canvas,
                self.pad,
                top + self._px(1),
                self.width - self.pad,
                top + self.item_height - self._px(1),
                self._px(4),
                fill=self.colors.hover,
                outline="",
            )

        middle = top + self.item_height // 2
        if item.enabled:
            color = self.colors.text
        else:
            color = self.colors.text_disabled

        glyph = item.icon
        if item.checked:
            glyph = GLYPH_BULLET if item.radio else GLYPH_CHECK
        if glyph:
            self.canvas.create_text(
                self.icon_x, middle, text=glyph, font=self.font_icon,
                fill=self.colors.accent if item.checked else color, anchor="center",
            )

        self.canvas.create_text(
            self.text_x, middle, text=item.label, font=self.font, fill=color, anchor="w"
        )

        if item.accelerator:
            self.canvas.create_text(
                self.width - self._px(14), middle, text=item.accelerator,
                font=self.font_accel, fill=self.colors.text_secondary, anchor="e",
            )
        elif item.submenu:
            self.canvas.create_text(
                self.width - self._px(16), middle, text=GLYPH_CHEVRON,
                font=self.font_icon, fill=self.colors.text_secondary, anchor="e",
            )

    # ------------------------------------------------------------------ events

    def _index_at(self, y: int) -> int | None:
        for index, item in enumerate(self.items):
            top = self._row_top(index)
            if top <= y < top + self._row_height(item) and not item.separator:
                return index
        return None

    def _set_hover(self, index: int | None) -> None:
        if index == self.hover_index:
            return
        self.hover_index = index
        self._draw()

    def _on_motion(self, event) -> None:
        index = self._index_at(event.y)
        self._set_hover(index)
        if index is None:
            return
        item = self.items[index]
        if item.submenu and item.enabled:
            self._open_submenu(index, item)
        elif self.child is not None:
            self.child.close()
            self.child = None

    def _move_hover(self, delta: int) -> None:
        selectable = [i for i, item in enumerate(self.items) if not item.separator and item.enabled]
        if not selectable:
            return
        if self.hover_index in selectable:
            position = selectable.index(self.hover_index) + delta
        else:
            position = 0 if delta > 0 else len(selectable) - 1
        self._set_hover(selectable[position % len(selectable)])

    def _activate_hover(self) -> None:
        if self.hover_index is None:
            return
        self._invoke(self.items[self.hover_index])

    def _on_click(self, event) -> None:
        index = self._index_at(event.y)
        if index is None:
            return
        self._invoke(self.items[index])

    def _invoke(self, item: MenuItem) -> None:
        if not item.enabled or item.separator:
            return
        if item.submenu:
            return
        action = item.action
        self.close_all()
        if action is not None:
            action()

    def _later(self, delay: int, callback) -> None:
        """after() whose timer is cancelled when the window goes away.

        A timer left armed fires into a destroyed widget, and Tcl reports that as a
        background error rather than swallowing it. Each timer drops itself from the
        set as it runs, so a repeating one does not accumulate ids.
        """
        if self._closed:
            return
        holder: dict[str, str] = {}

        def run() -> None:
            self._timers.discard(holder.get("id", ""))
            callback()

        holder["id"] = self.window.after(delay, run)
        self._timers.add(holder["id"])

    def _on_focus_out(self, event) -> None:
        # A submenu opening can carry focus with it, which Tk reports as the parent
        # losing focus. Only focus leaving the whole tree should dismiss the menu, and
        # that is only knowable once Tk has settled.
        self._later(50, self._dismiss_unless_focused)

    def _dismiss_unless_focused(self) -> None:
        root = self._root()
        if root._closed:
            return
        try:
            focused = root.window.focus_displayof()
        except (tk.TclError, KeyError):
            focused = None
        if focused is not None and root._holds(focused.winfo_toplevel()):
            return
        # Tk can report no focus at all during the parent -> submenu handoff, so the
        # window manager gets the deciding vote before the menu is torn down.
        active = get_foreground_window()
        if active is not None and root._holds_hwnd(active):
            return
        root.close()

    def _pointer_outside(self) -> bool:
        x, y = winui.cursor_position()
        node = self
        while node is not None:
            try:
                left, top = node.window.winfo_rootx(), node.window.winfo_rooty()
                if (left <= x < left + node.window.winfo_width()
                        and top <= y < top + node.window.winfo_height()):
                    return False
            except tk.TclError:
                pass
            node = node.child
        return True

    def _watch_dismiss(self) -> None:
        """What closes the menu when the user goes elsewhere.

        Tk's focus events do not fire reliably for a borderless popup, and the popup
        does not always win the foreground, so neither can be the only signal. A click
        outside the menu needs no focus at all to observe; the foreground check catches
        the rest (Alt+Tab, another window raising itself), but only once the menu has
        actually held the foreground -- Windows hands it over asynchronously, and
        without that gate a slow handoff would close the menu as it opened.
        """
        if self._closed:
            return

        if winui.mouse_click_since_last_call() and self._pointer_outside():
            self.close()
            return

        active = get_foreground_window()
        if active is not None:
            if self._holds_hwnd(active):
                self._had_foreground = True
            elif self._had_foreground:
                self.close()
                return

        self._later(80, self._watch_dismiss)

    def _root(self) -> "Win11Menu":
        node = self
        while node.parent is not None:
            node = node.parent
        return node

    def _holds(self, window) -> bool:
        node = self
        while node is not None:
            if node.window is window:
                return True
            node = node.child
        return False

    def _holds_hwnd(self, handle: int) -> bool:
        node = self
        while node is not None:
            if winui.hwnd_of(node.window) == handle:
                return True
            node = node.child
        return False

    # ------------------------------------------------------------------ showing

    def _open_submenu(self, index: int, item: MenuItem) -> None:
        if self.child is not None:
            if self.child.owner_index == index:
                return
            self.child.close()
        child = Win11Menu(item.submenu, parent=self)
        child.owner_index = index
        self.child = child

        left = self.window.winfo_rootx()
        overlap = self._px(4)
        x = left + self.width - overlap
        # Where it goes when there is no room on the right: the far side of this menu,
        # not on top of it. The tray is in the bottom-right corner, so that is not a
        # corner case -- it is what happens every time.
        beside = left - (child.width + 2) + overlap
        y = self.window.winfo_rooty() + self._row_top(index) - self.pad
        child.show_at(x, y, take_focus=False, flip_x=beside, slide_y=True)

    def show_at(
        self,
        x: int,
        y: int,
        *,
        take_focus: bool = True,
        flip_x: int | None = None,
        slide_y: bool = False,
    ) -> None:
        """Place the popup, keeping it inside the work area.

        A menu opened at the cursor flips to the other side of it. A submenu passes
        `flip_x` -- the position on the far side of its parent -- and `slide_y`, because
        it should slide up far enough to fit rather than jump above its own row.
        """
        left, top, right, bottom = winui.work_area()
        width = self.width + 2
        height = self.height + 2

        if x + width > right:
            x = flip_x if flip_x is not None else x - width
        x = max(left, min(x, right - width))

        if y + height > bottom:
            y = bottom - height if slide_y else y - height
        y = max(top, y)

        self.window.geometry(f"{width}x{height}+{int(x)}+{int(y)}")
        self.window.deiconify()
        self.window.update_idletasks()

        handle = winui.hwnd_of(self.window)
        winui.round_region(handle, width, height, self.radius)

        if take_focus:
            # Without the foreground, the popup never receives the focus change that
            # tells it the user clicked elsewhere, and it stays on screen for good.
            set_foreground_window(handle)
            self.window.focus_force()
            # Clears the "pressed since last call" bit left by the click that opened
            # the menu, so the watchdog only ever sees the next one.
            winui.mouse_click_since_last_call()
            self._later(80, self._watch_dismiss)

    def close(self) -> None:
        global _visible

        if self._closed:
            return
        self._closed = True
        if _visible is self:
            _visible = None
        for timer in list(self._timers):
            try:
                self.window.after_cancel(timer)
            except tk.TclError:
                pass
        self._timers.clear()
        if self.child is not None:
            self.child.close()
            self.child = None
        try:
            self.window.destroy()
        except tk.TclError:
            pass

    def close_all(self) -> None:
        self._root().close()


def popup(items: list[MenuItem]) -> None:
    """Show the menu at the mouse pointer. Safe to call from any thread."""
    ui = winui.UiThread.instance()

    def build() -> None:
        global _visible

        if _visible is not None:
            _visible.close()
        menu = Win11Menu(items)
        _visible = menu
        x, y = winui.cursor_position()
        menu.show_at(x, y)

    ui.submit(build)
