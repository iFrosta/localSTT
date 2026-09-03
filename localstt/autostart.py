"""Per-user autostart, as a shortcut in the Startup folder.

Writing a .lnk means the Windows shell link format, so this asks WScript.Shell to do it
rather than pulling in a COM dependency for something toggled once.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Any

PROJECT_DIR = Path(__file__).resolve().parent.parent
LAUNCHER = PROJECT_DIR / "start-localstt.vbs"
SHORTCUT_NAME = "LocalSTT.lnk"


def _no_window() -> dict[str, Any]:
    flag = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return {"creationflags": flag} if os.name == "nt" and flag else {}


def startup_dir() -> Path:
    appdata = os.environ.get("APPDATA", str(Path.home() / "AppData" / "Roaming"))
    return Path(appdata) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup"


def shortcut_path() -> Path:
    return startup_dir() / SHORTCUT_NAME


def is_enabled() -> bool:
    return shortcut_path().exists()


def enable(logger) -> bool:
    if os.name != "nt":
        return False
    if not LAUNCHER.exists():
        logger.warning("cannot enable autostart: %s is missing", LAUNCHER)
        return False

    target = shortcut_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    script = (
        "$s = New-Object -ComObject WScript.Shell; "
        f"$l = $s.CreateShortcut('{target}'); "
        f"$l.TargetPath = '{LAUNCHER}'; "
        f"$l.WorkingDirectory = '{PROJECT_DIR}'; "
        f"$l.IconLocation = '{LAUNCHER},0'; "
        "$l.Save()"
    )
    try:
        completed = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True,
            timeout=30,
            **_no_window(),
        )
    except (OSError, subprocess.SubprocessError) as exc:
        logger.warning("autostart shortcut could not be created: %s", exc)
        return False

    if completed.returncode != 0 or not target.exists():
        logger.warning(
            "autostart shortcut could not be created: %s",
            completed.stderr.decode("utf-8", "replace")[:200],
        )
        return False
    logger.info("autostart enabled: %s", target)
    return True


def disable(logger) -> bool:
    target = shortcut_path()
    try:
        target.unlink(missing_ok=True)
    except OSError as exc:
        logger.warning("autostart shortcut could not be removed: %s", exc)
        return False
    logger.info("autostart disabled")
    return True


def set_enabled(enabled: bool, logger) -> bool:
    return enable(logger) if enabled else disable(logger)
