from __future__ import annotations

import threading
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd


def list_microphones() -> list[dict]:
    devices = sd.query_devices()
    result = []
    for index, device in enumerate(devices):
        if int(device.get("max_input_channels", 0)) > 0:
            result.append(
                {
                    "index": index,
                    "name": device.get("name", f"Input {index}"),
                    "default_samplerate": device.get("default_samplerate"),
                }
            )
    return result


def default_microphone_index() -> int | None:
    """The device Windows hands us when config.microphone is None."""
    try:
        index = sd.default.device[0]
    except Exception:
        return None
    return index if isinstance(index, int) and index >= 0 else None


class AudioRecorder:
    def __init__(
        self,
        *,
        sample_rate: int = 16000,
        channels: int = 1,
        device: int | None = None,
        normalize_peak: float = 0.0,
        max_gain: float = 1.0,
    ) -> None:
        self.sample_rate = sample_rate
        self.channels = channels
        self.device = device
        self.normalize_peak = normalize_peak
        self.max_gain = max_gain
        self._stream: sd.InputStream | None = None
        self._lock = threading.Lock()
        self._chunks: list[np.ndarray] = []
        self._frames = 0
        self.last_status: str | None = None
        self.last_peak = 0.0
        self.last_rms = 0.0
        self.last_gain = 1.0

    def start(self) -> None:
        if self._stream is not None:
            return
        with self._lock:
            self._chunks = []
            self._frames = 0
        self._stream = sd.InputStream(
            samplerate=self.sample_rate,
            channels=self.channels,
            dtype="float32",
            device=self.device,
            callback=self._callback,
        )
        self._stream.start()

    def close(self) -> None:
        """Stop the input stream without producing a wav file."""
        stream, self._stream = self._stream, None
        if stream is None:
            return
        stream.stop()
        stream.close()

    def stop_to_wav(self, path: Path) -> float:
        if self._stream is None:
            return 0.0
        self.close()
        return self.snapshot_to_wav(path)

    def snapshot_to_wav(self, path: Path) -> float:
        """Write everything recorded so far to a wav file, leaving the stream untouched."""
        audio = self._buffered_audio()
        if audio is None:
            return 0.0

        audio = self._apply_gain(audio)
        pcm = (audio * 32767).astype(np.int16)
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), "wb") as wf:
            wf.setnchannels(self.channels)
            wf.setsampwidth(2)
            wf.setframerate(self.sample_rate)
            wf.writeframes(pcm.tobytes())
        return len(audio) / float(self.sample_rate)

    def _apply_gain(self, audio: np.ndarray) -> np.ndarray:
        """Lift a quiet microphone up to a usable level; Whisper's VAD drops near-silence."""
        self.last_peak = float(np.max(np.abs(audio)))
        self.last_rms = float(np.sqrt(np.mean(np.square(audio))))
        self.last_gain = 1.0
        if self.normalize_peak > 0.0 and self.max_gain > 1.0 and self.last_peak > 1e-5:
            if self.last_peak < self.normalize_peak:
                self.last_gain = min(self.normalize_peak / self.last_peak, self.max_gain)
                audio = audio * self.last_gain
        return np.clip(audio, -1.0, 1.0)

    def duration_seconds(self) -> float:
        with self._lock:
            return self._frames / float(self.sample_rate)

    def tail_silence_seconds(
        self,
        *,
        threshold: float = 0.008,
        max_seconds: float = 6.0,
        window_seconds: float = 0.05,
    ) -> float:
        """How long the recording has been quiet at the tail, capped at max_seconds."""
        tail = self._tail_audio(max_seconds)
        if tail is None:
            return 0.0

        mono = tail.reshape(len(tail), -1).mean(axis=1)
        window = max(1, int(window_seconds * self.sample_rate))
        silent = 0.0
        for start in range(len(mono) - window, -1, -window):
            block = mono[start : start + window]
            if float(np.sqrt(np.mean(np.square(block)))) >= threshold:
                break
            silent += window / float(self.sample_rate)
        return silent

    def chunk_count(self) -> int:
        with self._lock:
            return len(self._chunks)

    def _buffered_audio(self) -> np.ndarray | None:
        with self._lock:
            if not self._chunks:
                return None
            return np.concatenate(self._chunks, axis=0)

    def _tail_audio(self, max_seconds: float) -> np.ndarray | None:
        needed = max(1, int(max_seconds * self.sample_rate))
        with self._lock:
            if not self._chunks:
                return None
            collected: list[np.ndarray] = []
            total = 0
            for chunk in reversed(self._chunks):
                collected.append(chunk)
                total += len(chunk)
                if total >= needed:
                    break
            return np.concatenate(list(reversed(collected)), axis=0)

    def _callback(self, indata, frames, time, status) -> None:
        if status:
            self.last_status = str(status)
        with self._lock:
            self._chunks.append(indata.copy())
            self._frames += len(indata)
