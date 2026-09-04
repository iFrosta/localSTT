from __future__ import annotations

import json
import logging
import subprocess
import threading
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from . import performance
from .backends import FasterWhisperBackend, TranscriptionResult
from .config import AppConfig, HISTORY_PATH
from .dictionary import DevelopmentDictionary


@dataclass
class Metrics:
    transcription_count: int = 0
    total_audio_duration: float = 0.0
    total_processing_time: float = 0.0

    @property
    def average_processing_time(self) -> float:
        if self.transcription_count == 0:
            return 0.0
        return self.total_processing_time / self.transcription_count

    @property
    def realtime_factor(self) -> float:
        if self.total_audio_duration <= 0:
            return 0.0
        return self.total_processing_time / self.total_audio_duration


class STTService:
    def __init__(self, config: AppConfig, logger: logging.Logger) -> None:
        self.config = config
        self.logger = logger
        self.dictionary = DevelopmentDictionary.load()
        self.backend = FasterWhisperBackend(
            model_name=config.model,
            device=config.device,
            compute_type=config.compute_type,
            language=config.language,
            beam_size=config.beam_size,
            vad_filter=config.vad_filter,
            initial_prompt=self.dictionary.initial_prompt(),
        )
        self.lock = threading.Lock()
        self.metrics = Metrics()
        self.gpu_name = self._detect_gpu_name()

    def load(self) -> None:
        self.logger.info("STT ENGINE: faster-whisper")
        self.logger.info("MODEL: %s", self.config.model)
        self.logger.info("DEVICE: %s", self.config.device)
        self.logger.info("COMPUTE: %s", self.config.compute_type)
        self.logger.info("LANGUAGE: %s", self.config.language)
        self.logger.info("GPU: %s", self.gpu_name)
        self.backend.load()

    def _refresh_dictionary(self) -> None:
        """Re-read dictionary.json when it changes, so an edit needs no restart."""
        if not self.dictionary.is_stale():
            return
        self.dictionary = DevelopmentDictionary.load()
        self.backend.initial_prompt = self.dictionary.initial_prompt()
        self.logger.info(
            "dictionary reloaded: %s terms, %s replacements",
            len(self.dictionary.terms),
            len(self.dictionary.replacements),
        )
        overflow = self.dictionary.prompt_overflow()
        if overflow:
            self.logger.warning(
                "the term list is %s characters past what Whisper reads; the last terms "
                "are ignored. Move the rarely-spoken ones to replacements instead.",
                overflow,
            )

    def transcribe(
        self,
        audio: str | Path | Any,
        *,
        language: str | None = None,
        response_format: str = "json",
        beam_size: int | None = None,
        record: bool = True,
    ) -> TranscriptionResult:
        with self.lock:
            self._refresh_dictionary()
            result = self.backend.transcribe(audio, language=language, beam_size=beam_size)
        result.text = self.dictionary.apply(result.text)
        if record:
            self.metrics.transcription_count += 1
            self.metrics.total_audio_duration += result.duration
            self.metrics.total_processing_time += result.processing_time
            self._record_history(result)
            if self.config.performance_tracking:
                performance.record_transcription(result, self.config, self.metrics_snapshot())
        self.logger.info(
            "transcribed duration=%.2fs processing=%.2fs rtf=%.3f text=%r",
            result.duration,
            result.processing_time,
            result.processing_time / result.duration if result.duration else 0.0,
            result.text[:120],
        )
        return result

    def health(self) -> dict[str, Any]:
        data = {
            "status": "ok",
            "backend": "faster-whisper",
            "model": self.config.model,
            "device": self.config.device,
            "compute_type": self.config.compute_type,
            "language": self.config.language,
            "gpu": self.gpu_name,
        }
        data.update({k: v for k, v in self.backend.health().items() if k not in data})
        return data

    def metrics_snapshot(self) -> dict[str, Any]:
        return {
            "transcription_count": self.metrics.transcription_count,
            "total_audio_duration": self.metrics.total_audio_duration,
            "average_processing_time": self.metrics.average_processing_time,
            "realtime_factor": self.metrics.realtime_factor,
        }

    def _record_history(self, result: TranscriptionResult) -> None:
        if not self.config.history_enabled:
            return
        HISTORY_PATH.parent.mkdir(parents=True, exist_ok=True)
        row = {
            "ts": datetime.now().isoformat(timespec="seconds"),
            "text": result.text,
            "language": result.language,
            "duration": result.duration,
            "processing_time": result.processing_time,
        }
        with HISTORY_PATH.open("a", encoding="utf-8") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")

    def _detect_gpu_name(self) -> str:
        smi = Path("C:/Windows/System32/nvidia-smi.exe")
        if smi.exists():
            try:
                output = subprocess.check_output(
                    [str(smi), "--query-gpu=name", "--format=csv,noheader"],
                    text=True,
                    stderr=subprocess.STDOUT,
                    timeout=5,
                )
                name = output.splitlines()[0].strip()
                if name:
                    return name
            except Exception as exc:
                self.logger.warning("nvidia-smi GPU name probe failed: %s", exc)
        return "NVIDIA GPU"


def result_to_json(result: TranscriptionResult) -> dict[str, Any]:
    return {
        "text": result.text,
        "language": result.language,
        "duration": result.duration,
        "processing_time": result.processing_time,
        "backend": result.backend,
        "device": result.device,
        "model": result.model,
        "compute_type": result.compute_type,
    }
