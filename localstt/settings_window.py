"""Settings window, laid out the way Windows Terminal lays out its own.

Everything in `AppConfig` used to be reachable only by hand-editing config.json. This
gives the same settings a navigation pane, typed controls and a save step, and keeps the
"Open JSON file" escape hatch for the fields a UI should not pretend to own.
"""

from __future__ import annotations

import json
import os
import subprocess
import tkinter as tk
from dataclasses import dataclass, field as dataclass_field
from typing import Any, Callable

from . import app_index, autostart, branding, cleanup_model, performance, preflight, winui
from .command_runner import clear_availability_cache, command_statuses
from .config import COMMANDS_PATH, CONFIG_PATH, HISTORY_PATH, AppConfig, save_config
from .widgets import Button, Card, Dropdown, SettingRow, TextField, Theme, ToggleSwitch

RESTART_NOTE = "Applied when LocalSTT restarts."

# Segoe Fluent Icons: CheckMark, Warning, ErrorCircle, Health, Info.
GLYPH_OK = "\ue73e"
GLYPH_WARN = "\ue7ba"
GLYPH_FAIL = "\ue783"
GLYPH_HEALTH = "\ue95e"
GLYPH_INFO = "\ue946"
WARN_COLOR = "#f0a30a"


@dataclass
class Field:
    key: str
    title: str
    description: str = ""
    kind: str = "text"  # toggle | choice | number | text
    options: list[str] = dataclass_field(default_factory=list)
    labels: dict[str, str] = dataclass_field(default_factory=dict)
    parse: Callable[[str], Any] | None = None
    display: Callable[[Any], str] | None = None
    restart: bool = False
    # Settings that are not config.json fields (autostart) read and write through these,
    # and apply immediately instead of waiting for Save.
    getter: Callable[[], Any] | None = None
    setter: Callable[[Any], bool] | None = None


@dataclass
class Section:
    key: str
    title: str
    icon: str
    fields: list[Field] = dataclass_field(default_factory=list)
    custom: str = ""


def _microphone_options() -> tuple[list[str], dict[str, str]]:
    try:
        from .audio import list_microphones

        mics = list_microphones()
    except Exception:
        mics = []
    options = ["default"] + [str(m["index"]) for m in mics]
    labels = {"default": "Windows default"}
    labels.update({str(m["index"]): f"{m['index']}: {m['name']}" for m in mics})
    return options, labels


def _ollama_options(config: AppConfig, gpu_total_gb: float | None) -> tuple[list[str], dict[str, str]]:
    models = preflight.ollama_models(config.ollama_base_url, max_age=10.0) or []
    whisper_gb = preflight.model_vram_gb(config.model, config.compute_type) or 0.0
    budget = (gpu_total_gb - whisper_gb - preflight.GPU_RESERVE_GB) if gpu_total_gb else None

    options, labels = [], {}
    for model in models:
        name = str(model.get("name", ""))
        if not name:
            continue
        options.append(name)
        needed = preflight.ollama_vram_gb(model)
        if budget is None:
            labels[name] = name
        elif needed <= budget:
            labels[name] = f"{name}  ({needed:.1f} GB, fits)"
        else:
            labels[name] = f"{name}  ({needed:.1f} GB, spills to CPU)"
    if config.ollama_model and config.ollama_model not in options:
        options.insert(0, config.ollama_model)
        labels[config.ollama_model] = f"{config.ollama_model}  (not installed)"
    return options, labels


def build_sections(config: AppConfig, gpu_total_gb: float | None, logger) -> list[Section]:
    mic_options, mic_labels = _microphone_options()
    # Only what config already knows: asking Ollama here made opening the settings
    # window wait on the network (~2 s when nothing answers). The AI cleanup section
    # fills the real list in once it has one.
    ollama_options = [config.ollama_model] if config.ollama_model else []
    ollama_labels: dict[str, str] = {}

    compute_labels = {}
    for compute in ("float16", "int8_float16", "int8"):
        needed = preflight.model_vram_gb(config.model, compute)
        compute_labels[compute] = f"{compute}  (~{needed:.1f} GB VRAM)" if needed else compute

    return [
        Section("general", "General", "", fields=[
            Field("model", "Whisper model", "Larger models are more accurate and need more VRAM.",
                  kind="choice", options=list(config.allowed_models), restart=True),
            Field("compute_type", "Compute type",
                  "int8_float16 halves the memory the weights take, at almost no cost in accuracy.",
                  kind="choice", options=["float16", "int8_float16", "int8"],
                  labels=compute_labels, restart=True),
            Field("language", "Language", "Language Whisper is told to expect.",
                  kind="choice", options=list(config.allowed_languages),
                  labels={"ru": "Russian", "en": "English", "auto": "Detect automatically"}),
            Field("beam_size", "Beam size", "Higher is slightly more accurate and slower.",
                  kind="number"),
            Field("vad_filter", "Voice activity filter", "Drops silence before transcription.",
                  kind="toggle"),
            Field("autostart", "Start with Windows",
                  "Adds a shortcut to the Startup folder. Applied immediately.",
                  kind="toggle",
                  getter=autostart.is_enabled,
                  setter=lambda value: autostart.set_enabled(value, logger)),
        ]),
        Section("audio", "Audio", "", fields=[
            Field("microphone", "Microphone", kind="choice", options=mic_options, labels=mic_labels,
                  parse=lambda v: None if v == "default" else int(v),
                  display=lambda v: "default" if v is None else str(v)),
            Field("sample_rate", "Sample rate", "Whisper resamples to 16000 Hz anyway.", kind="number"),
            Field("input_normalize_peak", "Normalise peak",
                  "Quiet microphones are amplified up to this peak. 0 disables it.", kind="number"),
            Field("input_max_gain", "Maximum gain", "Ceiling for that amplification.", kind="number"),
        ]),
        Section("delivery", "Delivery", "", fields=[
            Field("delivery_method", "How text is delivered",
                  "Paste is instant; typewrite works in fields that block paste.",
                  kind="choice", options=["paste", "typewrite"],
                  labels={"paste": "Paste (Ctrl+V)", "typewrite": "Type character by character"}),
            Field("restore_clipboard_after_paste", "Restore clipboard afterwards", kind="toggle"),
            Field("paste_restore_delay_seconds", "Clipboard restore delay", kind="number"),
            Field("typewrite_interval_seconds", "Typing interval", kind="number"),
            Field("cancel_on_escape", "Escape cancels a recording", kind="toggle"),
            Field("hotkey_tap_seconds", "Tap threshold",
                  "Holding the hotkey longer than this makes it push-to-talk.", kind="number"),
            Field("hotkey_mode_grace_seconds", "Chord grace period",
                  "How long to wait for the rest of the chord before deciding the mode.",
                  kind="number"),
        ]),
        Section("commands", "Voice commands", "", custom="commands", fields=[
            Field("commands_enabled", "Voice commands", kind="toggle"),
            Field("command_auto_stop", "Stop as soon as a command is recognised", kind="toggle"),
            Field("command_poll_seconds", "Recognition interval", kind="number"),
            Field("command_listen_timeout_seconds", "Listening timeout", kind="number"),
            Field("command_silence_timeout_seconds", "Silence timeout", kind="number"),
            Field("command_capture_silence_seconds", "Silence before a capture ends", kind="number"),
            Field("command_min_audio_seconds", "Minimum audio length", kind="number"),
            Field("command_silence_rms", "Silence threshold (RMS)", kind="number"),
        ]),
        Section("cleanup", "AI cleanup", "", custom="cleanup", fields=[
            Field("ollama_model", "Cleanup model",
                  "Runs on the same GPU as Whisper, so it has to fit in what is left.",
                  kind="choice", options=ollama_options, labels=ollama_labels),
            Field("ollama_base_url", "Ollama address"),
            Field("ollama_timeout_seconds", "Request timeout", kind="number"),
        ]),
        Section("history", "History", "\ue81c", custom="history", fields=[
            Field("history_enabled", "Keep a history of dictations",
                  "Records every transcript with its date and time. Off by default: "
                  "everything you dictate would otherwise be stored on disk.",
                  kind="toggle"),
        ]),
        Section("performance", "Performance", "\ue916", custom="performance", fields=[
            Field("performance_tracking", "Measure performance",
                  "Records how long the last dictation and cleanup took. Turn it off to "
                  "stop writing performance.json.",
                  kind="toggle"),
        ]),
        Section("api", "API", "", fields=[
            Field("api_host", "Host", restart=True),
            Field("api_port", "Port", "OpenAI-compatible endpoint for other tools.",
                  kind="number", restart=True),
        ]),
        Section("health", "Health", GLYPH_HEALTH, custom="health"),
    ]


class SettingsWindow:
    def __init__(self, config: AppConfig, logger, on_saved: Callable[[list[str]], None] | None = None) -> None:
        self.config = config
        self.logger = logger
        self.on_saved = on_saved
        self.pending: dict[str, Any] = {}
        self.restart_keys: set[str] = set()
        self.controls: dict[str, Any] = {}

        gpu = preflight._query_gpu()
        self.gpu_total_gb = gpu["total_gb"] if gpu else None
        self.sections = build_sections(config, self.gpu_total_gb, logger)

        root = winui.UiThread.instance().root
        self.window = tk.Toplevel(root)
        self.theme = Theme(self.window)
        colors = self.theme.colors

        self.window.title("LocalSTT settings")
        self.window.configure(bg=colors.window)
        self.window.geometry(f"{self.theme.px(1000)}x{self.theme.px(700)}")
        self.window.minsize(self.theme.px(760), self.theme.px(520))
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        try:
            self.window.iconbitmap(default=str(branding.icon_file()))
        except Exception:  # Pillow may be missing on the machine this report is about
            logger.debug("window icon unavailable", exc_info=True)

        self._build()
        self.window.update_idletasks()
        winui.apply_window_chrome(winui.hwnd_of(self.window), colors)
        self._show_section(self.sections[0].key)
        self.window.lift()
        self.window.focus_force()

    # ------------------------------------------------------------------ chrome

    def _build(self) -> None:
        colors = self.theme.colors
        px = self.theme.px

        body = tk.Frame(self.window, bg=colors.window)
        body.pack(fill="both", expand=True)

        self.nav = tk.Frame(body, bg=colors.window, width=px(230))
        self.nav.pack(side="left", fill="y")
        self.nav.pack_propagate(False)

        tk.Label(
            self.nav, text="Settings", font=self.theme.font_title,
            bg=colors.window, fg=colors.text, anchor="w",
        ).pack(fill="x", padx=px(20), pady=(px(18), px(12)))

        self.nav_buttons: dict[str, tk.Canvas] = {}
        for section in self.sections:
            self.nav_buttons[section.key] = self._nav_button(section)

        right = tk.Frame(body, bg=colors.window)
        right.pack(side="left", fill="both", expand=True)

        self.header = tk.Label(
            right, text="", font=self.theme.font_section,
            bg=colors.window, fg=colors.text, anchor="w",
        )
        self.header.pack(fill="x", padx=px(24), pady=(px(22), px(10)))

        # The scrollbar has to claim its width before the canvas expands into it.
        self.canvas = tk.Canvas(right, bg=colors.window, highlightthickness=0, bd=0)
        scrollbar = tk.Scrollbar(
            right, command=self.canvas.yview, width=px(10),
            bg=colors.window, troughcolor=colors.window,
            activebackground=colors.text_secondary, relief="flat",
            borderwidth=0, highlightthickness=0,
        )
        scrollbar.pack(side="right", fill="y", padx=(px(4), px(8)), pady=px(2))
        self.canvas.configure(yscrollcommand=scrollbar.set)
        self.canvas.pack(side="left", fill="both", expand=True, padx=(px(24), 0))

        self.content = tk.Frame(self.canvas, bg=colors.window)
        self._content_window = self.canvas.create_window((0, 0), window=self.content, anchor="nw")
        self.content.bind(
            "<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )
        self.canvas.bind(
            "<Configure>", lambda e: self.canvas.itemconfigure(self._content_window, width=e.width)
        )
        self.window.bind_all("<MouseWheel>", self._on_wheel)

        self._build_footer()

    def _on_wheel(self, event) -> None:
        try:
            self.canvas.yview_scroll(int(-event.delta / 120), "units")
        except tk.TclError:
            pass

    def _nav_button(self, section: Section) -> tk.Canvas:
        px = self.theme.px
        colors = self.theme.colors
        height = px(38)
        canvas = tk.Canvas(
            self.nav, height=height, bg=colors.window,
            highlightthickness=0, bd=0, cursor="hand2",
        )
        canvas.pack(fill="x", padx=px(8), pady=px(1))
        canvas.section_key = section.key
        canvas.section = section
        canvas.selected = False
        canvas.hover = False
        canvas.bind("<Button-1>", lambda e, k=section.key: self._show_section(k))
        canvas.bind("<Enter>", lambda e, c=canvas: self._paint_nav(c, hover=True))
        canvas.bind("<Leave>", lambda e, c=canvas: self._paint_nav(c, hover=False))
        canvas.bind("<Configure>", lambda e, c=canvas: self._paint_nav(c))
        return canvas

    def _paint_nav(self, canvas: tk.Canvas, hover: bool | None = None) -> None:
        px = self.theme.px
        colors = self.theme.colors
        if hover is not None:
            canvas.hover = hover
        width = canvas.winfo_width()
        height = canvas.winfo_height()
        if width <= 1:
            return
        canvas.delete("all")
        if canvas.selected:
            fill = colors.hover
        elif canvas.hover:
            fill = colors.pressed if colors.dark else colors.divider
        else:
            fill = colors.window
        winui.round_rect(canvas, 1, 1, width - 1, height - 1, px(4), fill=fill, outline="")
        if canvas.selected:
            # The accent bar on the left is how Windows 11 marks the active nav item.
            canvas.create_line(
                px(3), height * 0.28, px(3), height * 0.72,
                fill=colors.accent, width=px(3), capstyle="round",
            )
        canvas.create_text(
            px(20), height / 2, text=canvas.section.icon,
            font=self.theme.font_icon, fill=colors.text, anchor="center",
        )
        canvas.create_text(
            px(40), height / 2, text=canvas.section.title,
            font=self.theme.font, fill=colors.text, anchor="w",
        )

    def _build_footer(self) -> None:
        px = self.theme.px
        colors = self.theme.colors

        footer = tk.Frame(self.window, bg=colors.surface, height=px(56))
        footer.pack(fill="x", side="bottom")
        footer.pack_propagate(False)

        link = tk.Label(
            footer, text="Open config.json", font=self.theme.font,
            bg=colors.surface, fg=colors.accent, cursor="hand2",
        )
        link.pack(side="left", padx=px(24))
        link.bind("<Button-1>", lambda e: _open_path(CONFIG_PATH))

        # Whether this machine is healthy is worth seeing without opening the section.
        self.health_badge = tk.Label(
            footer, text="", font=self.theme.font, bg=colors.surface,
            fg=colors.text_secondary, cursor="hand2",
        )
        self.health_badge.pack(side="left")
        self.health_badge.bind("<Button-1>", lambda e: self._show_section("health"))
        self._update_health_badge(preflight.load())

        self.status = tk.Label(
            footer, text="", font=self.theme.font_small,
            bg=colors.surface, fg=colors.text_secondary,
        )
        self.status.pack(side="left", padx=px(8))

        self.save_button = Button(footer, self.theme, "Save", self._save,
                                  primary=True, background=colors.surface)
        self.save_button.pack(side="right", padx=(px(8), px(24)), pady=px(12))
        Button(footer, self.theme, "Discard", self._discard,
               background=colors.surface).pack(side="right", pady=px(12))

    def _update_health_badge(self, report: preflight.Report | None) -> None:
        """The footer says how this machine is doing without opening the section."""
        colors = self.theme.colors
        if report is None:
            self.health_badge.configure(
                text=f"{GLYPH_HEALTH}  Health: not checked yet", fg=colors.text_secondary
            )
            return

        problems = [c for c in report.core if c.status != preflight.OK]
        status = report.core_status
        if status == preflight.OK:
            glyph, color, text = GLYPH_OK, colors.accent, "Healthy"
        elif status == preflight.WARN:
            glyph, color = GLYPH_WARN, WARN_COLOR
            text = f"{len(problems)} warning(s)"
        else:
            glyph, color = GLYPH_FAIL, colors.danger
            text = f"{len(problems)} problem(s)"
        self.health_badge.configure(text=f"{glyph}  {text}", fg=color)

    # ------------------------------------------------------------------ sections

    def _show_section(self, key: str) -> None:
        section = next(s for s in self.sections if s.key == key)
        for nav_key, canvas in self.nav_buttons.items():
            canvas.selected = nav_key == key
            self._paint_nav(canvas)

        self.header.configure(text=section.title)
        for child in self.content.winfo_children():
            child.destroy()
        self.controls.clear()

        if section.custom == "health":
            self._render_health()
            return

        if section.custom == "commands":
            # One availability pass feeds both the summary and the list below it.
            rows = command_statuses(self.logger)
            self._render_commands_health(rows)
            self._render_fields(section)
            self._render_commands(rows)
            return

        self._render_fields(section)
        if section.custom == "cleanup":
            self._render_cleanup()
        elif section.custom == "performance":
            self._render_performance()
        elif section.custom == "history":
            self._render_history()

    def _value_of(self, field: Field) -> Any:
        if field.getter is not None:
            return field.getter()
        if field.key in self.pending:
            return self.pending[field.key]
        return getattr(self.config, field.key)

    def _apply_now(self, field: Field, value: Any, control) -> None:
        """Fields backed by the system, not the config file, take effect on the spot."""
        if field.setter is not None and field.setter(value):
            self.status.configure(text=f"{field.title}: {'on' if value else 'off'}")
            return
        control.set(self._value_of(field))
        self.status.configure(text=f"{field.title} could not be changed")

    def _stage(self, field: Field, value: Any) -> None:
        self.pending[field.key] = value
        if field.restart:
            self.restart_keys.add(field.key)
        self.status.configure(text=f"{len(self.pending)} unsaved change(s)")

    def _render_fields(self, section: Section) -> None:
        px = self.theme.px
        for field in section.fields:
            card = Card(self.content, self.theme)
            card.pack_card(pady=(0, px(6)))
            description = field.description
            if field.restart:
                description = f"{description} {RESTART_NOTE}".strip()
            row = SettingRow(card, self.theme, field.title, description)
            row.pack(fill="x")
            self._build_control(row.control_area, field)
        tk.Frame(self.content, bg=self.theme.colors.window, height=px(16)).pack(fill="x")

    def _build_control(self, parent: tk.Misc, field: Field) -> None:
        value = self._value_of(field)

        if field.kind == "toggle":
            toggle = ToggleSwitch(parent, self.theme, bool(value), lambda _v: None)
            toggle.on_change = (
                (lambda v, f=field, c=toggle: self._apply_now(f, v, c))
                if field.setter is not None
                else (lambda v, f=field: self._stage(f, v))
            )
            toggle.pack()
            self.controls[field.key] = toggle
            return

        if field.kind == "choice":
            shown = field.display(value) if field.display else str(value)
            options = list(field.options)
            if shown not in options:
                options.insert(0, shown)
            dropdown = Dropdown(
                parent, self.theme, shown, options,
                lambda v, f=field: self._stage(f, f.parse(v) if f.parse else v),
                labels=field.labels, width=240,
            )
            dropdown.pack()
            self.controls[field.key] = dropdown
            return

        text_field = TextField(
            parent, self.theme, str(value),
            lambda v, f=field: self._stage(f, v),
            width=240 if field.kind == "text" else 120,
        )
        text_field.pack()
        self.controls[field.key] = text_field

    # ------------------------------------------------------------------ commands

    def _render_commands_health(self, rows: list) -> None:
        """How the commands fare on this machine, at the top of their own section.

        This used to be a line in the health report, where a missing text editor read
        as a problem with LocalSTT. It belongs next to the commands it describes.
        """
        px = self.theme.px
        unavailable = [(c, s) for c, s in rows if not s.available]

        if not rows:
            check = preflight.Check(
                "commands", "Voice commands", preflight.WARN,
                "commands.json holds no commands",
                "Add commands to commands.json, or use the button below to open it.",
            )
        elif unavailable:
            listed = ", ".join(str(c.get("name", "unnamed")) for c, _ in unavailable[:6])
            check = preflight.Check(
                "commands", "Voice commands", preflight.WARN,
                f"{len(rows) - len(unavailable)} of {len(rows)} work on this machine; "
                f"unavailable: {listed}",
                "These point at software this machine does not have. They are skipped, "
                "not broken, and the rest still work.",
            )
        else:
            check = preflight.Check(
                "commands", "Voice commands", preflight.OK,
                f"all {len(rows)} commands work on this machine",
            )

        self._check_card(self.content, check)
        tk.Frame(self.content, bg=self.theme.colors.window, height=px(10)).pack(fill="x")

    def _render_commands(self, rows: list) -> None:
        px = self.theme.px
        colors = self.theme.colors

        tk.Label(
            self.content, text="Installed commands", font=self.theme.font_bold,
            bg=colors.window, fg=colors.text, anchor="w",
        ).pack(fill="x", pady=(px(14), px(6)))

        tools = tk.Frame(self.content, bg=colors.window)
        tools.pack(fill="x", pady=(0, px(8)))
        Button(tools, self.theme, "Open commands.json",
               lambda: _open_path(COMMANDS_PATH), background=colors.window).pack(side="left")
        Button(tools, self.theme, "Edit app names",
               lambda: _open_path(app_index.APP_ALIASES_PATH),
               background=colors.window).pack(side="left", padx=px(8))
        Button(tools, self.theme, "Rebuild app index",
               self._rebuild_index, background=colors.window).pack(side="left")
        Button(tools, self.theme, "Re-check", self._recheck_commands,
               background=colors.window).pack(side="left", padx=px(8))

        for command, status in rows:
            name = str(command.get("name", "unnamed"))
            patterns = ", ".join(str(p) for p in command.get("patterns", [])[:3])
            description = patterns if status.available else f"{patterns}\nUnavailable: {status.reason}"

            card = Card(self.content, self.theme, padding=12)
            card.pack_card(pady=(0, px(4)))
            line = tk.Frame(card, bg=colors.card)
            line.pack(fill="x")
            # The mark is what makes a command that cannot run visible at a glance.
            glyph, color = (
                (GLYPH_OK, colors.accent) if status.available else (GLYPH_WARN, WARN_COLOR)
            )
            tk.Label(
                line, text=glyph, font=self.theme.font_icon, bg=colors.card, fg=color,
            ).pack(side="left", padx=(0, px(12)))
            row = SettingRow(line, self.theme, name, description)
            row.pack(side="left", fill="x", expand=True)
            ToggleSwitch(
                row.control_area, self.theme,
                command.get("enabled", True) is not False,
                lambda value, n=name: self._set_command_enabled(n, value),
            ).pack()

        tk.Frame(self.content, bg=colors.window, height=px(16)).pack(fill="x")

    def _recheck_commands(self) -> None:
        clear_availability_cache()
        self.status.configure(text="re-checked voice commands")
        self._show_section("commands")

    def _set_command_enabled(self, name: str, enabled: bool) -> None:
        try:
            data = json.loads(COMMANDS_PATH.read_text(encoding="utf-8-sig"))
            for command in data.get("commands", []):
                if command.get("name") == name:
                    if enabled:
                        command.pop("enabled", None)
                    else:
                        command["enabled"] = False
            COMMANDS_PATH.write_text(
                json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
            clear_availability_cache()
            self.status.configure(text=f"{name} {'enabled' if enabled else 'disabled'}")
        except (OSError, json.JSONDecodeError) as exc:
            self.logger.warning("could not update %s in commands.json: %s", name, exc)
            self.status.configure(text=f"could not update commands.json: {exc}")

    def _rebuild_index(self) -> None:
        self.status.configure(text="rebuilding application index...")

        def work() -> None:
            apps = app_index.load_index(self.logger, refresh=True)
            winui.UiThread.instance().submit(
                lambda: self.status.configure(text=f"indexed {len(apps)} applications")
            )

        _run_off_ui(work)

    # ------------------------------------------------------------------ cleanup model

    def _render_cleanup(self) -> None:
        """What this GPU can actually run, and a way to get it if nothing here fits.

        Ollama is asked off the UI thread: the answer used to be awaited inline, which
        froze the section while it was being drawn and left the card blank until the
        user navigated away and back.
        """
        px = self.theme.px
        card = Card(self.content, self.theme)
        card.pack_card(pady=(px(8), px(6)))
        row = SettingRow(card, self.theme, "What fits on this GPU", "Asking Ollama...")
        row.pack(fill="x")

        def work() -> None:
            gpu = {"total_gb": self.gpu_total_gb} if self.gpu_total_gb else None
            choice = preflight.recommend_cleanup_model(self.config, gpu, max_age=10.0)
            # Answered from the same reply the recommendation just used.
            options, labels = _ollama_options(self.config, self.gpu_total_gb)
            winui.UiThread.instance().submit(
                lambda: self._show_cleanup_choice(card, row, choice, options, labels)
            )

        _run_off_ui(work)

    def _show_cleanup_choice(
        self, card: Card, row: SettingRow, choice, options: list[str], labels: dict[str, str]
    ) -> None:
        colors = self.theme.colors
        try:
            if not row.winfo_exists():  # the section was left before Ollama answered
                return
        except tk.TclError:
            return

        dropdown = self.controls.get("ollama_model")
        if dropdown is not None and dropdown.winfo_exists() and options:
            dropdown.labels = labels
            dropdown.set_options(options, self.pending.get("ollama_model", self.config.ollama_model))

        row.set_description(choice.note)
        if choice.reachable:
            if choice.pull:
                Button(
                    row.control_area, self.theme, f"Download {choice.pull}",
                    lambda name=choice.pull: self._pull_cleanup_model(name),
                    primary=True, background=colors.card,
                ).pack()
            elif choice.best_installed and choice.best_installed != self.config.ollama_model:
                Button(
                    row.control_area, self.theme, f"Use {choice.best_installed}",
                    lambda name=choice.best_installed: self._use_cleanup_model(name),
                    primary=True, background=colors.card,
                ).pack()
        card.refresh()

    def _use_cleanup_model(self, name: str) -> None:
        self.config.ollama_model = name
        save_config(self.config)
        self.status.configure(text=f"cleanup model set to {name}")
        self._show_section("cleanup")

    def _pull_cleanup_model(self, name: str) -> None:
        self.status.configure(text=f"downloading {name}... this can take a while")

        def work() -> None:
            ok = cleanup_model.pull(name, self.logger)
            if ok:
                self.config.ollama_model = name
                save_config(self.config)

            def show() -> None:
                self.status.configure(
                    text=f"cleanup model set to {name}" if ok else f"could not download {name}"
                )
                self._show_section("cleanup")

            winui.UiThread.instance().submit(show)

        _run_off_ui(work)

    # ------------------------------------------------------------------ history

    HISTORY_LIMIT = 200

    def _load_history(self) -> list[dict[str, Any]]:
        """The most recent entries, kept in the order they were recorded."""
        if not HISTORY_PATH.exists():
            return []
        try:
            lines = HISTORY_PATH.read_text(encoding="utf-8-sig").splitlines()
        except OSError:
            return []

        rows: list[dict[str, Any]] = []
        for line in lines[-self.HISTORY_LIMIT :]:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(row, dict):
                rows.append(row)
        return rows

    def _render_history(self) -> None:
        px = self.theme.px
        colors = self.theme.colors

        tools = tk.Frame(self.content, bg=colors.window)
        tools.pack(fill="x", pady=(px(10), px(8)))
        Button(tools, self.theme, "Open history.jsonl", lambda: _open_path(HISTORY_PATH),
               background=colors.window).pack(side="left")
        self._clear_button = Button(
            tools, self.theme, "Clear history", self._clear_history, background=colors.window
        )
        self._clear_button.pack(side="left", padx=px(8))
        self._clear_armed = False

        rows = self._load_history()
        if not rows:
            tk.Label(
                self.content,
                text=(
                    "No dictations recorded."
                    if self.config.history_enabled
                    else "History is off, so nothing is being recorded."
                ),
                font=self.theme.font, bg=colors.window, fg=colors.text_secondary, anchor="w",
            ).pack(fill="x", pady=px(8))
            return

        current_day = None
        for row in rows:
            timestamp = str(row.get("ts", ""))
            day, _, clock = timestamp.partition("T")
            if day != current_day:
                current_day = day
                tk.Label(
                    self.content, text=day or "unknown date", font=self.theme.font_bold,
                    bg=colors.window, fg=colors.text, anchor="w",
                ).pack(fill="x", pady=(px(10), px(4)))

            card = Card(self.content, self.theme, padding=12)
            card.pack_card(pady=(0, px(4)))

            duration = float(row.get("duration", 0.0) or 0.0)
            processing = float(row.get("processing_time", 0.0) or 0.0)
            meta = f"{clock or '--'}   {row.get('language', '?')}   {duration:.1f}s audio, {processing:.2f}s to transcribe"
            tk.Label(
                card, text=str(row.get("text", "")).strip() or "(empty)",
                font=self.theme.font, bg=colors.card, fg=colors.text,
                anchor="w", justify="left", wraplength=px(560),
            ).pack(anchor="w")
            tk.Label(
                card, text=meta, font=self.theme.font_small, bg=colors.card,
                fg=colors.text_secondary, anchor="w",
            ).pack(anchor="w", pady=(px(2), 0))

        tk.Frame(self.content, bg=colors.window, height=px(16)).pack(fill="x")

    def _clear_history(self) -> None:
        """Deleting transcripts is not undoable, so the first click only arms it."""
        if not self._clear_armed:
            self._clear_armed = True
            self._clear_button.text = "Click again to delete"
            self._clear_button._draw()
            self.status.configure(text="this permanently deletes every recorded transcript")
            return
        try:
            HISTORY_PATH.unlink(missing_ok=True)
            self.status.configure(text="history deleted")
            self.logger.info("history cleared from the settings window")
        except OSError as exc:
            self.status.configure(text=f"could not delete history: {exc}")
        self._show_section("history")

    # ------------------------------------------------------------------ performance

    def _stat_card(self, parent: tk.Misc, title: str, lines: list[str]) -> None:
        px = self.theme.px
        colors = self.theme.colors
        card = Card(parent, self.theme)
        card.pack_card(pady=(0, px(6)))
        tk.Label(
            card, text=title, font=self.theme.font_bold,
            bg=colors.card, fg=colors.text, anchor="w",
        ).pack(anchor="w")
        for index, line in enumerate(lines):
            tk.Label(
                card, text=line,
                font=self.theme.font if index == 0 else self.theme.font_small,
                bg=colors.card,
                fg=colors.text if index == 0 else colors.text_secondary,
                anchor="w", justify="left", wraplength=px(560),
            ).pack(anchor="w", pady=(px(4) if index == 0 else px(1), 0))

    def _render_performance(self) -> None:
        px = self.theme.px
        colors = self.theme.colors

        tools = tk.Frame(self.content, bg=colors.window)
        tools.pack(fill="x", pady=(px(10), px(8)))
        Button(tools, self.theme, "Refresh", lambda: self._show_section("performance"),
               background=colors.window).pack(side="left")

        data = performance.load()
        transcription = data.get("transcription") or {}
        cleanup = data.get("cleanup") or {}

        if not transcription and not cleanup:
            tk.Label(
                self.content, text="Nothing measured yet. Dictate once and come back.",
                font=self.theme.font, bg=colors.window, fg=colors.text_secondary, anchor="w",
            ).pack(fill="x", pady=px(8))
            return

        if transcription:
            audio = float(transcription.get("audio_seconds", 0.0))
            processing = float(transcription.get("processing_seconds", 0.0))
            rtf = float(transcription.get("realtime_factor", 0.0))
            speed = f"{1 / rtf:.1f}x faster than real time" if rtf else "unknown speed"
            lines = [
                f"{audio:.1f} s of speech transcribed in {processing:.2f} s -- {speed}",
                f"Real-time factor {rtf:.2f}. Lower is faster; this is the metric that "
                f"describes the model rather than the speaker.",
                f"{transcription.get('chars', 0)} characters "
                f"({transcription.get('chars_per_second', 0)} per second, which mostly "
                f"reflects how fast you were talking).",
                f"{transcription.get('model')} @ {transcription.get('compute_type')} "
                f"on {transcription.get('device')}, beam {transcription.get('beam_size')}"
                f"  --  {transcription.get('ts', '')}",
            ]
            self._stat_card(self.content, "Last dictation", lines)

            session = transcription.get("session") or {}
            count = int(session.get("transcription_count", 0))
            if count:
                self._stat_card(self.content, "Since LocalSTT started", [
                    f"{count} dictation(s), "
                    f"{float(session.get('total_audio_duration', 0.0)):.1f} s of audio",
                    f"Average {float(session.get('average_processing_time', 0.0)):.2f} s per "
                    f"dictation, real-time factor "
                    f"{float(session.get('realtime_factor', 0.0)):.2f}",
                ])

        if cleanup:
            tokens = int(cleanup.get("tokens", 0))
            generate = float(cleanup.get("generate_seconds", 0.0))
            self._stat_card(self.content, "Last AI cleanup", [
                f"{tokens} tokens in {generate:.2f} s -- "
                f"{cleanup.get('tokens_per_second', 0)} tokens per second",
                f"{cleanup.get('model')}, {cleanup.get('prompt_tokens', 0)} prompt tokens, "
                f"{float(cleanup.get('total_seconds', 0.0)):.2f} s end to end"
                f"  --  {cleanup.get('ts', '')}",
                "Tokens per second is the right measure for the language model; the "
                "recognition above is measured against the length of the audio instead.",
            ])

        tk.Frame(self.content, bg=colors.window, height=px(16)).pack(fill="x")

    # ------------------------------------------------------------------ self-test

    def _render_health(self) -> None:
        px = self.theme.px
        colors = self.theme.colors

        tools = tk.Frame(self.content, bg=colors.window)
        tools.pack(fill="x", pady=(0, px(10)))
        Button(tools, self.theme, "Run health check", self._run_health_check,
               primary=True, background=colors.window).pack(side="left")

        self.health_host = tk.Frame(self.content, bg=colors.window)
        self.health_host.pack(fill="both", expand=True)

        report = preflight.load()
        if report is None:
            tk.Label(
                self.health_host,
                text="This device has not been checked yet.",
                font=self.theme.font, bg=colors.window, fg=colors.text_secondary, anchor="w",
            ).pack(fill="x", pady=px(8))
        else:
            self._render_report(report)

    def _run_health_check(self) -> None:
        self.status.configure(text="running health check...")

        def work() -> None:
            report = preflight.run(self.config, self.logger)
            preflight.save(report)

            def show() -> None:
                self._update_health_badge(report)
                if not self.health_host.winfo_exists():
                    return
                for child in self.health_host.winfo_children():
                    child.destroy()
                self._render_report(report)
                self.status.configure(text=f"health: {report.core_status}")

            winui.UiThread.instance().submit(show)

        _run_off_ui(work)

    def _check_card(self, parent: tk.Misc, check: preflight.Check) -> None:
        """One check, drawn the same way wherever it is shown."""
        px = self.theme.px
        colors = self.theme.colors
        marks = {
            preflight.OK: (GLYPH_OK, colors.accent),
            preflight.WARN: (GLYPH_WARN, WARN_COLOR),
            preflight.FAIL: (GLYPH_FAIL, colors.danger),
        }
        glyph, color = marks.get(check.status, ("", colors.text))

        card = Card(parent, self.theme, padding=12)
        card.pack_card(pady=(0, px(4)))

        line = tk.Frame(card, bg=colors.card)
        line.pack(fill="x")
        tk.Label(
            line, text=glyph, font=self.theme.font_icon, bg=colors.card, fg=color,
        ).pack(side="left", padx=(0, px(12)))

        text = tk.Frame(line, bg=colors.card)
        text.pack(side="left", fill="x", expand=True)
        tk.Label(
            text, text=check.title, font=self.theme.font,
            bg=colors.card, fg=colors.text, anchor="w",
        ).pack(anchor="w")
        body = check.detail if check.status == preflight.OK else f"{check.detail}\n{check.hint}".strip()
        tk.Label(
            text, text=body, font=self.theme.font_small, bg=colors.card,
            fg=colors.text_secondary, anchor="w", justify="left", wraplength=px(560),
        ).pack(anchor="w")

    def _render_report(self, report: preflight.Report) -> None:
        px = self.theme.px
        colors = self.theme.colors

        tk.Label(
            self.health_host,
            text=f"{report.machine} - {report.ran_at} - {report.core_status.upper()}",
            font=self.theme.font_small, bg=colors.window, fg=colors.text_secondary, anchor="w",
        ).pack(fill="x", pady=(0, px(8)))

        for check in report.core:
            self._check_card(self.health_host, check)

        # Cleanup dictation is an extra: when Ollama is missing or its model does not
        # fit, recording and typing still work, so it is reported on its own terms and
        # never colours the status above. Voice commands answer for themselves, in
        # their own section.
        extras = [c for c in report.advisory if c.name != "commands"]
        if extras:
            tk.Label(
                self.health_host, text="Extras", font=self.theme.font_bold,
                bg=colors.window, fg=colors.text, anchor="w",
            ).pack(fill="x", pady=(px(14), px(2)))
            tk.Label(
                self.health_host,
                text="Dictation records, transcribes and types without these.",
                font=self.theme.font_small, bg=colors.window, fg=colors.text_secondary, anchor="w",
            ).pack(fill="x", pady=(0, px(6)))
            for check in extras:
                self._check_card(self.health_host, check)

        tk.Frame(self.health_host, bg=colors.window, height=px(16)).pack(fill="x")

    # ------------------------------------------------------------------ save

    def _save(self) -> None:
        applied: list[str] = []
        problems: list[str] = []

        for key, raw in self.pending.items():
            current = getattr(self.config, key)
            try:
                setattr(self.config, key, _coerce(raw, current))
            except (TypeError, ValueError):
                problems.append(key)
                continue
            applied.append(key)

        if problems:
            self.status.configure(text=f"not a valid number: {', '.join(problems)}")
            return

        save_config(self.config)
        needs_restart = sorted(self.restart_keys & set(applied))
        self.pending.clear()
        self.restart_keys.clear()
        self.logger.info("settings saved: %s", ", ".join(applied) or "no changes")

        if self.on_saved is not None:
            self.on_saved(applied)

        # Both buttons dismiss the window; the tray shows the confirmation. A restart
        # note is the one thing the user still has to read, so it stays up briefly.
        if needs_restart:
            self.status.configure(text=f"saved - restart to apply {', '.join(needs_restart)}")
            self.window.after(1800, self.close)
        else:
            self.status.configure(text="saved")
            self.close()

    def _discard(self) -> None:
        self.pending.clear()
        self.restart_keys.clear()
        self.logger.info("settings discarded")
        self.close()

    def close(self) -> None:
        try:
            self.window.unbind_all("<MouseWheel>")
            self.window.destroy()
        except tk.TclError:
            pass


def _coerce(raw: Any, template: Any) -> Any:
    """Text from a field has to come back as the type the config field already holds."""
    if isinstance(template, bool) or isinstance(raw, bool):
        return bool(raw)
    if raw is None:
        return None
    if isinstance(template, int):
        return int(float(str(raw).strip()))
    if isinstance(template, float):
        return float(str(raw).strip())
    if template is None and isinstance(raw, int):
        return raw
    return raw if isinstance(raw, (list, dict)) else str(raw)


def _run_off_ui(work: Callable[[], None]) -> None:
    import threading

    threading.Thread(target=work, daemon=True).start()


def _open_path(path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        path.touch()
    try:
        os.startfile(str(path))
    except (OSError, AttributeError):
        subprocess.Popen(["notepad.exe", str(path)])


_WINDOW: SettingsWindow | None = None


def open_settings(
    config: AppConfig,
    logger,
    on_saved: Callable[[list[str]], None] | None = None,
    *,
    section: str | None = None,
    run_health_check: bool = False,
) -> None:
    """Show the settings window, reusing the open one. Safe to call from any thread."""

    def build() -> None:
        global _WINDOW
        if _WINDOW is not None:
            try:
                _WINDOW.window.deiconify()
                _WINDOW.window.lift()
                _WINDOW.window.focus_force()
                if section:
                    _WINDOW._show_section(section)
                if run_health_check:
                    _WINDOW._run_health_check()
                return
            except tk.TclError:
                _WINDOW = None
        window = SettingsWindow(config, logger, on_saved)
        if section:
            window._show_section(section)
        if run_health_check:
            window._run_health_check()
        original_close = window.close

        def close() -> None:
            global _WINDOW
            _WINDOW = None
            original_close()

        window.close = close
        window.window.protocol("WM_DELETE_WINDOW", close)
        _WINDOW = window

    winui.UiThread.instance().submit(build)


def show_report_blocking(config: AppConfig, logger) -> None:
    """Show the self-test and wait for the user to close it, before the tray exists."""
    import threading

    done = threading.Event()

    def build() -> None:
        window = SettingsWindow(config, logger)
        window._show_section("health")
        original_close = window.close

        def close() -> None:
            original_close()
            done.set()

        window.close = close
        window.window.protocol("WM_DELETE_WINDOW", close)

    winui.UiThread.instance().submit(build)
    done.wait()
