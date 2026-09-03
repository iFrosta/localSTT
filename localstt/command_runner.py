from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

from . import app_index
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


@dataclass
class CommandStatus:
    """Whether a command can run on *this* machine."""

    available: bool
    reason: str = ""


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


# Availability is recomputed when commands.json changes; probes are process-lifetime.
_COMMANDS_CACHE: tuple[tuple[int, int], list[dict[str, Any]]] | None = None
_PROBE_CACHE: dict[str, bool] = {}

_SCRIPT_INTERPRETERS = {".ps1": "pwsh.exe", ".vbs": "wscript.exe"}
_RUNNABLE_SUFFIXES = {".exe", ".cmd", ".bat", ".ps1", ".vbs"}


def clear_availability_cache() -> None:
    """Drop cached probe results so a re-check sees newly installed tools."""
    global _COMMANDS_CACHE
    _COMMANDS_CACHE = None
    _PROBE_CACHE.clear()


def resolve_command_path(raw: str) -> Path | None:
    """Expand %VARS%/~ and fall back to PATH lookup, so commands stay machine-agnostic."""
    text = os.path.expandvars(str(raw).strip())
    if not text:
        return None
    path = Path(text).expanduser()
    if path.is_absolute():
        return path
    found = shutil.which(text)
    return Path(found) if found else None


def _probe(spec: str) -> bool:
    """Evaluate one `requires` entry: path:/exe:/wsl: — anything else is treated as a path."""
    if spec in _PROBE_CACHE:
        return _PROBE_CACHE[spec]

    kind, _, rest = spec.partition(":")
    kind = kind.strip().lower()
    result = False
    try:
        if kind == "exe":
            result = shutil.which(os.path.expandvars(rest.strip())) is not None
        elif kind == "wsl":
            distro, _, command = rest.partition(":")
            result = _probe_wsl(distro.strip(), command.strip())
        elif kind == "appx":
            result = _probe_appx(rest.strip())
        else:
            # `path:C:\...`, or a bare path written without a prefix.
            target = rest if kind == "path" else spec
            resolved = resolve_command_path(target)
            result = resolved is not None and resolved.exists()
    except Exception:
        result = False

    _PROBE_CACHE[spec] = result
    return result


def _probe_wsl(distro: str, command: str) -> bool:
    if not distro or not command or shutil.which("wsl.exe") is None:
        return False
    completed = subprocess.run(
        ["wsl.exe", "-d", distro, "--", "sh", "-lc", f"command -v {command}"],
        capture_output=True,
        timeout=20,
        **_no_window(),
    )
    return completed.returncode == 0


# Store apps live behind an ACL-protected install dir, so the package registry is the
# only cheap way to tell whether one is installed.
_APPX_PACKAGES_KEY = (
    r"SOFTWARE\Classes\Local Settings\Software\Microsoft\Windows"
    r"\CurrentVersion\AppModel\Repository\Packages"
)


def _probe_appx(family_name: str) -> bool:
    """`Claude_pzs8sxrjxfjjc` matches the installed `Claude_1.30.0_x64__pzs8sxrjxfjjc`."""
    if os.name != "nt" or "_" not in family_name:
        return False
    import winreg

    name, _, publisher = family_name.rpartition("_")
    if not name or not publisher:
        return False

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _APPX_PACKAGES_KEY) as key:
            count = winreg.QueryInfoKey(key)[0]
            for index in range(count):
                full_name = winreg.EnumKey(key, index)
                if full_name.startswith(f"{name}_") and full_name.endswith(f"__{publisher}"):
                    return True
    except OSError:
        return False
    return False


def _no_window() -> dict[str, Any]:
    """Keep probe subprocesses from flashing a console window over the user's work."""
    flag = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return {"creationflags": flag} if os.name == "nt" and flag else {}


def command_status(command: dict[str, Any]) -> CommandStatus:
    """Why a command would not run here — a missing tool disables it instead of failing mid-dictation."""
    if command.get("enabled") is False:
        return CommandStatus(False, "disabled in commands.json")

    for spec in command.get("requires", []) or []:
        if not _probe(str(spec)):
            return CommandStatus(False, f"requirement not met: {spec}")

    if str(command.get("type", "")).strip().lower() != "process":
        return CommandStatus(True)

    raw = str(command.get("command", ""))
    executable = resolve_command_path(raw)
    if executable is None:
        return CommandStatus(False, f"path not resolvable: {raw}")
    if not executable.exists():
        return CommandStatus(False, f"not installed: {executable}")

    suffix = executable.suffix.lower()
    if suffix not in _RUNNABLE_SUFFIXES:
        return CommandStatus(False, f"unsupported extension: {suffix or '(none)'}")

    interpreter = _SCRIPT_INTERPRETERS.get(suffix)
    if interpreter and shutil.which(interpreter) is None:
        return CommandStatus(False, f"{interpreter} not installed")

    return CommandStatus(True)


def load_all_commands(logger) -> list[dict[str, Any]]:
    """Every command in commands.json, including the ones this machine cannot run."""
    global _COMMANDS_CACHE
    if not COMMANDS_PATH.exists():
        logger.warning("commands file not found: %s", COMMANDS_PATH)
        return []

    stat = COMMANDS_PATH.stat()
    key = (stat.st_mtime_ns, stat.st_size)
    if _COMMANDS_CACHE is not None and _COMMANDS_CACHE[0] == key:
        return _COMMANDS_CACHE[1]

    try:
        data = json.loads(COMMANDS_PATH.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        logger.error("commands file is unreadable (%s): %s", COMMANDS_PATH, exc)
        return []

    commands = list(data.get("commands", []))
    _PROBE_CACHE.clear()
    _COMMANDS_CACHE = (key, commands)
    return commands


def command_statuses(logger) -> list[tuple[dict[str, Any], CommandStatus]]:
    """Every command paired with its availability, for the settings window and preflight."""
    return [(command, command_status(command)) for command in load_all_commands(logger)]


def _load_commands(logger) -> list[dict[str, Any]]:
    available: list[dict[str, Any]] = []
    skipped: list[str] = []
    for command, status in command_statuses(logger):
        if status.available:
            available.append(command)
        else:
            skipped.append(f"{command.get('name', 'unnamed')} ({status.reason})")
    if skipped:
        logger.info("voice commands unavailable on this machine: %s", ", ".join(skipped))
    return available


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
    if command_type == "app_launch":
        return _execute_app_launch(command, capture, logger)
    if command_type == "microsoft_todo":
        return _queue_microsoft_todo(command, capture, logger)
    if command_type == "localstt":
        if app_handler is None:
            return CommandOutcome(False, "App commands are only available from the tray app", name)
        return app_handler(command, capture)
    return CommandOutcome(False, f"Unsupported command type: {command_type}", name)


def _execute_process(command: dict[str, Any], config: AppConfig, logger) -> CommandOutcome:
    name = str(command.get("name", "process"))
    status = command_status(command)
    if not status.available:
        return CommandOutcome(False, f"Unavailable on this machine: {status.reason}", name)

    executable = resolve_command_path(str(command.get("command", "")))
    assert executable is not None  # command_status already rejected unresolvable paths
    suffix = executable.suffix.lower()
    raw_args = command.get("args", [])
    args = [os.path.expandvars(str(item)) for item in raw_args] if isinstance(raw_args, list) else []

    if suffix == ".ps1":
        cmd = ["pwsh.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(executable), *args]
    elif suffix == ".vbs":
        # CreateProcess cannot start a script file: "%1 is not a valid Win32 application".
        cmd = ["wscript.exe", str(executable), *args]
    else:
        cmd = [str(executable), *args]

    parent = executable.parent
    logger.info("executing voice command %s: %r", name, cmd)
    subprocess.Popen(cmd, cwd=str(parent) if parent.is_dir() else None)
    return CommandOutcome(True, f"Executed: {name}", name)


def _execute_app_launch(command: dict[str, Any], capture: str, logger) -> CommandOutcome:
    """Open whatever installed application the user named, without a commands.json entry."""
    name = str(command.get("name", "app.open"))
    query = capture.strip()
    if not query:
        return CommandOutcome(False, "No application name was heard", name)

    app = app_index.find_app(query, logger)
    if app is None:
        return CommandOutcome(False, f"No installed application matched: {query}", name)

    app_index.launch(app, logger)
    return CommandOutcome(True, f"Opened: {app['name']}", name)


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