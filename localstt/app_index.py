"""Launch any installed application by the name the user speaks.

Windows keeps every Start-menu-visible app -- desktop shortcuts, Store packages and
registered app ids alike -- in the AppsFolder shell namespace, each addressed by an
AppUserModelID. `Get-StartApps` enumerates that folder, and `explorer.exe
shell:AppsFolder\\<id>` launches any entry in it, so one index and one launcher cover
everything the user can start from the Start menu.
"""

from __future__ import annotations

import json
import os
import subprocess
import time
from difflib import SequenceMatcher
from pathlib import Path
from typing import Any

from .config import APPDATA_DIR

APP_INDEX_PATH = APPDATA_DIR / "app-index.json"
APP_ALIASES_PATH = APPDATA_DIR / "app-aliases.json"

# The Start menu changes when software is installed, which is rare enough that a daily
# rebuild is plenty; the settings window can force one.
INDEX_TTL_SECONDS = 24 * 60 * 60

# Below this the match is more likely to open the wrong app than the right one.
MATCH_THRESHOLD = 0.72

_MEMORY_INDEX: list[dict[str, str]] | None = None

# Whisper transcribes Russian speech in Cyrillic, so the spoken name of a Latin-named
# app never matches on its own. Seeded on first run and editable by the user.
DEFAULT_ALIASES = {
    "калькулятор": "Calculator",
    "блокнот": "Notepad",
    "проводник": "File Explorer",
    "параметры": "Settings",
    "настройки": "Settings",
    "диспетчер задач": "Task Manager",
    "панель управления": "Control Panel",
    "командная строка": "Command Prompt",
    "терминал": "Terminal",
    "часы": "Clock",
    "камера": "Camera",
    "почта": "Mail",
    "календарь": "Calendar",
    "фотографии": "Photos",
    "магазин": "Microsoft Store",
    "клод": "Claude",
    "кодекс": "ChatGPT",
    "курсор": "Cursor",
    "обсидиан": "Obsidian",
    "телеграм": "Telegram",
    "хром": "Google Chrome",
    "докер": "Docker Desktop",
    "вс код": "Visual Studio Code",
    "антигравити": "Antigravity",
}

_TRANSLIT = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ж": "zh",
    "з": "z", "и": "i", "й": "y", "к": "k", "л": "l", "м": "m", "н": "n",
    "о": "o", "п": "p", "р": "r", "с": "s", "т": "t", "у": "u", "ф": "f",
    "х": "h", "ц": "c", "ч": "ch", "ш": "sh", "щ": "sch", "ъ": "", "ы": "y",
    "ь": "", "э": "e", "ю": "yu", "я": "ya",
}


def _no_window() -> dict[str, Any]:
    flag = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return {"creationflags": flag} if os.name == "nt" and flag else {}


def normalize(text: str) -> str:
    text = str(text).lower().replace("ё", "е")
    kept = [ch if (ch.isalnum() or ch.isspace()) else " " for ch in text]
    return " ".join("".join(kept).split())


def transliterate(text: str) -> str:
    return "".join(_TRANSLIT.get(ch, ch) for ch in text)


def load_aliases() -> dict[str, str]:
    if not APP_ALIASES_PATH.exists():
        save_aliases(DEFAULT_ALIASES)
        return dict(DEFAULT_ALIASES)
    try:
        data = json.loads(APP_ALIASES_PATH.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        return dict(DEFAULT_ALIASES)
    return {str(k): str(v) for k, v in data.items()} if isinstance(data, dict) else {}


def save_aliases(aliases: dict[str, str]) -> None:
    APP_ALIASES_PATH.parent.mkdir(parents=True, exist_ok=True)
    APP_ALIASES_PATH.write_text(
        json.dumps(aliases, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8"
    )


def build_index(logger) -> list[dict[str, str]]:
    """Ask Windows for every app the Start menu can launch."""
    if os.name != "nt":
        return []
    script = (
        "[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
        "Get-StartApps | Select-Object Name,AppID | ConvertTo-Json -Compress"
    )
    try:
        completed = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True,
            timeout=60,
            **_no_window(),
        )
    except (OSError, subprocess.SubprocessError) as exc:
        logger.warning("app index build failed: %s", exc)
        return []

    if completed.returncode != 0:
        logger.warning("Get-StartApps failed: %s", completed.stderr.decode("utf-8", "replace")[:200])
        return []

    try:
        raw = json.loads(completed.stdout.decode("utf-8", "replace") or "[]")
    except json.JSONDecodeError as exc:
        logger.warning("app index is not valid JSON: %s", exc)
        return []

    if isinstance(raw, dict):
        raw = [raw]
    apps = [
        {"name": str(item.get("Name", "")).strip(), "appid": str(item.get("AppID", "")).strip()}
        for item in raw
        if isinstance(item, dict)
    ]
    apps = [a for a in apps if a["name"] and a["appid"]]
    logger.info("indexed %s installed applications", len(apps))
    return apps


def save_index(apps: list[dict[str, str]]) -> None:
    APP_INDEX_PATH.parent.mkdir(parents=True, exist_ok=True)
    payload = {"ts": time.time(), "apps": apps}
    APP_INDEX_PATH.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def load_index(logger, *, refresh: bool = False) -> list[dict[str, str]]:
    global _MEMORY_INDEX
    if _MEMORY_INDEX is not None and not refresh:
        return _MEMORY_INDEX

    cached: list[dict[str, str]] = []
    fresh = False
    if APP_INDEX_PATH.exists() and not refresh:
        try:
            payload = json.loads(APP_INDEX_PATH.read_text(encoding="utf-8-sig"))
            cached = list(payload.get("apps", []))
            fresh = (time.time() - float(payload.get("ts", 0))) < INDEX_TTL_SECONDS
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            cached, fresh = [], False

    if fresh and cached:
        _MEMORY_INDEX = cached
        return cached

    apps = build_index(logger)
    if not apps:
        # A failed rebuild should not throw away a usable stale index.
        _MEMORY_INDEX = cached
        return cached

    save_index(apps)
    _MEMORY_INDEX = apps
    return apps


def _score(query: str, name: str) -> float:
    if not query or not name:
        return 0.0
    if query == name:
        return 1.0
    if name.startswith(query):
        return 0.92
    query_tokens, name_tokens = set(query.split()), set(name.split())
    if query_tokens and query_tokens <= name_tokens:
        return 0.88

    best = SequenceMatcher(None, query, name).ratio()
    latin = transliterate(query)
    if latin != query:
        if name.startswith(latin):
            return max(best, 0.9)
        best = max(best, SequenceMatcher(None, latin, name).ratio())
    return best


def find_app(query: str, logger) -> dict[str, str] | None:
    """Resolve a spoken application name, trimming trailing chatter if needed."""
    apps = load_index(logger)
    if not apps:
        return None

    aliases = {normalize(k): v for k, v in load_aliases().items()}
    tokens = normalize(query).split()
    if not tokens:
        return None

    # "открой калькулятор пожалуйста" -- try the whole capture first, then shorter ones.
    for end in range(len(tokens), 0, -1):
        candidate = " ".join(tokens[:end])
        if len(candidate) < 2:
            continue

        target = aliases.get(candidate)
        if target:
            for app in apps:
                if normalize(app["name"]) == normalize(target):
                    return app

        best, best_score = None, 0.0
        for app in apps:
            score = _score(candidate, normalize(app["name"]))
            if score > best_score:
                best, best_score = app, score
        if best is not None and best_score >= MATCH_THRESHOLD:
            logger.info("app match %r -> %r (score %.2f)", candidate, best["name"], best_score)
            return best
    return None


def launch(app: dict[str, str], logger) -> None:
    target = f"shell:AppsFolder\\{app['appid']}"
    logger.info("launching app %s via %s", app["name"], target)
    subprocess.Popen(["explorer.exe", target])
