from __future__ import annotations

import argparse
import csv
from pathlib import Path

from localstt.config import load_config
from localstt.logging_setup import setup_logging
from localstt.service import STTService


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark LocalSTT faster-whisper CUDA models.")
    parser.add_argument("wav", nargs="+", help="WAV files to benchmark")
    parser.add_argument("--output", default="benchmark-results.csv", help="CSV output path")
    args = parser.parse_args()

    logger = setup_logging()
    rows = []
    configs = [
        ("large-v3-turbo", 1),
        ("large-v3-turbo", 5),
        ("large-v3", 1),
        ("large-v3", 5),
    ]

    for model, beam in configs:
        cfg = load_config()
        cfg.model = model
        cfg.beam_size = beam
        service = STTService(cfg, logger)
        service.load()
        for wav in args.wav:
            result = service.transcribe(Path(wav), beam_size=beam)
            rtf = result.processing_time / result.duration if result.duration else 0.0
            row = {
                "model": model,
                "beam_size": beam,
                "file": wav,
                "audio_duration": f"{result.duration:.3f}",
                "processing_time": f"{result.processing_time:.3f}",
                "realtime_factor": f"{rtf:.3f}",
                "transcript": result.text,
            }
            rows.append(row)
            print(
                f"{model} beam={beam} file={wav} "
                f"duration={row['audio_duration']}s processing={row['processing_time']}s "
                f"rtf={row['realtime_factor']} text={result.text}"
            )
        del service

    out = Path(args.output)
    with out.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print("Saved:", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
