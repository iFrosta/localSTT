"""Environment self-test.

LocalSTT has no CPU fallback, so a machine that is missing a driver, a CUDA DLL or a
microphone fails at the first dictation with a stack trace in a log file nobody opens.
The self-test front-loads those questions and runs itself the first time LocalSTT starts
on a device it has not seen before.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from .config import APPDATA_DIR, AppConfig

PREFLIGHT_PATH = APPDATA_DIR / "preflight.json"

OK = "ok"
WARN = "warn"
FAIL = "fail"

_STATUS_ORDER = {OK: 0, WARN: 1, FAIL: 2}

# CUDA 12.x wheels need a driver from the 12.0 era or newer; older ones load and then
# fail inside cuBLAS with an unhelpful error.
MIN_DRIVER_VERSION = 527.41

# Peak VRAM in GB, measured with beam_size 5 and a ~30s window. Activations and the
# beam cost roughly as much again as the weights, which is what makes 4 GB cards tight.
MODEL_VRAM_GB: dict[str, dict[str, float]] = {
    "large-v3-turbo": {"float16": 2.4, "int8_float16": 1.5, "int8": 1.2},
    "large-v3": {"float16": 3.2, "int8_float16": 1.9, "int8": 1.6},
    "large-v2": {"float16": 3.2, "int8_float16": 1.9, "int8": 1.6},
    "medium": {"float16": 1.8, "int8_float16": 1.1, "int8": 0.9},
    "small": {"float16": 0.9, "int8_float16": 0.6, "int8": 0.5},
    "base": {"float16": 0.4, "int8_float16": 0.3, "int8": 0.3},
    "tiny": {"float16": 0.3, "int8_float16": 0.2, "int8": 0.2},
}

MODEL_DOWNLOAD_GB = {
    "large-v3-turbo": 1.6,
    "large-v3": 3.1,
    "large-v2": 3.1,
    "medium": 1.5,
    "small": 0.5,
    "base": 0.15,
    "tiny": 0.08,
}

# Fallback only: faster-whisper owns this mapping and moves models between repos
# (large-v3-turbo lives under mobiuslabsgmbh, not Systran), so ask it when it is installed.
HF_REPOS = {
    "large-v3-turbo": "mobiuslabsgmbh/faster-whisper-large-v3-turbo",
    "large-v3": "Systran/faster-whisper-large-v3",
    "large-v2": "Systran/faster-whisper-large-v2",
    "medium": "Systran/faster-whisper-medium",
    "small": "Systran/faster-whisper-small",
    "base": "Systran/faster-whisper-base",
    "tiny": "Systran/faster-whisper-tiny",
}

REQUIRED_CUDA_DLLS = [
    "nvidia/cublas/bin/cublas64_12.dll",
    "nvidia/cublas/bin/cublasLt64_12.dll",
    "nvidia/cudnn/bin/cudnn64_9.dll",
    "nvidia/cudnn/bin/cudnn_ops64_9.dll",
]

REQUIRED_PACKAGES = [
    "ctranslate2",
    "faster_whisper",
    "sounddevice",
    "numpy",
    "pynput",
    "pyperclip",
    "pystray",
    "PIL",
    "fastapi",
    "uvicorn",
    "requests",
    "psutil",
]

# The GPU also has to draw the desktop and hold the driver's own allocations.
GPU_RESERVE_GB = 0.6

# Failing any of these means LocalSTT cannot transcribe at all, so it stops and shows the
# report. A missing microphone or a busy port is worth reporting but not worth refusing
# to start over -- the user may be about to plug one in.
BLOCKING_CHECKS = {
    "platform", "python", "packages", "driver", "gpu", "cuda_runtime", "ctranslate2", "compute_fit",
}


@dataclass
class Check:
    name: str
    title: str
    status: str
    detail: str
    hint: str = ""


@dataclass
class Report:
    status: str = OK
    signature: str = ""
    machine: str = ""
    ran_at: str = ""
    checks: list[Check] = field(default_factory=list)

    @property
    def failures(self) -> list[Check]:
        return [c for c in self.checks if c.status == FAIL]

    @property
    def warnings(self) -> list[Check]:
        return [c for c in self.checks if c.status == WARN]

    @property
    def blocking_failures(self) -> list[Check]:
        return [c for c in self.failures if c.name in BLOCKING_CHECKS]


def _worst(statuses: list[str]) -> str:
    return max(statuses, key=lambda s: _STATUS_ORDER[s], default=OK)


def _gb(value: float) -> str:
    return f"{value:.1f} GB"


# --------------------------------------------------------------------------- probes


def _nvidia_smi() -> Path | None:
    candidate = Path("C:/Windows/System32/nvidia-smi.exe")
    if candidate.exists():
        return candidate
    found = shutil.which("nvidia-smi")
    return Path(found) if found else None


def _query_gpu() -> dict[str, Any] | None:
    """name, driver_version, total/used VRAM in MiB and compute capability."""
    smi = _nvidia_smi()
    if smi is None:
        return None
    try:
        output = subprocess.check_output(
            [
                str(smi),
                "--query-gpu=name,driver_version,memory.total,memory.used,compute_cap",
                "--format=csv,noheader,nounits",
            ],
            text=True,
            stderr=subprocess.STDOUT,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return None

    line = next((l for l in output.splitlines() if l.strip()), "")
    parts = [p.strip() for p in line.split(",")]
    if len(parts) < 5:
        return None
    try:
        return {
            "name": parts[0],
            "driver": parts[1],
            "total_gb": float(parts[2]) / 1024.0,
            "used_gb": float(parts[3]) / 1024.0,
            "compute_cap": float(parts[4]),
        }
    except ValueError:
        return None


def _site_packages() -> Path:
    return Path(sys.prefix) / "Lib" / "site-packages"


def model_vram_gb(model: str, compute_type: str) -> float | None:
    table = MODEL_VRAM_GB.get(model)
    if table is None:
        return None
    if compute_type in table:
        return table[compute_type]
    # int8_bfloat16 and friends land near their int8_float16 cousin.
    if compute_type.startswith("int8"):
        return table.get("int8_float16", table.get("int8"))
    return table.get("float16")


def _hf_cache_root() -> Path:
    for var in ("HUGGINGFACE_HUB_CACHE", "HF_HUB_CACHE"):
        value = os.environ.get(var)
        if value:
            return Path(value)
    home = os.environ.get("HF_HOME")
    if home:
        return Path(home) / "hub"
    return Path.home() / ".cache" / "huggingface" / "hub"


def _model_repo(model: str) -> str | None:
    try:
        from faster_whisper import utils

        repo = getattr(utils, "_MODELS", {}).get(model)
        if repo:
            return str(repo)
    except Exception:
        pass
    return HF_REPOS.get(model)


def _model_is_cached(model: str) -> bool:
    repo = _model_repo(model)
    if repo is None:
        return False
    folder = _hf_cache_root() / ("models--" + repo.replace("/", "--"))
    snapshots = folder / "snapshots"
    return snapshots.is_dir() and any(snapshots.iterdir())


def _port_is_free(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((host, port))
        except OSError:
            return False
    return True


def _listening_connections(proc) -> list:
    # psutil 6 renamed Process.connections() to net_connections().
    query = getattr(proc, "net_connections", None) or proc.connections
    return query(kind="inet")


def _port_holder(port: int):
    """The process listening on the port, or None when Windows will not say."""
    try:
        import psutil
    except ImportError:
        return None

    try:
        own = psutil.Process()
        for conn in _listening_connections(own):
            if conn.laddr and conn.laddr.port == port:
                return own
    except Exception:
        pass

    # Only reached for a port held by someone else; this call needs privileges our own
    # process never does, so it is the fallback rather than the first question.
    try:
        for conn in psutil.net_connections(kind="inet"):
            if conn.laddr and conn.laddr.port == port and conn.pid:
                return psutil.Process(conn.pid)
    except Exception:
        return None
    return None


def _is_localstt(proc) -> bool:
    if proc.pid == os.getpid():
        return True
    try:
        # The interpreter path is skipped on purpose: every stray script started from
        # C:\Apps\LocalSTT would otherwise look like the app itself.
        args = [arg.lower().replace("\\", "/") for arg in proc.cmdline()[1:]]
    except Exception:
        return False
    return any(arg == "localstt.main" or arg.endswith("localstt/main.py") for arg in args)


# --------------------------------------------------------------------------- checks


def check_platform() -> Check:
    if os.name != "nt":
        return Check(
            "platform", "Operating system", FAIL,
            f"{platform.system()} {platform.release()}",
            "LocalSTT drives Windows APIs for text input and hotkeys; it only runs on Windows.",
        )
    return Check("platform", "Operating system", OK, f"Windows {platform.release()} build {platform.version()}")


def check_python() -> Check:
    version = ".".join(str(p) for p in sys.version_info[:3])
    if sys.version_info < (3, 10):
        return Check(
            "python", "Python", FAIL, f"Python {version} at {sys.executable}",
            "Python 3.10 or newer is required; 3.12 is what the project is built against.",
        )
    in_venv = sys.prefix != getattr(sys, "base_prefix", sys.prefix)
    if not in_venv:
        return Check(
            "python", "Python", WARN, f"Python {version}, not running from a virtualenv",
            "Start LocalSTT through .venv\\Scripts\\python.exe so it sees the CUDA wheels.",
        )
    return Check("python", "Python", OK, f"Python {version} in {sys.prefix}")


def check_packages() -> Check:
    import importlib.util

    missing = [name for name in REQUIRED_PACKAGES if importlib.util.find_spec(name) is None]
    if missing:
        return Check(
            "packages", "Python packages", FAIL, f"missing: {', '.join(missing)}",
            "Run: .venv\\Scripts\\python.exe -m pip install -r requirements.txt",
        )
    return Check("packages", "Python packages", OK, f"all {len(REQUIRED_PACKAGES)} imports resolve")


def check_driver(gpu: dict[str, Any] | None) -> Check:
    if gpu is None:
        return Check(
            "driver", "NVIDIA driver", FAIL, "nvidia-smi did not report a GPU",
            "Install the NVIDIA driver. Without CUDA LocalSTT cannot run: CPU fallback is disabled.",
        )
    try:
        version = float(re.match(r"\d+(?:\.\d+)?", gpu["driver"]).group(0))
    except (AttributeError, ValueError):
        return Check("driver", "NVIDIA driver", WARN, f"unrecognised version {gpu['driver']!r}")

    if version < MIN_DRIVER_VERSION:
        return Check(
            "driver", "NVIDIA driver", FAIL, f"driver {gpu['driver']}",
            f"CUDA 12 wheels need driver {MIN_DRIVER_VERSION} or newer. Update the NVIDIA driver.",
        )
    return Check("driver", "NVIDIA driver", OK, f"driver {gpu['driver']}")


def check_gpu(gpu: dict[str, Any] | None) -> Check:
    if gpu is None:
        return Check("gpu", "GPU", FAIL, "no NVIDIA GPU detected")
    detail = f"{gpu['name']}, {_gb(gpu['total_gb'])} VRAM, compute {gpu['compute_cap']:.1f}"
    if gpu["compute_cap"] < 7.0:
        return Check(
            "gpu", "GPU", FAIL, detail,
            "float16 needs compute capability 7.0 (Volta) or newer.",
        )
    return Check("gpu", "GPU", OK, detail)


def check_cuda_runtime() -> Check:
    site = _site_packages()
    missing = [rel for rel in REQUIRED_CUDA_DLLS if not (site / rel).exists()]
    if missing:
        return Check(
            "cuda_runtime", "CUDA runtime DLLs", FAIL,
            f"missing {len(missing)}/{len(REQUIRED_CUDA_DLLS)}: {', '.join(Path(m).name for m in missing)}",
            "Run: .venv\\Scripts\\python.exe -m pip install nvidia-cublas-cu12 nvidia-cudnn-cu12",
        )
    return Check("cuda_runtime", "CUDA runtime DLLs", OK, f"all {len(REQUIRED_CUDA_DLLS)} present in site-packages")


def check_ctranslate2() -> tuple[Check, set[str]]:
    try:
        from .cuda_paths import configure_cuda_dll_search

        configure_cuda_dll_search()
        import ctranslate2
    except Exception as exc:
        return (
            Check(
                "ctranslate2", "CTranslate2 CUDA", FAIL, f"import failed: {exc}",
                "The CUDA runtime wheels are missing or the driver cannot be reached.",
            ),
            set(),
        )

    count = ctranslate2.get_cuda_device_count()
    if count < 1:
        return (
            Check(
                "ctranslate2", "CTranslate2 CUDA", FAIL,
                f"CTranslate2 {ctranslate2.__version__} reports zero CUDA devices",
                "The driver is present but CUDA initialisation failed. A reboot after a driver update usually fixes it.",
            ),
            set(),
        )

    try:
        supported = set(ctranslate2.get_supported_compute_types("cuda"))
    except Exception:
        supported = set()
    detail = f"CTranslate2 {ctranslate2.__version__}, {count} CUDA device(s)"
    if supported:
        detail += f", compute types: {', '.join(sorted(supported))}"
    return Check("ctranslate2", "CTranslate2 CUDA", OK, detail), supported


def check_compute_fit(config: AppConfig, gpu: dict[str, Any] | None, supported: set[str]) -> Check:
    title = "Model fits VRAM"
    if supported and config.compute_type not in supported:
        return Check(
            "compute_fit", title, FAIL,
            f"{config.compute_type} is not supported by this GPU",
            f"Pick one of: {', '.join(sorted(supported))}.",
        )

    needed = model_vram_gb(config.model, config.compute_type)
    if gpu is None or needed is None:
        return Check("compute_fit", title, WARN, f"cannot estimate for {config.model}/{config.compute_type}")

    budget = gpu["total_gb"] - GPU_RESERVE_GB
    detail = f"{config.model} @ {config.compute_type} needs ~{_gb(needed)}, {_gb(budget)} usable"
    if needed > budget:
        lighter = _lighter_option(config.model, budget)
        return Check(
            "compute_fit", title, FAIL, detail,
            f"Switch to {lighter} in the settings." if lighter else "No configured model fits this GPU.",
        )
    if needed > budget * 0.85:
        return Check("compute_fit", title, WARN, detail, "It fits, but leaves little room for anything else on the GPU.")
    return Check("compute_fit", title, OK, detail)


def _lighter_option(model: str, budget_gb: float) -> str | None:
    """The best model/compute pair that still fits, preferring to keep the model."""
    for candidate_model in [model, "large-v3-turbo", "medium", "small"]:
        for compute in ["int8_float16", "int8"]:
            needed = model_vram_gb(candidate_model, compute)
            if needed is not None and needed <= budget_gb:
                return f"{candidate_model} @ {compute}"
    return None


def check_model_cache(config: AppConfig) -> Check:
    title = "Whisper model"
    if _model_is_cached(config.model):
        return Check("model_cache", title, OK, f"{config.model} is already downloaded")

    download = MODEL_DOWNLOAD_GB.get(config.model, 2.0)
    free_gb = shutil.disk_usage(Path(sys.prefix).anchor).free / (1024**3)
    detail = f"{config.model} not cached, first run downloads ~{_gb(download)} ({_gb(free_gb)} free)"
    if free_gb < download * 2:
        return Check("model_cache", title, FAIL, detail, "Free up disk space before the first run.")
    return Check("model_cache", title, WARN, detail, "The first dictation will be slow while the model downloads.")


def check_microphone(config: AppConfig) -> Check:
    title = "Microphone"
    try:
        from .audio import list_microphones

        mics = list_microphones()
    except Exception as exc:
        return Check("microphone", title, FAIL, f"audio device probe failed: {exc}")

    if not mics:
        return Check("microphone", title, FAIL, "no input devices found", "Plug in a microphone and check Windows privacy settings.")

    if config.microphone is not None:
        chosen = next((m for m in mics if m["index"] == config.microphone), None)
        if chosen is None:
            return Check(
                "microphone", title, WARN,
                f"configured device {config.microphone} is gone; {len(mics)} others available",
                "Pick a microphone again in the tray menu.",
            )
        return Check("microphone", title, OK, f"{chosen['name']} (device {chosen['index']})")
    return Check("microphone", title, OK, f"{len(mics)} input device(s), using the Windows default")


def check_api_port(config: AppConfig) -> Check:
    title = "API port"
    address = f"{config.api_host}:{config.api_port}"
    if _port_is_free(config.api_host, config.api_port):
        return Check("api_port", title, OK, f"{address} is free")

    # The self-test also runs from the settings window of a LocalSTT that is already
    # serving on this port, and a warning about ourselves is noise, not a finding.
    holder = _port_holder(config.api_port)
    if holder is not None and _is_localstt(holder):
        who = "this LocalSTT" if holder.pid == os.getpid() else f"a running LocalSTT (PID {holder.pid})"
        return Check("api_port", title, OK, f"{address} is served by {who}")

    if holder is not None:
        try:
            name = holder.name()
        except Exception:
            name = "another process"
        return Check(
            "api_port", title, WARN, f"{address} is already in use by {name} (PID {holder.pid})",
            "Close that process, or change api_port in the settings.",
        )
    return Check(
        "api_port", title, WARN, f"{address} is already in use",
        "Another LocalSTT is probably running. Close it, or change api_port in the settings.",
    )


def check_commands(logger) -> Check:
    title = "Voice commands"
    try:
        from .command_runner import command_statuses

        rows = command_statuses(logger)
    except Exception as exc:
        return Check("commands", title, WARN, f"commands.json could not be read: {exc}")

    if not rows:
        return Check("commands", title, WARN, "no commands defined")

    disabled = [(c.get("name", "unnamed"), s.reason) for c, s in rows if not s.available]
    detail = f"{len(rows) - len(disabled)}/{len(rows)} available on this machine"
    if disabled:
        listed = ", ".join(name for name, _ in disabled[:6])
        return Check(
            "commands", title, WARN, f"{detail}; disabled: {listed}",
            "These commands point at software this machine does not have. They are skipped, not broken.",
        )
    return Check("commands", title, OK, detail)


def _ollama_models(base_url: str) -> list[dict[str, Any]] | None:
    """None means the Ollama service did not answer."""
    url = base_url.rstrip("/") + "/api/tags"
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            payload = json.loads(response.read().decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, json.JSONDecodeError, TimeoutError):
        return None
    return list(payload.get("models", []))


def ollama_models(base_url: str) -> list[dict[str, Any]] | None:
    return _ollama_models(base_url)


def ollama_vram_gb(model: dict[str, Any]) -> float:
    """Weights plus the KV cache Ollama allocates alongside them."""
    size_gb = float(model.get("size", 0)) / (1024**3)
    return size_gb * 1.15 + 0.35


# Registry tags with roughly the VRAM they occupy at their default quantisation. Used
# only to suggest something to download when nothing installed fits.
CLEANUP_PULL_CANDIDATES: list[tuple[str, float]] = [
    ("qwen2.5:7b-instruct", 5.5),
    ("qwen3.5:2b-q4_K_M", 2.4),
    ("qwen2.5:3b-instruct", 2.4),
    ("qwen3:1.7b", 1.8),
    ("qwen2.5:1.5b-instruct", 1.5),
    ("qwen3.5:0.8b", 1.4),
    ("qwen2.5:0.5b-instruct", 0.8),
]


def is_cleanup_capable(model: dict[str, Any]) -> bool:
    """An embedding model cannot rewrite text and a cloud model is not local.

    ollama_cleanup has a stricter version of this, but it imports requests, and the
    self-test has to run on a machine where nothing is installed yet.
    """
    name = str(model.get("name") or model.get("model") or "").lower()
    if model.get("remote_host") or model.get("remote_model") or name.endswith(":cloud"):
        return False
    capabilities = set(model.get("capabilities") or [])
    if capabilities and capabilities <= {"embedding"}:
        return False
    return "embed" not in name


@dataclass
class CleanupChoice:
    """What this machine can actually run for cleanup dictation."""

    reachable: bool
    budget_gb: float
    installed: list[tuple[str, float]] = field(default_factory=list)
    best_installed: str | None = None
    best_installed_gb: float = 0.0
    pull: str | None = None
    pull_gb: float = 0.0
    note: str = ""


def cleanup_budget_gb(config: AppConfig, gpu: dict[str, Any] | None) -> float:
    """VRAM left for Ollama once Whisper is resident."""
    if gpu is None:
        return 0.0
    whisper_gb = model_vram_gb(config.model, config.compute_type) or 0.0
    return max(0.0, gpu["total_gb"] - whisper_gb - GPU_RESERVE_GB)


def recommend_cleanup_model(config: AppConfig, gpu: dict[str, Any] | None = None) -> CleanupChoice:
    """Prefer something already installed that fits; only suggest a download otherwise."""
    if gpu is None:
        gpu = _query_gpu()
    budget = cleanup_budget_gb(config, gpu)

    models = _ollama_models(config.ollama_base_url)
    if models is None:
        return CleanupChoice(
            reachable=False, budget_gb=budget,
            note=f"Ollama did not answer on {config.ollama_base_url}.",
        )

    installed = sorted(
        (
            (str(m.get("name", "")), ollama_vram_gb(m))
            for m in models
            if m.get("name") and is_cleanup_capable(m)
        ),
        key=lambda item: item[1],
        reverse=True,
    )
    fitting = [item for item in installed if item[1] <= budget]

    if fitting:
        name, needed = fitting[0]
        return CleanupChoice(
            reachable=True, budget_gb=budget, installed=installed,
            best_installed=name, best_installed_gb=needed,
            note=f"{name} fits in the {_gb(budget)} left after Whisper.",
        )

    candidates = [c for c in CLEANUP_PULL_CANDIDATES if c[1] <= budget]
    if candidates:
        name, needed = candidates[0]
        note = f"Nothing installed fits in {_gb(budget)}. {name} would."
    else:
        # Even the smallest spills; say so rather than pretending otherwise.
        name, needed = CLEANUP_PULL_CANDIDATES[-1]
        note = (
            f"Only {_gb(budget)} is free after Whisper, so any cleanup model will spill "
            f"onto the CPU. {name} is the least bad, or use a smaller Whisper model."
        )
    return CleanupChoice(
        reachable=True, budget_gb=budget, installed=installed,
        pull=name, pull_gb=needed, note=note,
    )


def check_ollama(config: AppConfig, gpu: dict[str, Any] | None) -> Check:
    title = "Ollama cleanup model"
    models = _ollama_models(config.ollama_base_url)
    if models is None:
        return Check(
            "ollama", title, WARN, f"no answer from {config.ollama_base_url}",
            "Cleanup dictation (Ctrl+Shift+Win) needs Ollama running. Plain dictation works without it.",
        )
    if not models:
        return Check(
            "ollama", title, WARN, "Ollama is running but has no models",
            "Run: ollama pull qwen3.5:2b-q4_K_M",
        )

    whisper_gb = model_vram_gb(config.model, config.compute_type) or 0.0
    budget = (gpu["total_gb"] - whisper_gb - GPU_RESERVE_GB) if gpu else 0.0

    by_name = {str(m.get("name", "")): m for m in models}
    fitting = sorted(
        (m for m in models if is_cleanup_capable(m) and ollama_vram_gb(m) <= budget),
        key=ollama_vram_gb,
        reverse=True,
    )
    best = str(fitting[0].get("name")) if fitting else None

    chosen = config.ollama_model
    if not chosen:
        hint = f"Set ollama_model to {best}." if best else "Run: ollama pull qwen3.5:0.8b"
        return Check(
            "ollama", title, WARN,
            f"no model pinned; {len(models)} installed, {_gb(budget)} VRAM free after Whisper",
            hint + " Otherwise LocalSTT may auto-pick a model too large for this GPU.",
        )

    model = by_name.get(chosen)
    if model is None:
        return Check(
            "ollama", title, WARN, f"configured model {chosen!r} is not installed",
            f"Run: ollama pull {chosen}" + (f" -- or switch to {best}, which fits." if best else ""),
        )

    needed = ollama_vram_gb(model)
    detail = f"{chosen} needs ~{_gb(needed)}, {_gb(budget)} free alongside Whisper"
    if gpu is None:
        return Check("ollama", title, WARN, f"{chosen} installed; VRAM budget unknown")
    if needed > budget:
        hint = (
            f"It will spill onto the CPU and be slow. {best} fits instead."
            if best
            else "It will spill onto the CPU and be slow. Pull a smaller model, e.g. qwen3.5:0.8b."
        )
        return Check("ollama", title, WARN, detail, hint)
    return Check("ollama", title, OK, detail)


# --------------------------------------------------------------------------- runner


def machine_signature() -> str:
    """Identifies the device+interpreter pair, so a new laptop or venv re-runs the test."""
    parts = [
        platform.node(),
        platform.machine(),
        f"{sys.version_info.major}.{sys.version_info.minor}",
        sys.prefix,
    ]
    return hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()[:16]


def run(config: AppConfig, logger) -> Report:
    gpu = _query_gpu()
    ctranslate_check, supported = check_ctranslate2()

    checks = [
        check_platform(),
        check_python(),
        check_packages(),
        check_driver(gpu),
        check_gpu(gpu),
        check_cuda_runtime(),
        ctranslate_check,
        check_compute_fit(config, gpu, supported),
        check_model_cache(config),
        check_microphone(config),
        check_api_port(config),
        check_commands(logger),
        check_ollama(config, gpu),
    ]

    report = Report(
        status=_worst([c.status for c in checks]),
        signature=machine_signature(),
        machine=platform.node(),
        ran_at=datetime.now().isoformat(timespec="seconds"),
        checks=checks,
    )
    for check in checks:
        logger.info("preflight %-14s %-4s %s", check.name, check.status, check.detail)
    return report


def save(report: Report) -> None:
    PREFLIGHT_PATH.parent.mkdir(parents=True, exist_ok=True)
    PREFLIGHT_PATH.write_text(json.dumps(asdict(report), ensure_ascii=False, indent=2), encoding="utf-8")


def load() -> Report | None:
    if not PREFLIGHT_PATH.exists():
        return None
    try:
        data = json.loads(PREFLIGHT_PATH.read_text(encoding="utf-8-sig"))
        checks = [Check(**c) for c in data.get("checks", [])]
        return Report(
            status=str(data.get("status", OK)),
            signature=str(data.get("signature", "")),
            machine=str(data.get("machine", "")),
            ran_at=str(data.get("ran_at", "")),
            checks=checks,
        )
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        return None


def should_run() -> bool:
    """Run on a device we have not tested, and keep running until the device passes."""
    previous = load()
    if previous is None:
        return True
    if previous.signature != machine_signature():
        return True
    # Re-test until the device can actually run; advisory failures should not make every
    # launch pay for a self-test.
    return bool(previous.blocking_failures)


_MARKS = {OK: "[ ok ]", WARN: "[warn]", FAIL: "[FAIL]"}


def format_report(report: Report) -> str:
    lines = [
        "LocalSTT self-test",
        f"device {report.machine}  {report.ran_at}  ->  {report.status.upper()}",
        "",
    ]
    for check in report.checks:
        lines.append(f"{_MARKS[check.status]} {check.title}: {check.detail}")
        if check.hint and check.status != OK:
            lines.append(f"        {check.hint}")
    return "\n".join(lines)


def main() -> int:
    import logging

    from .config import load_config

    logging.basicConfig(level=logging.WARNING, format="%(message)s")
    report = run(load_config(), logging.getLogger("localstt.preflight"))
    save(report)
    print(format_report(report))
    return 1 if report.blocking_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
