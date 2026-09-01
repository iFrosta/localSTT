from __future__ import annotations

import time
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .cuda_paths import configure_cuda_dll_search

CUDA_DLL_DIRS = configure_cuda_dll_search()

import ctranslate2  # noqa: E402
from faster_whisper import WhisperModel  # noqa: E402


def normalize_language(language: str | None) -> str | None:
    if language is None:
        return None
    value = language.strip().lower()
    if not value or value in {"auto", "detect", "autodetect"}:
        return None
    return value


@dataclass
class TranscriptionResult:
    text: str
    language: str | None
    duration: float
    processing_time: float
    backend: str
    device: str
    model: str
    compute_type: str
    segments: list[dict[str, Any]]


class ASRBackend(ABC):
    @abstractmethod
    def load(self) -> None:
        raise NotImplementedError

    @abstractmethod
    def transcribe(
        self,
        audio: str | Path | Any,
        *,
        language: str | None = None,
        beam_size: int | None = None,
        vad_filter: bool | None = None,
    ) -> TranscriptionResult:
        raise NotImplementedError

    @abstractmethod
    def health(self) -> dict[str, Any]:
        raise NotImplementedError


class FasterWhisperBackend(ASRBackend):
    name = "faster-whisper"

    def __init__(
        self,
        *,
        model_name: str,
        device: str,
        compute_type: str,
        language: str,
        beam_size: int,
        vad_filter: bool,
        initial_prompt: str = "",
    ) -> None:
        self.model_name = model_name
        self.device = device
        self.compute_type = compute_type
        self.language = language
        self.beam_size = beam_size
        self.vad_filter = vad_filter
        self.initial_prompt = initial_prompt
        self.model: WhisperModel | None = None

    def load(self) -> None:
        if self.device != "cuda":
            raise RuntimeError("CPU fallback is disabled. Configure device='cuda'.")
        count = ctranslate2.get_cuda_device_count()
        if count < 1:
            raise RuntimeError("CUDA initialization failed: CTranslate2 reports zero CUDA devices.")
        self.model = WhisperModel(self.model_name, device=self.device, compute_type=self.compute_type)

    def transcribe(
        self,
        audio: str | Path | Any,
        *,
        language: str | None = None,
        beam_size: int | None = None,
        vad_filter: bool | None = None,
    ) -> TranscriptionResult:
        if self.model is None:
            self.load()

        whisper_language = normalize_language(language) if language is not None else normalize_language(self.language)
        start = time.perf_counter()
        segments_iter, info = self.model.transcribe(
            audio,
            language=whisper_language,
            beam_size=beam_size or self.beam_size,
            vad_filter=self.vad_filter if vad_filter is None else vad_filter,
            initial_prompt=self.initial_prompt or None,
        )
        segments = []
        parts = []
        for segment in segments_iter:
            parts.append(segment.text)
            segments.append({"start": segment.start, "end": segment.end, "text": segment.text})

        processing_time = time.perf_counter() - start
        duration = float(getattr(info, "duration", 0.0) or 0.0)
        return TranscriptionResult(
            text="".join(parts).strip(),
            language=getattr(info, "language", whisper_language),
            duration=duration,
            processing_time=processing_time,
            backend=self.name,
            device=self.device,
            model=self.model_name,
            compute_type=self.compute_type,
            segments=segments,
        )

    def health(self) -> dict[str, Any]:
        return {
            "backend": self.name,
            "model": self.model_name,
            "device": self.device,
            "compute_type": self.compute_type,
            "language": self.language,
            "cuda_devices": ctranslate2.get_cuda_device_count(),
            "cuda_dll_dirs": [str(p) for p in CUDA_DLL_DIRS],
        }


class ParakeetBackend(ASRBackend):
    def load(self) -> None:
        raise NotImplementedError("Parakeet backend is reserved for future integration.")

    def transcribe(self, audio: str | Path | Any, **kwargs: Any) -> TranscriptionResult:
        raise NotImplementedError("Parakeet backend is reserved for future integration.")

    def health(self) -> dict[str, Any]:
        return {"backend": "parakeet", "status": "not-installed"}
