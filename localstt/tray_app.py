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
from .config import APPDATA_DIR, CONFIG_PATH, LAST_TRANSCRIPT_PATH, LOG_PATH, AppConfig, save_config
from .ollama_cleanup import polish_text
from .service import STTService
from .window_focus import get_foreground_window, set_foreground_window


class AppState(str, Enum):
    LOADING = "loading"
    READY = "ready"
    RECORDING = "recording"
    TRANSCRIBING = "transcribing"
    CLEANUP = "cleanup"
    ERROR = "error"


COLORS = {
    AppState.LOADING: "#777777",
    AppState.READY: "#19a463",
    AppState.RECORDING: "#d83b35",
    AppState.TRANSCRIBING: "#2d7ff9",
    AppState.CLEANUP: "#8a4de8",
    AppState.ERROR: "#e0b400",
}


class LocalSTTTrayApp:
    def __init__(self, config: AppConfig, service: STTService) -> None:
        self.config = config
        self.service = service
        self.state = AppState.LOADING
        self.icon = pystray.Icon("LocalSTT", self._image(self.state), "LocalSTT", self._menu())
        self.recorder: AudioRecorder | None = None
        self.pressed: set[str] = set()
        self.recording_mode: str | None = None
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
            "global hotkeys ready: hold Ctrl+Win to dictate, tap Ctrl+Win again to stop toggle recording"
        )

    def _on_press(self, key) -> None:
        name = self._key_name(key)
        should_start = False
        should_stop = False
        mode = "dictation"
        with self.state_lock:
            if name:
                self.pressed.add(name)
            if self._ctrl_win_down() and not self.stop_in_progress:
                if self.recording_mode is None:
                    mode = "cleanup" if "shift" in self.pressed else "dictation"
                    self.recording_mode = mode
                    self.hotkey_down_at = time.perf_counter()
                    self.target_hwnd = get_foreground_window()
                    should_start = True
                else:
                    should_stop = True
                    self.stop_in_progress = True
        if should_start:
            threading.Thread(target=self._start_recording, args=(mode,), daemon=True).start()
        elif should_stop:
            self.service.logger.info("recording stop requested by repeated Ctrl+Win press")
            threading.Thread(target=self._stop_transcribe_paste, daemon=True).start()

    def _on_release(self, key) -> None:
        name = self._key_name(key)
        should_stop = False
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
                should_stop = True
                self.stop_in_progress = True
        if should_stop:
            self.service.logger.info("recording stop requested by hotkey release")
            threading.Thread(target=self._stop_transcribe_paste, daemon=True).start()

    def _ctrl_win_down(self) -> bool:
        return "ctrl" in self.pressed and "win" in self.pressed

    def _start_recording(self, mode: str) -> None:
        recorder = AudioRecorder(
            sample_rate=self.config.sample_rate,
            channels=self.config.channels,
            device=self.config.microphone,
        )
        try:
            recorder.start()
            with self.state_lock:
                self.recorder = recorder
            self._set_state(AppState.RECORDING)
            self._notify("LocalSTT", f"Recording started ({mode})")
            self.service.logger.info("recording started mode=%s", mode)
        except Exception as exc:
            with self.state_lock:
                self.recording_mode = None
                self.stop_in_progress = False
                self.hotkey_down_at = None
                self.target_hwnd = None
                self.recorder = None
            self.service.logger.exception("recording failed: %s", exc)
            self._set_state(AppState.ERROR)
            self._notify("LocalSTT error", str(exc))

    def _stop_transcribe_paste(self) -> None:
        with self.state_lock:
            recorder = self.recorder
            mode = self.recording_mode
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
            self.service.logger.info("recording stopped duration=%.3fs file=%s", duration, wav_path)
            if duration < 0.15:
                self._set_state(AppState.READY)
                wav_path.unlink(missing_ok=True)
                self._notify("LocalSTT", "Recording was too short")
                self._finish_recording()
                return
            self._set_state(AppState.TRANSCRIBING)
            result = self.service.transcribe(wav_path)
            text = result.text
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
            self.stop_in_progress = False
            self.hotkey_down_at = None
            self.target_hwnd = None

    def _paste_text(self, text: str) -> None:
        old = pyperclip.paste()
        LAST_TRANSCRIPT_PATH.parent.mkdir(parents=True, exist_ok=True)
        LAST_TRANSCRIPT_PATH.write_text(text, encoding="utf-8")
        clipboard_ready = self._set_clipboard_text(text)
        hotkeys_released = self._wait_for_hotkeys_released()
        focused = set_foreground_window(self.target_hwnd)
        self.service.logger.info(
            "pasting %s chars focused_target=%s hotkeys_released=%s clipboard_ready=%s",
            len(text),
            focused,
            hotkeys_released,
            clipboard_ready,
        )
        time.sleep(0.25)
        controller = keyboard.Controller()
        with controller.pressed(keyboard.Key.ctrl):
            controller.press("v")
            controller.release("v")
        if self.config.restore_clipboard_after_paste:
            time.sleep(self.config.paste_restore_delay_seconds)
            try:
                if pyperclip.paste() == text:
                    pyperclip.copy(old)
            except Exception:
                self.service.logger.warning("clipboard restore failed", exc_info=True)

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

    def _menu(self) -> pystray.Menu:
        return pystray.Menu(
            pystray.MenuItem("Settings", lambda *args: self._open_path(CONFIG_PATH)),
            pystray.MenuItem("Microphone", pystray.Menu(*self._microphone_items())),
            pystray.MenuItem("Language", pystray.Menu(*self._language_items())),
            pystray.MenuItem("Model", pystray.Menu(*self._model_items())),
            pystray.MenuItem("CUDA diagnostics", self._run_diagnostics),
            pystray.MenuItem("History", self._open_history),
            pystray.MenuItem("Last transcript", lambda *args: self._open_path(LAST_TRANSCRIPT_PATH)),
            pystray.MenuItem("Open logs", lambda *args: self._open_path(LOG_PATH)),
            pystray.MenuItem("Restart STT", self._restart),
            pystray.MenuItem("Exit", self._exit),
        )

    def _microphone_items(self) -> list[pystray.MenuItem]:
        items = []
        for mic in list_microphones()[:20]:
            label = f"{mic['index']}: {mic['name']}"
            items.append(pystray.MenuItem(label, lambda *args, i=mic["index"]: self._set_microphone(i)))
        return items or [pystray.MenuItem("No microphones found", lambda *args: None, enabled=False)]

    def _model_items(self) -> list[pystray.MenuItem]:
        return [pystray.MenuItem(m, lambda *args, name=m: self._set_model(name)) for m in self.config.allowed_models]

    def _language_items(self) -> list[pystray.MenuItem]:
        labels = {
            "ru": "Russian",
            "en": "English",
            "auto": "Auto ru/en",
        }
        return [
            pystray.MenuItem(
                labels.get(lang, lang),
                lambda *args, value=lang: self._set_language(value),
                checked=lambda item, value=lang: self.config.language == value,
                radio=True,
            )
            for lang in self.config.allowed_languages
        ]

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

    def _run_diagnostics(self, *args) -> None:
        subprocess.Popen([sys.executable, str(Path("C:/Apps/LocalSTT/diagnostics.py"))], cwd="C:/Apps/LocalSTT")

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

    def _restart(self, *args) -> None:
        subprocess.Popen([sys.executable, "-m", "localstt.main"], cwd="C:/Apps/LocalSTT")
        self._exit()

    def _exit(self, *args) -> None:
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
        image = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse((8, 8, 56, 56), fill=COLORS[state], outline="#202020", width=3)
        draw.rectangle((29, 18, 35, 43), fill="white")
        draw.arc((21, 30, 43, 52), 0, 180, fill="white", width=4)
        return image

    def _key_name(self, key) -> str | None:
        if key in (keyboard.Key.ctrl, keyboard.Key.ctrl_l, keyboard.Key.ctrl_r):
            return "ctrl"
        if key in (keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r):
            return "win"
        if key in (keyboard.Key.shift, keyboard.Key.shift_l, keyboard.Key.shift_r):
            return "shift"
        return None
