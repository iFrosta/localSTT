from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


APPDATA_DIR = Path(os.environ.get("APPDATA", str(Path.home() / "AppData" / "Roaming"))) / "LocalSTT"
CONFIG_PATH = APPDATA_DIR / "config.json"
LOG_DIR = APPDATA_DIR / "logs"
LOG_PATH = LOG_DIR / "localstt.log"
HISTORY_PATH = APPDATA_DIR / "history.jsonl"
INSTALL_DIR = Path("C:/Apps/LocalSTT")
DICTIONARY_PATH = INSTALL_DIR / "dictionary.json"


@dataclass
class AppConfig:
    model: str = "large-v3-turbo"
    allowed_models: list[str] = field(default_factory=lambda: ["large-v3-turbo", "large-v3", "medium"])
    language: str = "ru"
    allowed_languages: list[str] = field(default_factory=lambda: ["ru", "en", "auto"])
    device: str = "cuda"
    compute_type: str = "float16"
    vad_filter: bool = True
    beam_size: int = 5
    api_host: str = "127.0.0.1"
    api_port: int = 7777
    microphone: int | None = None
    sample_rate: int = 16000
    channels: int = 1
    history_enabled: bool = True
    ollama_base_url: str = "http://127.0.0.1:11434"
    ollama_model: str | None = None
    preferred_ollama_cleanup_models: list[str] = field(
        default_factory=lambda: [
            "qwen3:4b-instruct",
            "qwen3:4b",
            "qwen3:1.7b",
            "qwen2.5:7b-instruct",
            "qwen2.5:3b-instruct",
        ]
    )
    ollama_timeout_seconds: float = 20.0
    paste_restore_delay_seconds: float = 0.8
    hotkey_tap_seconds: float = 0.45


def load_config() -> AppConfig:
    APPDATA_DIR.mkdir(parents=True, exist_ok=True)
    if not CONFIG_PATH.exists():
        cfg = AppConfig()
        save_config(cfg)
        return cfg

    data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    defaults = asdict(AppConfig())
    defaults.update({k: v for k, v in data.items() if k in defaults})
    return AppConfig(**defaults)


def save_config(config: AppConfig) -> None:
    APPDATA_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(asdict(config), ensure_ascii=False, indent=2), encoding="utf-8")


def config_summary(config: AppConfig) -> dict[str, Any]:
    return {
        "model": config.model,
        "language": config.language,
        "allowed_languages": config.allowed_languages,
        "device": config.device,
        "compute_type": config.compute_type,
        "vad_filter": config.vad_filter,
        "beam_size": config.beam_size,
        "api": f"http://{config.api_host}:{config.api_port}",
        "history_enabled": config.history_enabled,
        "ollama_model": config.ollama_model,
        "preferred_ollama_cleanup_models": config.preferred_ollama_cleanup_models,
    }
