from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

from .config import APPDATA_DIR, COMMANDS_PATH, AppConfig


# Handles commands of type "localstt", which act on the running app itself.
AppCommandHandler = Callable[[dict[str, Any], str], "CommandOutcome"]


COMMAND_HISTORY_PATH = APPDATA_DIR / "command-history.jsonl"
TODO_QUEUE_PATH = APPDATA_DIR / "todo-queue.jsonl"


@dataclass
class CommandOutcome:
    executed: bool
    message: str
    command_name: str | None = None


def match_voice_command(text: str, config: AppConfig, logger) -> tuple[dict[str, Any], str] | None:
    """Return (command, capture) for the first allowlisted command matching the text."""
    if not config.commands_enabled or not text.strip():
        return None

    for command in _load_commands(logger):
        capture = _match_any(text, command.get("patterns", []))
        if capture is not None:
            return command, capture
    return None


def run_matched_command(
    text: str,
    command: dict[str, Any],
    capture: str,
    config: AppConfig,
    logger,
    app_handler: AppCommandHandler | None = None,
) -> CommandOutcome:
    outcome = _execute_command(command, capture, config, logger, app_handler)
    _record_command(text, command, outcome)
    return outcome


def execute_voice_command(
    text: str,
    config: AppConfig,
    logger,
    app_handler: AppCommandHandler | None = None,
) -> CommandOutcome:
    if not config.commands_enabled:
        return CommandOutcome(False, "Voice commands are disabled")

    match = match_voice_command(text, config, logger)
    if match is not None:
        command, capture = match
        return run_matched_command(text, command, capture, config, logger, app_handler)

    outcome = CommandOutcome(False, "No command matched")
    _record_command(text, None, outcome)
    return outcome


def _load_commands(logger) -> list[dict[str, Any]]:
    if not COMMANDS_PATH.exists():
        logger.warning("commands file not found: %s", COMMANDS_PATH)
        return []
    data = json.loads(COMMANDS_PATH.read_text(encoding="utf-8-sig"))
    return list(data.get("commands", []))


def _execute_command(
    command: dict[str, Any],
    capture: str,
    config: AppConfig,
    logger,
    app_handler: AppCommandHandler | None = None,
) -> CommandOutcome:
    command_type = str(command.get("type", "")).strip().lower()
    name = str(command.get("name", "unnamed"))

    if command_type == "process":
        return _execute_process(command, config, logger)
    if command_type == "microsoft_todo":
        return _queue_microsoft_todo(command, capture, logger)
    if command_type == "localstt":
        if app_handler is None:
            return CommandOutcome(False, "App commands are only available from the tray app", name)
        return app_handler(command, capture)
    return CommandOutcome(False, f"Unsupported command type: {command_type}", name)


def _execute_process(command: dict[str, Any], config: AppConfig, logger) -> CommandOutcome:
    name = str(command.get("name", "process"))
    executable = Path(str(command.get("command", "")))
    if not executable.is_absolute():
        return CommandOutcome(False, f"Rejected non-absolute command path: {executable}", name)
    if not executable.exists():
        return CommandOutcome(False, f"Command file not found: {executable}", name)

    suffix = executable.suffix.lower()
    raw_args = command.get("args", [])
    args = [str(item) for item in raw_args] if isinstance(raw_args, list) else []

    if suffix == ".ps1":
        cmd = ["pwsh.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(executable), *args]
    elif suffix == ".vbs":
        # CreateProcess cannot start a script file: "%1 is not a valid Win32 application".
        cmd = ["wscript.exe", str(executable), *args]
    elif suffix in {".exe", ".cmd", ".bat"}:
        cmd = [str(executable), *args]
    else:
        return CommandOutcome(False, f"Rejected unsupported command extension: {suffix}", name)

    logger.info("executing voice command %s: %r", name, cmd)
    subprocess.Popen(cmd, cwd=str(executable.parent))
    return CommandOutcome(True, f"Executed: {name}", name)


def _queue_microsoft_todo(command: dict[str, Any], capture: str, logger) -> CommandOutcome:
    name = str(command.get("name", "todo.create"))
    title = capture.strip(" .,;:!?\"'")
    if not title:
        return CommandOutcome(False, "Todo title was empty", name)

    TODO_QUEUE_PATH.parent.mkdir(parents=True, exist_ok=True)
    row = {
        "ts": datetime.now().isoformat(timespec="seconds"),
        "title": title,
        "list": command.get("list", "Tasks"),
        "status": "queued_locally",
    }
    with TODO_QUEUE_PATH.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
    logger.info("queued Microsoft To Do task locally: %s", title)
    return CommandOutcome(True, f"Todo queued locally: {title}", name)


def _match_any(text: str, patterns: list[str]) -> str | None:
    normalized = _normalize(text)
    for pattern in patterns:
        capture = _match_pattern(normalized, _normalize(str(pattern)))
        if capture is not None:
            return capture
    return None


def _match_pattern(text: str, pattern: str) -> str | None:
    """A command has to open the phrase, so it cannot fire from mid-sentence chatter.

    `phrase`   matches "phrase" and "phrase, please", not "well I said phrase".
    `phrase *` matches the rest of the sentence and returns it as the capture.
    `*phrase`  opts back in to matching anywhere in the sentence.
    """
    if "*" not in pattern:
        return "" if _phrase_at_start(text, pattern) else None

    if pattern.startswith("*") and "*" not in pattern[1:]:
        anywhere = re.escape(pattern[1:].strip())
        return "" if re.search(rf"(?:^|\W){anywhere}(?:\W|$)", text) else None

    if pattern.endswith("*") and "*" not in pattern[:-1]:
        prefix = re.escape(pattern[:-1].strip())
        match = re.match(rf"{prefix}\s+(?P<capture>.+?)\s*$", text)
        return match.group("capture").strip() if match else None

    parts = pattern.split("*")
    if len(parts) != 2:
        return None
    head, tail = re.escape(parts[0].strip()), re.escape(parts[1].strip())
    match = re.match(rf"{head}\s+(?P<capture>.+?)\s+{tail}\W*$", text)
    return match.group("capture").strip() if match else None


def _phrase_at_start(text: str, phrase: str) -> bool:
    return re.match(rf"{re.escape(phrase)}(?:\W|$)", text) is not None


def _normalize(text: str) -> str:
    text = text.lower().replace("ё", "е")
    text = text.replace("black out", "blackout")
    text = re.sub(r"[^\w\s*.-]+", " ", text, flags=re.UNICODE)
    return re.sub(r"\s+", " ", text).strip()


def _record_command(text: str, command: dict[str, Any] | None, outcome: CommandOutcome) -> None:
    COMMAND_HISTORY_PATH.parent.mkdir(parents=True, exist_ok=True)
    row = {
        "ts": datetime.now().isoformat(timespec="seconds"),
        "text": text,
        "matched": command.get("name") if command else None,
        "executed": outcome.executed,
        "message": outcome.message,
    }
    with COMMAND_HISTORY_PATH.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")