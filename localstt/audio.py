from __future__ import annotations

import queue
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


class AudioRecorder:
    def __init__(self, *, sample_rate: int = 16000, channels: int = 1, device: int | None = None) -> None:
        self.sample_rate = sample_rate
        self.channels = channels
        self.device = device
        self._stream: sd.InputStream | None = None
        self._chunks: queue.Queue[np.ndarray] = queue.Queue()
        self.last_status: str | None = None

    def start(self) -> None:
        if self._stream is not None:
            return
        self._chunks = queue.Queue()
        self._stream = sd.InputStream(
            samplerate=self.sample_rate,
            channels=self.channels,
            dtype="float32",
            device=self.device,
            callback=self._callback,
        )
        self._stream.start()

    def stop_to_wav(self, path: Path) -> float:
        if self._stream is None:
            return 0.0
        self._stream.stop()
        self._stream.close()
        self._stream = None

        chunks = []
        while not self._chunks.empty():
            chunks.append(self._chunks.get())
        if not chunks:
            return 0.0

        audio = np.concatenate(chunks, axis=0)
        audio = np.clip(audio, -1.0, 1.0)
        pcm = (audio * 32767).astype(np.int16)
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), "wb") as wf:
            wf.setnchannels(self.channels)
            wf.setsampwidth(2)
            wf.setframerate(self.sample_rate)
            wf.writeframes(pcm.tobytes())
        return len(audio) / float(self.sample_rate)

    def chunk_count(self) -> int:
        return self._chunks.qsize()

    def _callback(self, indata, frames, time, status) -> None:
        if status:
            self.last_status = str(status)
        self._chunks.put(indata.copy())
