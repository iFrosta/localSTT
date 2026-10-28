"""Incremental dictation. Only confirmed words are committed, never rewritten.

Whisper is polled on the uncommitted audio. Two matching hypotheses confirm a
prefix; a speech pause or the stop hotkey flushes the remaining words. Word
end times advance the audio cursor, so the final pass cannot type a prefix twice.
"""
from __future__ import annotations

import tempfile
import threading
from dataclasses import replace
from pathlib import Path
from typing import Callable


class LiveDictation:
    POLL_SECONDS = 0.9
    MAX_WINDOW_SECONDS = 25.0
    MIN_AUDIO_SECONDS = 0.5

    def __init__(self, recorder, service, deliver: Callable[[str], int]) -> None:
        self.recorder = recorder
        self.service = service
        # Returns the number of characters actually sent. Zero defers delivery.
        self.deliver = deliver
        self.stopped = threading.Event()
        self.cancelled = threading.Event()
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.run, name="live-dictation", daemon=True)
        self.text = ""
        self.delivered = 0
        self.offset = 0.0
        self.previous: list[dict] = []
        self.processing_time = 0.0
        self.last_result = None
        self.error: Exception | None = None

    def start(self) -> None:
        self.thread.start()

    def cancel(self) -> None:
        self.cancelled.set()
        self.stopped.set()

    def flush_delivery(self) -> None:
        if self.cancelled.is_set() or self.delivered >= len(self.text):
            return
        count = self.deliver(self.text[self.delivered:])
        if not 0 <= count <= len(self.text) - self.delivered:
            raise ValueError("invalid live delivery count")
        self.delivered += count
        if count:
            self.service.logger.info("live typed chars=%s total=%s recording=%s",
                                     count, self.delivered, not self.stopped.is_set())

    def accept(self, result, duration: float, *, flush: bool) -> None:
        """Commit a stable prefix using times relative to this audio window."""
        words = [w for segment in result.segments for w in segment.get("words", [])
                 if str(w.get("word", "")).strip() and 0 <= w["start"] < w["end"] <= duration + 0.05]
        if flush:
            selected = words
        else:
            stable = 0
            for old, new in zip(self.previous, words):
                if old["word"].strip() != new["word"].strip():
                    break
                stable += 1
            # The trailing word can still change while the speaker completes it.
            keep = max(0, len(words) - 1)
            if len(words) == stable == 1 and duration >= self.MAX_WINDOW_SECONDS:
                keep = 1  # do not stall on one word followed by a long silence
            selected = words[:min(stable, keep)]
        self.previous = words
        if not selected:
            if flush or (not words and duration >= self.MAX_WINDOW_SECONDS):
                # Empty/silent windows must not cause the final loop to stall.
                # Some backends omit word timings; only a final window may use text.
                if not words and result.text.strip():
                    self._append(result.text)
                self.offset += duration
                self.previous = []
            return
        self._append(self.service.dictionary.apply("".join(w["word"] for w in selected)))
        self.offset += duration if flush else min(duration, float(selected[-1]["end"]))
        self.previous = []

    def _append(self, text: str) -> None:
        text = text.strip()
        if text:
            separator = " " if self.text and text[0] not in ",.!?:;)]}" else ""
            self.text += separator + text

    def run(self) -> None:
        try:
            with tempfile.TemporaryDirectory(prefix="localstt-live-") as tmp:
                path = Path(tmp) / "chunk.wav"
                while not self.cancelled.is_set():
                    final = self.stopped.is_set()
                    remaining = self.recorder.duration_seconds() - self.offset
                    if remaining < (0.15 if final else self.MIN_AUDIO_SECONDS):
                        self.flush_delivery()
                        if final:
                            break
                        self.stopped.wait(self.POLL_SECONDS)
                        continue
                    pause = self.recorder.tail_silence_seconds(
                        threshold=self.service.config.command_silence_rms, max_seconds=1.0,
                    ) >= 0.65
                    duration = self.recorder.snapshot_to_wav(
                        path, start_seconds=self.offset, max_seconds=self.MAX_WINDOW_SECONDS,
                    )
                    if duration <= 0:
                        break
                    result = self.service.transcribe(
                        path, record=False, beam_size=1, word_timestamps=True,
                    )
                    self.last_result = result
                    self.processing_time += result.processing_time
                    if self.cancelled.is_set():
                        break
                    # Flush only the window whose end was observed. Audio recorded
                    # during inference belongs to the next window, even on Stop.
                    flush = (final or pause) and remaining <= self.MAX_WINDOW_SECONDS
                    self.accept(result, duration, flush=flush)
                    self.flush_delivery()
                    if not self.stopped.is_set():
                        self.stopped.wait(self.POLL_SECONDS)
        except Exception as exc:
            self.error = exc
            self.service.logger.exception("live dictation failed")
        finally:
            self.done.set()

    def result(self):
        if self.last_result is None:
            return None
        return replace(
            self.last_result, text=self.text, duration=self.recorder.duration_seconds(),
            processing_time=self.processing_time, segments=[],
        )
