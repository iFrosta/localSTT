"""Pick, download and configure the Ollama model behind cleanup dictation.

The right model is a property of the machine, not a constant: it has to fit in the VRAM
left after Whisper is resident. This asks the self-test what fits here instead of
hard-coding a name that is wrong on a 4 GB card.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from . import preflight
from .config import load_config, save_config


def _no_window() -> dict[str, Any]:
    flag = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return {"creationflags": flag} if os.name == "nt" and flag else {}


def ollama_executable() -> str | None:
    local = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Ollama" / "ollama.exe"
    if local.exists():
        return str(local)
    return shutil.which("ollama")


def pull(model: str, logger) -> bool:
    """Download a model. Minutes on a slow link, so callers should run it off the UI."""
    executable = ollama_executable()
    if executable is None:
        logger.warning("ollama executable not found; cannot pull %s", model)
        return False

    logger.info("pulling Ollama model %s", model)
    try:
        completed = subprocess.run(
            [executable, "pull", model], capture_output=True, timeout=3600, **_no_window()
        )
    except (OSError, subprocess.SubprocessError) as exc:
        logger.warning("ollama pull %s failed: %s", model, exc)
        return False

    if completed.returncode != 0:
        logger.warning(
            "ollama pull %s failed: %s", model, completed.stderr.decode("utf-8", "replace")[:300]
        )
        return False
    logger.info("pulled Ollama model %s", model)
    return True


def main() -> int:
    import logging

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", help="use this model instead of the recommended one")
    parser.add_argument("--pull", action="store_true", help="download the model if it is missing")
    parser.add_argument("--apply", action="store_true", help="write the choice into config.json")
    parser.add_argument("--timeout", type=float, help="ollama_timeout_seconds to store")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    logger = logging.getLogger("localstt.cleanup_model")

    config = load_config()
    choice = preflight.recommend_cleanup_model(config)

    print(f"VRAM free for cleanup after Whisper: {choice.budget_gb:.1f} GB")
    if not choice.reachable:
        print(choice.note)
        print("Start Ollama and run this again.")
        return 1

    if choice.installed:
        print("Installed models:")
        for name, needed in choice.installed:
            fits = "fits" if needed <= choice.budget_gb else "spills to CPU"
            marker = "*" if name == config.ollama_model else " "
            print(f"  {marker} {name:<28} ~{needed:.1f} GB  ({fits})")
    print(choice.note)

    target = args.model or choice.best_installed or choice.pull
    if target is None:
        print("No cleanup model could be chosen.")
        return 1

    installed_names = {name for name, _ in choice.installed}
    if target not in installed_names:
        if not args.pull:
            print(f"\n{target} is not installed. Re-run with --pull to download it.")
            return 1
        if not pull(target, logger):
            return 1

    if args.apply:
        config.ollama_model = target
        if args.timeout is not None:
            config.ollama_timeout_seconds = args.timeout
        save_config(config)
        print(f"\nConfigured cleanup model: {target}")
        print("Restart LocalSTT to use it.")
    else:
        print(f"\nRecommended cleanup model: {target}")
        print("Re-run with --apply to write it into config.json.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
