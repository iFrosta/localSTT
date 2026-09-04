"""Whether a newer release exists.

Asked of the GitHub releases API and nothing else: one GET, no account, no telemetry,
nothing sent about the machine. GitHub sees the request and therefore the IP address it
came from, which is why this is a setting and not a certainty -- see `update_check_enabled`.
"""

from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

import requests

from . import __version__
from .config import APPDATA_DIR, AppConfig

STATE_PATH = APPDATA_DIR / "update-check.json"
API = "https://api.github.com/repos/{repo}/releases/latest"


@dataclass
class Update:
    version: str
    url: str
    published: str

    @property
    def label(self) -> str:
        return f"LocalSTT {self.version} is available"


def parse_version(text: str) -> tuple[int, ...]:
    """"v1.2.3" -> (1, 2, 3). Anything unparseable sorts as oldest.

    A pre-release suffix is dropped rather than ordered: "1.2.0-rc1" reads as 1.2.0,
    which is close enough for "is there something newer" and avoids inventing a
    precedence rule nobody asked for.
    """
    cleaned = str(text or "").strip().lstrip("vV").split("-")[0].split("+")[0]
    parts: list[int] = []
    for chunk in cleaned.split("."):
        digits = "".join(c for c in chunk if c.isdigit())
        if not digits:
            break
        parts.append(int(digits))
    return tuple(parts)


def is_newer(candidate: str, current: str) -> bool:
    left, right = parse_version(candidate), parse_version(current)
    if not left:
        return False
    # (1, 1) has to beat (1, 0, 5), so compare on equal length.
    width = max(len(left), len(right))
    return left + (0,) * (width - len(left)) > right + (0,) * (width - len(right))


def check(config: AppConfig, logger: logging.Logger, *, timeout: float = 6.0) -> Update | None:
    """The newest published release, or None when there is nothing newer or no answer."""
    repo = (config.update_repository or "").strip()
    if not repo:
        return None
    try:
        response = requests.get(
            API.format(repo=repo),
            timeout=timeout,
            headers={"Accept": "application/vnd.github+json"},
        )
        response.raise_for_status()
        data: dict[str, Any] = response.json()
    except Exception as exc:
        # Being offline is not an error worth showing anyone.
        logger.info("update check failed: %s", exc)
        return None
    finally:
        _remember_check()

    tag = str(data.get("tag_name") or "")
    if not is_newer(tag, __version__):
        logger.info("update check: %s is current (latest published is %s)", __version__, tag or "none")
        return None

    update = Update(
        version=tag.lstrip("vV"),
        url=str(data.get("html_url") or f"https://github.com/{repo}/releases/latest"),
        published=str(data.get("published_at") or ""),
    )
    logger.info("update available: %s (running %s)", update.version, __version__)
    return update


def check_if_due(config: AppConfig, logger: logging.Logger) -> Update | None:
    """The startup check, which does nothing at all unless it is time."""
    if not config.update_check_enabled:
        return None
    if not _is_due(config):
        return None
    return check(config, logger)


def _is_due(config: AppConfig) -> bool:
    hours = max(0.0, config.update_check_interval_hours)
    if hours <= 0:
        return True
    try:
        last = float(json.loads(STATE_PATH.read_text(encoding="utf-8")).get("checked_at", 0))
    except (OSError, ValueError, json.JSONDecodeError, AttributeError):
        return True
    return (time.time() - last) >= hours * 3600


def _remember_check() -> None:
    try:
        APPDATA_DIR.mkdir(parents=True, exist_ok=True)
        STATE_PATH.write_text(
            json.dumps(
                {"checked_at": time.time(), "checked_at_iso": datetime.now(timezone.utc).isoformat()},
                indent=2,
            ),
            encoding="utf-8",
        )
    except OSError:
        # Losing the timestamp only means checking again sooner than necessary.
        pass
