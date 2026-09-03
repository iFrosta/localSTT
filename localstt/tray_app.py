from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import threading
import time
from enum import Enum
from pathlib import Path

import pyperclip
import pystray
import uvicorn
from PIL import Image, ImageDraw
from pynput import keyboard

from .api import create_app
from .audio import AudioRecorder, list_microphones
from .command_runner import (
    COMMAND_HISTORY_PATH,
    CommandOutcome,
    execute_voice_command,
    match_voice_command,
    run_matched_command,
)
from .config import APPDATA_DIR, COMMANDS_PATH, CONFIG_PATH, LAST_TRANSCRIPT_PATH, LOG_PATH, AppConfig, save_config
from .ollama_cleanup import polish_text
from .service import STTService
from . import settings_window, text_input, tray_menu
from .tray_menu import MenuItem, separator
from .window_focus import get_foreground_window, set_foreground_window


class AppState(str, Enum):
    LOADING = "loading"
    READY = "ready"
    RECORDING = "recording"
    TRANSCRIBING = "transcribing"
    CLEANUP = "cleanup"
    COMMAND = "command"
    ERROR = "error"


COLORS = {
    AppState.LOADING: "#777777",
    AppState.READY: "#19a463",
    AppState.RECORDING: "#d83b35",
    AppState.TRANSCRIBING: "#2d7ff9",
    AppState.CLEANUP: "#8a4de8",
    AppState.COMMAND: "#f08c00",
    AppState.ERROR: "#e0b400",
}

# Set while the hotkey chord is still being typed and the mode is not decided yet.
MODE_PENDING = "pending"

# Windows fixes the size of the notification-area cell, so a tray icon can only look
# bigger by filling more of its own bitmap. The original glyph covered 48 of 64 pixels;
# 1.29 takes it to the edge, which is as large as it can get without clipping.
ICON_GLYPH_SCALE = 1.29
ICON_SIZE = 256


class Win11TrayIcon(pystray.Icon):
    """pystray shows the legacy Win32 menu; both clicks go to the Fluent one instead."""

    def __init__(self, *args, on_menu, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._on_menu = on_menu

    def _on_notify(self, wparam, lparam) -> None:
        from pystray._util import win32

        if lparam in (win32.WM_LBUTTONUP, win32.WM_RBUTTONUP):
            # The popup can only dismiss itself on focus loss if the tray window is
            # foreground first -- the same reason pystray does this for TrackPopupMenu.
            win32.SetForegroundWindow(self._hwnd)
            self._on_menu()


class LocalSTTTrayApp:
    def __init__(self, config: AppConfig, service: STTService) -> None:
        self.config = config
        self.service = service
        self.state = AppState.LOADING
        self.icon = Win11TrayIcon(
            "LocalSTT", self._image(self.state), "LocalSTT", None, on_menu=self._show_menu
        )
        self.recorder: AudioRecorder | None = None
        self.pressed: set[str] = set()
        self.recording_mode: str | None = None
        self.chord_modifiers: set[str] = set()
        self.session_id = 0
        self.stop_in_progress = False
        self.hotkey_down_at: float | None = None
        self.target_hwnd: int | None = None
        self.state_lock = threading.RLock()
        self.listener: keyboard.Listener | None = None

    def run(self) -> None:
        threading.Thread(target=self._bootstrap, daemon=True).start()
        self.icon.run()

    def _bootstrap(self) -> None:
        try:
            self.service.load()
            self._start_api()
            self._start_hotkeys()
            self._set_state(AppState.READY)
        except Exception as exc:
            self.service.logger.exception("LocalSTT startup failed: %s", exc)
            self._set_state(AppState.ERROR)

    def _start_api(self) -> None:
        app = create_app(self.service)
        config = uvicorn.Config(
            app,
            host=self.config.api_host,
            port=self.config.api_port,
            log_level="warning",
            access_log=False,
        )
        server = uvicorn.Server(config)
        threading.Thread(target=server.run, daemon=True).start()

    def _start_hotkeys(self) -> None:
        self.listener = keyboard.Listener(on_press=self._on_press, on_release=self._on_release)
        self.listener.start()
        self.service.logger.info(
            "global hotkeys ready: Ctrl+Win=dictation, Ctrl+Shift+Win=cleanup, Ctrl+Alt+Win=command "
            "(command auto-stop=%s); Ctrl+Win always stops the active recording, Esc cancels it (%s)",
            self.config.command_auto_stop,
            self.config.cancel_on_escape,
        )

    def _on_press(self, key) -> None:
        name = self._key_name(key)
        if name is None:
            return
        should_start = False
        should_stop = False
        should_cancel = False
        session = 0
        with self.state_lock:
            was_down = self._ctrl_win_down()
            self.pressed.add(name)
            if (
                name == "esc"
                and self.config.cancel_on_escape
                and self.recording_mode is not None
                and not self.stop_in_progress
            ):
                session = self.session_id
                self.stop_in_progress = True
                should_cancel = True
            if self.recording_mode == MODE_PENDING:
                self.chord_modifiers.add(name)
            # Only a fresh Ctrl+Win chord starts or stops a recording. Modifiers pressed
            # afterwards (the Alt of Ctrl+Alt+Win) must not count as a second press.
            if not should_cancel and self._ctrl_win_down() and not was_down and not self.stop_in_progress:
                if self.recording_mode is None:
                    self.session_id += 1
                    session = self.session_id
                    self.recording_mode = MODE_PENDING
                    self.chord_modifiers = set(self.pressed)
                    self.hotkey_down_at = time.perf_counter()
                    self.target_hwnd = get_foreground_window()
                    should_start = True
                else:
                    session = self.session_id
                    self.stop_in_progress = True
                    should_stop = True
        if should_cancel:
            threading.Thread(target=self._cancel_recording, args=(session,), daemon=True).start()
        elif should_start:
            threading.Thread(target=self._start_recording, args=(session,), daemon=True).start()
        elif should_stop:
            self.service.logger.info("recording stop requested by Ctrl+Win press session=%s", session)
            threading.Thread(target=self._stop_transcribe_paste, args=(session,), daemon=True).start()

    def _on_release(self, key) -> None:
        name = self._key_name(key)
        should_stop = False
        session = 0
        with self.state_lock:
            if name:
                self.pressed.discard(name)
            held_for = time.perf_counter() - self.hotkey_down_at if self.hotkey_down_at else 0.0
            if (
                self.recording_mode is not None
                and not self.stop_in_progress
                and not self._ctrl_win_down()
                and held_for >= self.config.hotkey_tap_seconds
            ):
                mode = self._resolve_mode_locked()
                # Command mode keeps listening until it recognises something, so a
                # push-to-talk release must not cut it short.
                if not (mode == "command" and self.config.command_auto_stop):
                    session = self.session_id
                    self.stop_in_progress = True
                    should_stop = True
        if should_stop:
            self.service.logger.info("recording stop requested by hotkey release session=%s", session)
            threading.Thread(target=self._stop_transcribe_paste, args=(session,), daemon=True).start()

    def _ctrl_win_down(self) -> bool:
        return "ctrl" in self.pressed and "win" in self.pressed

    def _resolve_mode_locked(self) -> str | None:
        """Decide the mode from every modifier seen while the chord was held."""
        if self.recording_mode == MODE_PENDING:
            if "alt" in self.chord_modifiers:
                self.recording_mode = "command"
            elif "shift" in self.chord_modifiers:
                self.recording_mode = "cleanup"
            else:
                self.recording_mode = "dictation"
        return self.recording_mode

    def _claim_stop(self, session: int) -> bool:
        """Take ownership of stopping this recording; False if someone else already did."""
        with self.state_lock:
            if session != self.session_id or self.recording_mode is None or self.stop_in_progress:
                return False
            self.stop_in_progress = True
            return True

    def _start_recording(self, session: int) -> None:
        recorder = AudioRecorder(
            sample_rate=self.config.sample_rate,
            channels=self.config.channels,
            device=self.config.microphone,
            normalize_peak=self.config.input_normalize_peak,
            max_gain=self.config.input_max_gain,
        )
        try:
            recorder.start()
            with self.state_lock:
                stale = session != self.session_id
                if not stale:
                    self.recorder = recorder
            if stale:
                recorder.close()
                self.service.logger.info("dropped recorder for stale session=%s", session)
                return
            self._set_state(AppState.RECORDING)
        except Exception as exc:
            with self.state_lock:
                if session == self.session_id:
                    self.recorder = None
            self.service.logger.exception("recording failed: %s", exc)
            self._set_state(AppState.ERROR)
            self._notify("LocalSTT error", str(exc))
            self._finish_recording()
            return

        # Give the remaining modifiers of the chord a moment to arrive before
        # deciding whether this is dictation, cleanup or a command.
        time.sleep(max(0.0, self.config.hotkey_mode_grace_seconds))
        with self.state_lock:
            if session != self.session_id or self.stop_in_progress:
                return
            mode = self._resolve_mode_locked()
        self.service.logger.info("recording started mode=%s session=%s", mode, session)
        self._notify("LocalSTT", f"Recording started ({mode})")
        if mode == "command" and self.config.command_auto_stop:
            self._set_state(AppState.COMMAND)
            threading.Thread(target=self._command_listen_loop, args=(session,), daemon=True).start()

    def _command_listen_loop(self, session: int) -> None:
        """Transcribe the running recording until a command matches, then stop by itself."""
        config = self.config
        started = time.perf_counter()
        deadline = started + config.command_listen_timeout_seconds
        next_poll = started + config.command_poll_seconds
        last_text = ""
        self.service.logger.info(
            "command listening started session=%s poll=%.2fs timeout=%.1fs",
            session,
            config.command_poll_seconds,
            config.command_listen_timeout_seconds,
        )
        while True:
            time.sleep(0.05)
            with self.state_lock:
                recorder = self.recorder
                if session != self.session_id or self.stop_in_progress or recorder is None:
                    self.service.logger.info("command listening handed over session=%s", session)
                    return
            now = time.perf_counter()
            if now >= deadline:
                self._finish_command_listening(session, last_text, "timeout")
                return
            if now < next_poll or recorder.duration_seconds() < config.command_min_audio_seconds:
                continue
            next_poll = now + config.command_poll_seconds

            text = self._transcribe_partial(recorder, session)
            if text is None:
                continue
            if text:
                last_text = text
            silence = recorder.tail_silence_seconds(
                threshold=config.command_silence_rms,
                max_seconds=config.command_silence_timeout_seconds + 1.0,
            )
            match = match_voice_command(text, config, self.service.logger)
            if match is not None:
                command, capture = match
                # A wildcard command ("создай задачу *") keeps growing while the user
                # is still talking, so wait for a pause before taking the captured text.
                if capture and silence < config.command_capture_silence_seconds:
                    self.service.logger.info(
                        "holding command %s until speech ends capture=%r silence=%.2fs",
                        command.get("name"),
                        capture,
                        silence,
                    )
                    continue
                self._execute_matched_command(session, text, command, capture)
                return
            if last_text and silence >= config.command_silence_timeout_seconds:
                self._finish_command_listening(session, last_text, "silence")
                return

    def _transcribe_partial(self, recorder: AudioRecorder, session: int) -> str | None:
        wav_path = Path(tempfile.gettempdir()) / f"localstt-cmd-{session}-{int(time.time() * 1000)}.wav"
        try:
            captured = recorder.snapshot_to_wav(wav_path)
            if captured < self.config.command_min_audio_seconds:
                return None
            # Greedy decoding is enough to spot a short command and keeps the poll cheap.
            return self.service.transcribe(wav_path, record=False, beam_size=1).text
        except Exception:
            self.service.logger.exception("command listening poll failed")
            return None
        finally:
            try:
                wav_path.unlink(missing_ok=True)
            except OSError:
                pass

    def _execute_matched_command(self, session: int, text: str, command: dict, capture: str) -> None:
        if not self._claim_stop(session):
            return
        self._stop_recorder_quietly()
        self._set_state(AppState.COMMAND)
        outcome = run_matched_command(
            text, command, capture, self.config, self.service.logger, self._run_app_command
        )
        self.service.logger.info(
            "voice command auto-stop executed=%s command=%s message=%s text=%r",
            outcome.executed,
            outcome.command_name,
            outcome.message,
            text[:120],
        )
        self._notify("LocalSTT command", outcome.message[:120])
        self._set_state(AppState.READY)
        self._finish_recording()

    def _finish_command_listening(self, session: int, text: str, reason: str) -> None:
        if not self._claim_stop(session):
            return
        self._stop_recorder_quietly()
        self._set_state(AppState.COMMAND)
        if text:
            message = execute_voice_command(
                text, self.config, self.service.logger, self._run_app_command
            ).message
        else:
            message = "No speech detected"
        self.service.logger.info(
            "command listening finished reason=%s message=%s text=%r", reason, message, text[:120]
        )
        self._notify("LocalSTT command", message[:120])
        self._set_state(AppState.READY)
        self._finish_recording()

    def _run_app_command(self, command: dict, capture: str) -> CommandOutcome:
        """Voice commands of type "localstt" that act on the app itself."""
        name = str(command.get("name", "localstt"))
        action = str(command.get("action", "")).strip().lower()
        value = str(command.get("value", "") or capture).strip().lower()

        if action == "language":
            if value not in self.config.allowed_languages:
                return CommandOutcome(False, f"Unknown language: {value}", name)
            self._set_language(value)
            return CommandOutcome(True, f"Language: {value}", name)

        if action == "delivery":
            if value not in {"paste", "typewrite"}:
                return CommandOutcome(False, f"Unknown delivery method: {value}", name)
            self._set_delivery_method(value)
            return CommandOutcome(True, f"Delivery: {value}", name)

        if action == "repeat":
            text = LAST_TRANSCRIPT_PATH.read_text(encoding="utf-8") if LAST_TRANSCRIPT_PATH.exists() else ""
            if not text.strip():
                return CommandOutcome(False, "No transcript to repeat", name)
            self._paste_text(text)
            return CommandOutcome(True, f"Repeated {len(text)} chars", name)

        return CommandOutcome(False, f"Unsupported app action: {action}", name)

    def _cancel_recording(self, session: int) -> None:
        """Drop the current recording without transcribing or running anything."""
        self._stop_recorder_quietly()
        self.service.logger.info("recording cancelled by Esc session=%s", session)
        self._notify("LocalSTT", "Recording cancelled")
        self._set_state(AppState.READY)
        self._finish_recording()

    def _stop_recorder_quietly(self) -> None:
        with self.state_lock:
            recorder = self.recorder
        if recorder is None:
            return
        try:
            recorder.close()
        except Exception:
            self.service.logger.warning("closing recorder failed", exc_info=True)

    def _stop_transcribe_paste(self, session: int = 0) -> None:
        with self.state_lock:
            recorder = self.recorder
            mode = self._resolve_mode_locked()
        if recorder is None:
            self.service.logger.warning("stop requested before recorder was ready")
            self._finish_recording()
            return

        wav_path = Path(tempfile.gettempdir()) / f"localstt-{int(time.time() * 1000)}.wav"
        try:
            self.service.logger.info(
                "stopping recorder chunks=%s last_status=%s",
                recorder.chunk_count(),
                recorder.last_status,
            )
            duration = recorder.stop_to_wav(wav_path)
            self.service.logger.info(
                "recording stopped duration=%.3fs peak=%.5f rms=%.5f gain=%.1fx file=%s",
                duration,
                recorder.last_peak,
                recorder.last_rms,
                recorder.last_gain,
                wav_path,
            )
            if recorder.last_peak < 0.01:
                self.service.logger.warning(
                    "microphone level is very low (peak=%.5f); check the input device, its Windows "
                    "level and the hardware mute button",
                    recorder.last_peak,
                )
            if duration < 0.15:
                self._set_state(AppState.READY)
                wav_path.unlink(missing_ok=True)
                self._notify("LocalSTT", "Recording was too short")
                self._finish_recording()
                return
            self._set_state(AppState.TRANSCRIBING)
            result = self.service.transcribe(wav_path)
            text = result.text
            if mode == "command":
                self._set_state(AppState.COMMAND)
                outcome = execute_voice_command(
                    text, self.config, self.service.logger, self._run_app_command
                )
                self.service.logger.info(
                    "voice command result executed=%s command=%s message=%s",
                    outcome.executed,
                    outcome.command_name,
                    outcome.message,
                )
                self._notify("LocalSTT command", outcome.message[:120])
            else:
                if mode == "cleanup":
                    self._set_state(AppState.CLEANUP)
                    text = polish_text(text, self.config, self.service.logger)
                if text:
                    self._paste_text(text)
                    self._notify("LocalSTT", f"Pasted {len(text)} chars")
                else:
                    self.service.logger.warning("transcription returned empty text")
                    self._notify("LocalSTT", "Whisper returned empty text")
            wav_path.unlink(missing_ok=True)
            self._set_state(AppState.READY)
            self._finish_recording()
        except Exception as exc:
            self.service.logger.exception("transcription failed: %s", exc)
            self._set_state(AppState.ERROR)
            self._notify("LocalSTT error", str(exc))
            try:
                wav_path.unlink(missing_ok=True)
            except OSError:
                pass
            self._finish_recording()

    def _finish_recording(self) -> None:
        with self.state_lock:
            self.recorder = None
            self.recording_mode = None
            self.chord_modifiers = set()
            self.stop_in_progress = False
            self.hotkey_down_at = None
            self.target_hwnd = None

    def _save_last_transcript(self, text: str) -> None:
        LAST_TRANSCRIPT_PATH.parent.mkdir(parents=True, exist_ok=True)
        LAST_TRANSCRIPT_PATH.write_text(text, encoding="utf-8")

    def _paste_text(self, text: str) -> None:
        old = pyperclip.paste()
        self._save_last_transcript(text)
        clipboard_ready = self._set_clipboard_text(text)
        hotkeys_released = self._wait_for_hotkeys_released()
        focused = set_foreground_window(self.target_hwnd)
        method = self.config.delivery_method.lower().strip()
        self.service.logger.info(
            "delivering %s chars method=%s focused_target=%s hotkeys_released=%s clipboard_ready=%s",
            len(text),
            method,
            focused,
            hotkeys_released,
            clipboard_ready,
        )
        time.sleep(0.25)
        try:
            if method == "typewrite":
                delivered = text_input.type_unicode_text(text, self.config.typewrite_interval_seconds)
                self.service.logger.info("typewrite delivery result=%s chars=%s", delivered, len(text))
                if not delivered:
                    self.service.logger.warning(
                        "typewrite failed (SendInput last error=%s), falling back to clipboard paste",
                        text_input.last_error,
                    )
                    self._send_ctrl_v()
            else:
                self._send_ctrl_v()
        except Exception:
            self.service.logger.exception("text delivery failed; transcript remains in clipboard and last-transcript.txt")

        if self.config.restore_clipboard_after_paste:
            time.sleep(self.config.paste_restore_delay_seconds)
            try:
                if pyperclip.paste() == text:
                    pyperclip.copy(old)
            except Exception:
                self.service.logger.warning("clipboard restore failed", exc_info=True)

    def _send_ctrl_v(self) -> None:
        controller = keyboard.Controller()
        with controller.pressed(keyboard.Key.ctrl):
            controller.press("v")
            controller.release("v")

    def _set_clipboard_text(self, text: str) -> bool:
        for attempt in range(1, 6):
            try:
                pyperclip.copy(text)
                if pyperclip.paste() == text:
                    return True
            except Exception as exc:
                self.service.logger.warning("clipboard copy attempt %s failed: %s", attempt, exc)
            time.sleep(0.05 * attempt)
        return False

    def _wait_for_hotkeys_released(self) -> bool:
        deadline = time.perf_counter() + 1.5
        while time.perf_counter() < deadline:
            with self.state_lock:
                if not ({"ctrl", "win", "shift"} & self.pressed):
                    return True
            time.sleep(0.02)
        return False

    def _show_menu(self) -> None:
        tray_menu.popup(self._menu_items())

    def _menu_items(self) -> list[MenuItem]:
        return [
            MenuItem("Settings", self._open_settings, icon="\ue713"),
            MenuItem("Run self-test", self._run_selftest, icon="\ue9d9"),
            separator(),
            MenuItem("Language", icon="\uf2b7", submenu=self._language_items()),
            MenuItem("Microphone", icon="\ue720", submenu=self._microphone_items()),
            MenuItem("Delivery", icon="\ue765", submenu=self._delivery_items()),
            MenuItem("Model", icon="\ue945", submenu=self._model_items()),
            MenuItem(
                "Command auto-stop",
                self._toggle_command_auto_stop,
                icon="\ue916",
                checked=self.config.command_auto_stop,
            ),
            separator(),
            MenuItem("Last transcript", lambda: self._open_path(LAST_TRANSCRIPT_PATH), icon="\ue8a5"),
            MenuItem("Command history", lambda: self._open_path(COMMAND_HISTORY_PATH), icon="\ue81c"),
            MenuItem("Open logs", lambda: self._open_path(LOG_PATH), icon="\ue9d5"),
            separator(),
            MenuItem("Restart LocalSTT", self._restart, icon="\ue72c"),
            MenuItem("Exit", self._exit, icon="\ue7e8"),
        ]

    def _microphone_items(self) -> list[MenuItem]:
        items = [
            MenuItem(
                f"{mic['index']}: {mic['name']}",
                (lambda index=mic["index"]: self._set_microphone(index)),
                checked=self.config.microphone == mic["index"],
                radio=True,
            )
            for mic in list_microphones()[:20]
        ]
        return items or [MenuItem("No microphones found", enabled=False)]

    def _model_items(self) -> list[MenuItem]:
        return [
            MenuItem(
                name,
                (lambda value=name: self._set_model(value)),
                checked=self.config.model == name,
                radio=True,
            )
            for name in self.config.allowed_models
        ]

    def _language_items(self) -> list[MenuItem]:
        labels = {"ru": "Russian", "en": "English", "auto": "Auto ru/en"}
        return [
            MenuItem(
                labels.get(language, language),
                (lambda value=language: self._set_language(value)),
                checked=self.config.language == language,
                radio=True,
            )
            for language in self.config.allowed_languages
        ]

    def _delivery_items(self) -> list[MenuItem]:
        labels = {"paste": "Paste", "typewrite": "Typewrite"}
        return [
            MenuItem(
                labels.get(method, method),
                (lambda value=method: self._set_delivery_method(value)),
                checked=self.config.delivery_method == method,
                radio=True,
            )
            for method in ["paste", "typewrite"]
        ]

    def _open_settings(self) -> None:
        settings_window.open_settings(self.config, self.service.logger, self._on_settings_saved)

    def _run_selftest(self) -> None:
        settings_window.open_settings(
            self.config, self.service.logger, self._on_settings_saved,
            section="selftest", run_selftest=True,
        )

    def _on_settings_saved(self, keys: list[str]) -> None:
        if "language" in keys:
            self.service.backend.language = self.config.language
        if "beam_size" in keys:
            self.service.backend.beam_size = self.config.beam_size
        if "vad_filter" in keys:
            self.service.backend.vad_filter = self.config.vad_filter
        self._notify("LocalSTT", "Settings saved")

    def _set_delivery_method(self, method: str) -> None:
        self.config.delivery_method = method
        save_config(self.config)
        self.service.logger.info("delivery method changed to %s", method)
        self._notify("LocalSTT delivery", method)
    def _toggle_command_auto_stop(self) -> None:
        self.config.command_auto_stop = not self.config.command_auto_stop
        save_config(self.config)
        state = "on" if self.config.command_auto_stop else "off"
        self.service.logger.info("command auto-stop set to %s", state)
        self._notify("LocalSTT commands", f"Auto-stop: {state}")

    def _set_microphone(self, index: int) -> None:
        self.config.microphone = index
        save_config(self.config)

    def _set_language(self, language: str) -> None:
        self.config.language = language
        self.service.backend.language = language
        save_config(self.config)
        self.service.logger.info("language changed to %s", language)
        self._notify("LocalSTT language", language)

    def _set_model(self, model: str) -> None:
        self.config.model = model
        save_config(self.config)
        self._restart()

    def _open_history(self, *args) -> None:
        self._open_path(APPDATA_DIR / "history.jsonl")

    def _open_path(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.touch()
        try:
            os.startfile(str(path))
        except OSError as exc:
            self.service.logger.warning("Windows file association failed for %s: %s", path, exc)
            try:
                subprocess.Popen(["notepad.exe", str(path)])
            except Exception:
                self.service.logger.exception("failed to open %s in Notepad", path)
                self._notify("LocalSTT", f"Could not open {path}")

    def _restart(self) -> None:
        subprocess.Popen([sys.executable, "-m", "localstt.main"], cwd="C:/Apps/LocalSTT")
        self._exit()

    def _exit(self) -> None:
        if self.listener:
            self.listener.stop()
        self.icon.stop()

    def _set_state(self, state: AppState) -> None:
        self.state = state
        self.icon.icon = self._image(state)
        self.icon.title = f"LocalSTT - {state.value}"
        self.service.logger.info("state=%s", state.value)

    def _notify(self, title: str, message: str) -> None:
        try:
            self.icon.notify(message, title)
        except Exception:
            self.service.logger.debug("tray notification failed", exc_info=True)

    def _image(self, state: AppState) -> Image.Image:
        """The design lives on a 64px grid; ICON_SIZE only controls how crisply it renders."""
        unit = ICON_SIZE / 64.0

        def box(*points: float) -> tuple[float, ...]:
            # Scale about the centre of the grid, so the glyph grows into its padding.
            return tuple(((value - 32.0) * ICON_GLYPH_SCALE + 32.0) * unit for value in points)

        image = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse(
            box(8, 8, 56, 56),
            fill=COLORS[state],
            outline="#202020",
            width=max(1, round(3 * ICON_GLYPH_SCALE * unit)),
        )
        draw.rectangle(box(29, 18, 35, 43), fill="white")
        draw.arc(
            box(21, 30, 43, 52), 0, 180,
            fill="white",
            width=max(1, round(4 * ICON_GLYPH_SCALE * unit)),
        )
        return image

    def _key_name(self, key) -> str | None:
        if key in (keyboard.Key.ctrl, keyboard.Key.ctrl_l, keyboard.Key.ctrl_r):
            return "ctrl"
        if key in (keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r):
            return "win"
        if key in (keyboard.Key.shift, keyboard.Key.shift_l, keyboard.Key.shift_r):
            return "shift"
        if key in (keyboard.Key.alt, keyboard.Key.alt_l, keyboard.Key.alt_r):
            return "alt"
        if key == keyboard.Key.esc:
            return "esc"
        return None
