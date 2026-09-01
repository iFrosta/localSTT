from __future__ import annotations

import os
import site
import sys
from pathlib import Path


def candidate_cuda_bin_dirs() -> list[Path]:
    roots: list[Path] = [Path(sys.prefix) / "Lib" / "site-packages" / "nvidia"]
    roots.extend(Path(p) / "nvidia" for p in site.getsitepackages())

    dirs: list[Path] = []
    for root in roots:
        dirs.extend(
            [
                root / "cublas" / "bin",
                root / "cudnn" / "bin",
                root / "cuda_nvrtc" / "bin",
            ]
        )

    cuda_path = os.environ.get("CUDA_PATH")
    if cuda_path:
        dirs.append(Path(cuda_path) / "bin")
        dirs.append(Path(cuda_path) / "bin" / "x64")

    seen: set[str] = set()
    existing: list[Path] = []
    for item in dirs:
        key = str(item).lower()
        if key not in seen and item.exists():
            seen.add(key)
            existing.append(item)
    return existing


def configure_cuda_dll_search() -> list[str]:
    added: list[str] = []
    if os.name != "nt":
        return added

    for path in candidate_cuda_bin_dirs():
        text = str(path)
        try:
            os.add_dll_directory(text)
            added.append(text)
        except (FileNotFoundError, OSError):
            continue

    if added:
        current = os.environ.get("PATH", "")
        os.environ["PATH"] = ";".join(added + [current])
    return added
