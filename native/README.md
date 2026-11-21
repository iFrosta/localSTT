# Native build and verification

Requirements for building: Windows x64, Visual Studio 2022 Build Tools with the C++
workload and Windows SDK, CMake. Building and running never require Python.

```powershell
.\native\setup.ps1
.\native\build.ps1
```

Setup obtains pinned, hash-checked CTranslate2 headers/native DLLs, CUDA libraries,
the model and native Silero VAD files. Python wheels are used only as ZIP containers;
setup does not run Python or install packages. Build creates `LocalSTT.exe` (tray, no console)
and `localstt-cli.exe` in `native-install`, runs the native regression tests and
bundles runtime files. For different models see `Get-Help .\native\setup.ps1 -Full`.
The NVIDIA display driver is required on the destination computer. MSVC runtime DLLs
are bundled locally; the CUDA Toolkit and C++ build tools are not runtime dependencies. Keep the bundled files
and their license notices together. Downloaded binaries/models are excluded from Git.

For manual builds with dependencies already present:

```powershell
cmake -S .\native -B .\build-native -A x64
cmake --build .\build-native --config Release --parallel 4
ctest --test-dir .\build-native -C Release --output-on-failure
```

The Release executables are written to `LOCALSTT_OUTPUT_DIR`, `native-install` by
default; pass `-DLOCALSTT_OUTPUT_DIR=...` to build elsewhere while that copy is running.
The version is read from `localstt/__init__.py` at configure time into a generated
`version.h`, so the native and Python applications always report the same one.

`native-install` is the portable application folder; models may be large. Copy
`commands.json`, `dictionary.json` and `cleanup-prompt.txt` with the application or pass
`--root` to the original project; an edited copy in `%APPDATA%\LocalSTT` wins over both. Model-path overrides live in `native_ct2_model_path`,
`native_model_path` (optional whisper.cpp backend), and `native_vad_path`.
The engine verifies the actual CUDA backend and rejects silent
CPU fallback. Explicit `device=cpu` is supported for troubleshooting.

## Tests

CTest covers stable-prefix live output, final flush/cancellation, Unicode command
matching, allowlist tampering/cancellation, and real Media Foundation decoding and
resampling. It also covers hotkey naming, chord matching and conflicts; the update
check's version comparison, release parsing and shared `update-check.json`; and the
dictionary, cleanup prompt and the user-or-bundled lookup of editable files (the
`dictionary` test also checks that the built-in cleanup prompt still matches
`cleanup-prompt.txt`). It also checks log-mel extraction and tokenizer references against the
original implementation. Audio tests include Unicode filenames and malformed input.

The optional `test_localstt_ct2` target verifies real GPU decoding, word alignment,
silence, in-flight cancellation and reuse. `tests/Test-Ct2.ps1` runs it with the
public whisper.cpp JFK sample; `-Live` adds a live-output regression.

The runtime smoke test uses a supplied speech WAV, identifies the listening process,
checks its modules/process tree for Python or `.venv`, and requests a real CUDA
transcription through the compatible HTTP API:

```powershell
.\native\tests\Test-NativeRuntime.ps1 -InstallDir .\native-install -Wav C:\Audio\speech.wav
```

The test must target a running native server, not the previous Python service.
Use a second port and `--state-dir` when testing alongside the old application.
Hardware microphone, hotkeys and delivery should also be checked in the applications
you dictate into; Windows input permissions and individual editor behavior vary.

The original Python implementation remains available for rollback through
`run-python-legacy.ps1`. Stop the native app before starting it. No migration script
deletes the Python environment or your model caches.
