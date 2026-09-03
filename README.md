# LocalSTT

Local Windows faster-whisper dictation and OpenAI-compatible STT API.

- Install dir: `C:\Apps\LocalSTT`
- Venv: `C:\Apps\LocalSTT.venv`
- Config: `%APPDATA%\LocalSTT\config.json`
- Logs: `%APPDATA%\LocalSTT\logs\localstt.log`
- API: `http://127.0.0.1:7777`

Run CUDA diagnostics:

```powershell
C:\Apps\LocalSTT\run-diagnostics.ps1
```

Run development app with console:

```powershell
C:\Apps\LocalSTT\run-dev.ps1
```

Run normal tray app without console:

```powershell
wscript.exe C:\Apps\LocalSTT\start-localstt.vbs
```

Hotkeys:

- `Ctrl+Win`: dictation. Hold and release, or tap to start and tap again to stop; the text is transcribed and delivered into the focused field.
- `Ctrl+Shift+Win`: dictation with cleanup through the local Ollama model before delivery.
- `Ctrl+Alt+Win`: voice command. Nothing is typed; the transcript is matched against `commands.json`.
- `Ctrl+Win` stops whatever recording is running, whichever mode started it.
- `Esc` cancels the current recording: nothing is transcribed, pasted or executed (`cancel_on_escape`).

Voice commands:

Command mode listens continuously and stops by itself as soon as a command is
recognised, so there is no need to press the hotkey a second time:

- The running recording is transcribed every `command_poll_seconds` (0.9s).
- An exact match (`выключи подсветку`) runs immediately and ends the session.
- A wildcard match (`создай задачу *`) waits for `command_capture_silence_seconds`
  of silence first, so the captured text is the whole phrase and not half of it.
- If nothing matches, the session ends after `command_silence_timeout_seconds` of
  silence, or after `command_listen_timeout_seconds` in total.
- Set `command_auto_stop` to `false` (or use the tray item `Command auto-stop`) to
  go back to plain start/stop behaviour.

A command has to *open* the phrase, so chatter cannot trigger it: `выключи подсветку`
matches "Выключи подсветку." and "выключи подсветку пожалуйста", but not "ну и потом
выключи подсветку сказал он". Pattern forms:

| Pattern | Matches |
| --- | --- |
| `выключи подсветку` | the phrase at the start of what was said |
| `создай задачу *` | the phrase at the start; the rest becomes the capture |
| `поставь таймер на * минут` | phrase around a capture |
| `*blackout` | anywhere in the sentence (explicit opt-in, easier to trigger by accident) |

Only actions listed in `C:\Apps\LocalSTT\commands.json` can run. Speech never executes
an arbitrary shell command. Command types:

- `process`: an absolute, existing `.exe`, `.cmd`, `.bat`, `.ps1` or `.vbs` path with a
  fixed argument list. `.ps1` runs through `pwsh.exe`, `.vbs` through `wscript.exe`.
- `localstt`: acts on the running app -- `action` is `language` (`value` must be one of
  the allowed languages), `delivery` (`paste` / `typewrite`) or `repeat`, which delivers
  the last dictated text again.
- `microsoft_todo`: queued locally into `%APPDATA%\LocalSTT\todo-queue.jsonl` until
  Microsoft Graph is wired up.

Shipped commands: lighting blackout / default / Aura fix, monitors off, lock the
workstation, sleep, open the install folder, switch language, switch delivery, repeat
the last text, and queue a To Do task.

Command results are logged to `%APPDATA%\LocalSTT\command-history.jsonl`
(tray: `Command history`).

Languages:

- `Language -> Russian`: force Russian recognition.
- `Language -> English`: force English recognition.
- `Language -> Auto ru/en`: let faster-whisper detect the spoken language.

API example:

```powershell
curl.exe -X POST http://127.0.0.1:7777/v1/audio/transcriptions `
  -F "file=@C:\path\sample.wav" `
  -F "model=large-v3-turbo" `
  -F "language=ru" `
  -F "response_format=json"
```

Use `-F "language=auto"` for automatic language detection or `-F "language=en"` for English.

Health check:

```powershell
C:\Apps\LocalSTT\health.ps1
```

Transcribe a file:

```powershell
C:\Apps\LocalSTT\transcribe-file.ps1 C:\path\sample.wav
```

Install or remove per-user autostart:

```powershell
C:\Apps\LocalSTT\install-autostart.ps1
C:\Apps\LocalSTT\uninstall-autostart.ps1
```

Recommended local cleanup model:

```powershell
C:\Apps\LocalSTT\install-qwen-cleanup-model.ps1
```

Or configure an already installed Ollama model:

```powershell
C:\Apps\LocalSTT\set-ollama-cleanup-model.ps1 -Model qwen3:4b-instruct -TimeoutSeconds 60
```
