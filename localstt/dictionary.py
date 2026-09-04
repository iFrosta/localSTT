from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from .config import dictionary_path


# Whisper gives the prompt half of its 448-token text window. Anything past that is
# dropped silently, so a dictionary that keeps growing quietly stops working. Roughly
# three characters per token for mixed Russian and Latin, kept deliberately pessimistic.
PROMPT_CHAR_BUDGET = 600


@dataclass
class DevelopmentDictionary:
    terms: list[str]
    replacements: dict[str, str]
    source: Path | None = None
    stamp: tuple[float, int] | None = None

    @classmethod
    def load(cls, path: Path | None = None) -> "DevelopmentDictionary":
        path = path or dictionary_path()
        stamp = None
        try:
            stat = path.stat()
            stamp = (stat.st_mtime, stat.st_size)
            data = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError):
            return cls(terms=[], replacements={}, source=path, stamp=stamp)
        return cls(
            terms=list(data.get("terms", [])),
            replacements=dict(data.get("replacements", {})),
            source=path,
            stamp=stamp,
        )

    def is_stale(self) -> bool:
        """Whether the file has changed since this was read.

        Checked before every transcription so a new word takes effect on the next
        dictation. Adding one and having to restart the app is not a loop anyone will
        use while they are still working out which words need it.
        """
        path = dictionary_path()
        if path != self.source:
            return True
        try:
            stat = path.stat()
        except OSError:
            return self.stamp is not None
        return self.stamp != (stat.st_mtime, stat.st_size)

    def initial_prompt(self) -> str:
        if not self.terms:
            return ""
        return "Технические термины и имена: " + ", ".join(self.terms)

    def prompt_overflow(self) -> int:
        """Characters past what Whisper will actually read, or 0."""
        return max(0, len(self.initial_prompt()) - PROMPT_CHAR_BUDGET)

    def apply(self, text: str) -> str:
        result = text
        for phrase, replacement in sorted(self.replacements.items(), key=lambda kv: len(kv[0]), reverse=True):
            pattern = re.compile(rf"(?<![\w-]){re.escape(phrase)}(?![\w-])", flags=re.IGNORECASE | re.UNICODE)
            result = pattern.sub(replacement, result)
        return result
