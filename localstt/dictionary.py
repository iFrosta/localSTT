from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from .config import DICTIONARY_PATH


@dataclass
class DevelopmentDictionary:
    terms: list[str]
    replacements: dict[str, str]

    @classmethod
    def load(cls, path: Path = DICTIONARY_PATH) -> "DevelopmentDictionary":
        if not path.exists():
            return cls(terms=[], replacements={})
        data = json.loads(path.read_text(encoding="utf-8"))
        return cls(terms=list(data.get("terms", [])), replacements=dict(data.get("replacements", {})))

    def initial_prompt(self) -> str:
        if not self.terms:
            return ""
        return "Технические термины и имена: " + ", ".join(self.terms)

    def apply(self, text: str) -> str:
        result = text
        for phrase, replacement in sorted(self.replacements.items(), key=lambda kv: len(kv[0]), reverse=True):
            pattern = re.compile(rf"(?<![\w-]){re.escape(phrase)}(?![\w-])", flags=re.IGNORECASE | re.UNICODE)
            result = pattern.sub(replacement, result)
        return result
