"""Timings of the last run, so the settings window can show what the machine achieves.

Speed of speech recognition is measured against the length of the audio, not the length
of the text: the real-time factor, processing time over audio duration. Characters per
second would mostly measure how fast the user talks. The cleanup step is a language
model, so there the useful number is tokens per second.
"""

from __future__ import annotations

import json
from datetime import datetime
from typing import Any

from .config import PERFORMANCE_PATH


def _write(section: str, payload: dict[str, Any]) -> None:
    data = load()
    data[section] = payload
    try:
        PERFORMANCE_PATH.parent.mkdir(parents=True, exist_ok=True)
        PERFORMANCE_PATH.write_text(
            json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    except OSError:
        pass  # timings are a nicety; never let them break a dictation


def load() -> dict[str, Any]:
    if not PERFORMANCE_PATH.exists():
        return {}
    try:
        data = json.loads(PERFORMANCE_PATH.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def record_transcription(result: Any, config: Any, session: dict[str, Any] | None = None) -> None:
    duration = float(getattr(result, "duration", 0.0) or 0.0)
    processing = float(getattr(result, "processing_time", 0.0) or 0.0)
    text = getattr(result, "text", "") or ""

    _write(
        "transcription",
        {
            "ts": datetime.now().isoformat(timespec="seconds"),
            "audio_seconds": round(duration, 3),
            "processing_seconds": round(processing, 3),
            "realtime_factor": round(processing / duration, 3) if duration else 0.0,
            "chars": len(text),
            "chars_per_second": round(len(text) / processing, 1) if processing else 0.0,
            "model": getattr(result, "model", config.model),
            "compute_type": getattr(result, "compute_type", config.compute_type),
            "device": getattr(result, "device", config.device),
            "beam_size": config.beam_size,
            "session": session or {},
        },
    )


def record_cleanup(model: str, response: dict[str, Any], seconds: float) -> None:
    """Ollama reports its own token counts, which beat timing the HTTP round trip."""
    tokens = int(response.get("eval_count") or 0)
    eval_ns = int(response.get("eval_duration") or 0)
    generate_seconds = eval_ns / 1e9 if eval_ns else 0.0

    _write(
        "cleanup",
        {
            "ts": datetime.now().isoformat(timespec="seconds"),
            "model": model,
            "tokens": tokens,
            "generate_seconds": round(generate_seconds, 3),
            "tokens_per_second": round(tokens / generate_seconds, 1) if generate_seconds else 0.0,
            "total_seconds": round(seconds, 3),
            "prompt_tokens": int(response.get("prompt_eval_count") or 0),
        },
    )
