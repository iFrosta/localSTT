from __future__ import annotations

import json
import subprocess
import sys
import traceback
from pathlib import Path

from localstt.cuda_paths import candidate_cuda_bin_dirs, configure_cuda_dll_search


def main() -> int:
    print("===== LocalSTT CUDA diagnostics =====")
    print("python:", sys.version)
    print("executable:", sys.executable)
    print("cuda_dll_dirs:", json.dumps([str(p) for p in candidate_cuda_bin_dirs()], ensure_ascii=False, indent=2))
    added = configure_cuda_dll_search()
    print("dll_search_added:", json.dumps(added, ensure_ascii=False, indent=2))

    try:
        import ctranslate2
        from faster_whisper import WhisperModel

        print("CTranslate2 version:", ctranslate2.__version__)
        print("CUDA devices:", ctranslate2.get_cuda_device_count())
        gpu = gpu_name()
        print("CUDA device 0:", gpu)
        print("STT ENGINE: faster-whisper")
        print("MODEL: large-v3-turbo")
        print("DEVICE: cuda")
        print("COMPUTE: float16")
        print("GPU:", gpu)
        print("Creating WhisperModel(device='cuda', compute_type='float16')...")
        model = WhisperModel("large-v3-turbo", device="cuda", compute_type="float16")
        print("WhisperModel created:", type(model).__name__)
        print("Actual compute_type: float16")
        return 0
    except Exception:
        print("CUDA diagnostics failed. CPU fallback is disabled; fix CUDA/cuBLAS/cuDNN before running LocalSTT.")
        traceback.print_exc()
        print_missing_cuda_hints()
        return 1


def gpu_name() -> str:
    smi = Path("C:/Windows/System32/nvidia-smi.exe")
    if smi.exists():
        try:
            return subprocess.check_output(
                [str(smi), "--query-gpu=name", "--format=csv,noheader"],
                text=True,
                stderr=subprocess.STDOUT,
                timeout=5,
            ).splitlines()[0].strip()
        except Exception:
            pass
    return "NVIDIA GPU"


def print_missing_cuda_hints() -> None:
    expected = [
        "nvidia/cublas/bin/cublas64_12.dll",
        "nvidia/cublas/bin/cublasLt64_12.dll",
        "nvidia/cudnn/bin/cudnn64_9.dll",
        "nvidia/cudnn/bin/cudnn_ops64_9.dll",
    ]
    site = Path(sys.prefix) / "Lib" / "site-packages"
    print("Expected CUDA runtime DLLs:")
    for rel in expected:
        path = site / rel
        print(f"  {path}: {'OK' if path.exists() else 'MISSING'}")


if __name__ == "__main__":
    raise SystemExit(main())
