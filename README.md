# LocalSTT

Local Windows faster-whisper dictation and OpenAI-compatible STT API.

- Install dir: `C:\Apps\LocalSTT`
- Venv: `C:\Apps\LocalSTT\.venv`
- Config: `%APPDATA%\LocalSTT\config.json`
- Logs: `%APPDATA%\LocalSTT\logs\localstt.log`
- API: `http://127.0.0.1:7777`

Self-test:

LocalSTT runs a self-test the first time it starts on a device it has not seen before,
and repeats it until that device passes. It checks the NVIDIA driver version, the GPU's
VRAM and compute capability, the CUDA runtime DLLs in the venv, that CTranslate2 sees a
CUDA device, that the configured model and compute type fit in VRAM, the microphone, the
API port, which voice commands this machine can run, and whether the Ollama cleanup model
fits in the VRAM left over after Whisper.

Only failures that make transcription impossible stop the app; a missing microphone or a
busy port are reported and the app still starts. Run it any time from the tray menu
(`Run self-test`) or from a console:

```powershell
C:\Apps\LocalSTT\.venv\Scripts\python.exe -m localstt.preflight
```

The last result is kept in `%APPDATA%\LocalSTT\preflight.json`.

Settings:

`Settings` in the tray menu opens a window with every option in `config.json` grouped
into sections, plus the command list, the history, the timings and the self-test.
`Open config.json` at the bottom still edits the file by hand. Changing the model,
compute type or API port needs a restart; everything else applies on save.

The clipboard is left alone by default. Typing the text needs no clipboard, so
whatever you had copied stays there; turn on `Settings -> Delivery -> Leave the
transcript in the clipboard` (or the tray's `Delivery -> Copy to clipboard`) to have
each transcript end up there as well. Paste delivery has to borrow the clipboard, so
with the setting off it puts the old contents back afterwards. Either way the last
transcript is written to `%APPDATA%\LocalSTT\last-transcript.txt`.

History is off by default: with it on, every transcript is appended to
`%APPDATA%\LocalSTT\history.jsonl` and listed in `Settings -> History` by date, in the
order it was recorded.

Performance:

`Settings -> Performance` shows how long the last dictation took. Recognition speed is
reported as the real-time factor -- processing time over the length of the audio -- and
as the multiple of real time it achieves. Characters per second is shown too but means
little: it mostly measures how fast the speaker talks. The cleanup step is a language
model, so it is measured in tokens per second, taken from Ollama's own counters.
Turn `Measure performance` off to stop writing `performance.json`.

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
- Any of the three stops whatever recording is running, whichever mode started it.
- `Esc` cancels the current recording: nothing is transcribed, pasted or executed (`cancel_on_escape`).

Those are the defaults. `Settings -> Hotkeys` rebinds each of them: click the field,
press the combination, let go. Any key can take part, not only modifiers -- `F9` or
`Ctrl+Alt+D` work as well as `Ctrl+Win` -- and the glyph on the right of the field
clears a binding so that mode has no hotkey at all. The chords are stored in
`config.json` as `hotkey_dictation`, `hotkey_cleanup`, `hotkey_command` and
`hotkey_cancel` (`"ctrl+shift+win"`), and a new one takes effect on save without a
restart. Two modes cannot share a chord: saving says so instead.

A mode whose chord extends another one -- cleanup is dictation plus `Shift` -- is
recognised through `hotkey_mode_grace_seconds`: the recording starts on the first
chord and the mode is settled once that grace period has passed, so the extra key
has to arrive within it.

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

Commands whose target is not installed on this machine are switched off automatically
rather than failing when spoken: a `process` command whose executable is missing, a
`.ps1` command with no `pwsh.exe`, or an unmet `requires` entry. The tray's
`Settings -> Voice commands` page lists every command with the reason it is off, and its
toggle writes `"enabled": false` back into `commands.json`.

Command paths may use `%LOCALAPPDATA%`-style variables or a bare executable name resolved
through `PATH`. A command may also declare what it needs:

| `requires` entry | Satisfied when |
| --- | --- |
| `path:C:\tools\Thing.exe` | that file exists |
| `exe:wsl.exe` | the name resolves through `PATH` |
| `appx:Claude_pzs8sxrjxfjjc` | that Store package is installed |
| `wsl:Ubuntu:claude` | `claude` is on the `PATH` inside the Ubuntu distro |

Order matters. The matcher runs the first command whose phrase opens the sentence, and
"открой терминал" opens "открой терминал ubuntu" as well, so the specific variants are
listed before the general one.

Only actions listed in `C:\Apps\LocalSTT\commands.json` can run. Speech never executes
an arbitrary shell command. Command types:

- `process`: an absolute, existing `.exe`, `.cmd`, `.bat`, `.ps1` or `.vbs` path with a
  fixed argument list. `.ps1` runs through `pwsh.exe`, `.vbs` through `wscript.exe`.
- `localstt`: acts on the running app -- `action` is `language` (`value` must be one of
  the allowed languages), `delivery` (`paste` / `typewrite`) or `repeat`, which delivers
  the last dictated text again.
- `microsoft_todo`: queued locally into `%APPDATA%\LocalSTT\todo-queue.jsonl` until
  Microsoft Graph is wired up.
- `app_launch`: opens whatever application the capture names, without needing an entry of
  its own. "Открой калькулятор", "запусти обсидиан", "открой чат гпт" all work.

Application launching:

`app_launch` matches against every app the Start menu can open -- desktop shortcuts, Store
packages and registered app ids alike -- which Windows exposes through `Get-StartApps`.
The index is cached in `%APPDATA%\LocalSTT\app-index.json` and rebuilt daily, or on
demand from `Settings -> Voice commands -> Rebuild app index`.

Because Whisper writes Russian speech in Cyrillic, spoken names of Latin-named apps are
resolved through transliteration, fuzzy matching, and an alias file at
`%APPDATA%\LocalSTT\app-aliases.json` (`Settings -> Voice commands -> Edit app names`).
Add a line there for anything that resolves to the wrong app:

```json
{ "калькулятор": "Calculator", "клод": "Claude" }
```

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

Autostart:

`Settings -> General -> Start with Windows` adds or removes the Startup shortcut, and
takes effect immediately. The same thing from a console:

```powershell
C:\Apps\LocalSTT\install-autostart.ps1
C:\Apps\LocalSTT\uninstall-autostart.ps1
```

Cleanup model:

Cleanup dictation runs an Ollama model on the same GPU as Whisper, so the right model is
whatever fits in the VRAM left over -- on a 4 GB card that is under 2 GB. Let LocalSTT
pick and download one:

```powershell
C:\Apps\LocalSTT\install-qwen-cleanup-model.ps1
```

To see the choice without changing anything:

```powershell
C:\Apps\LocalSTT\.venv\Scripts\python.exe -m localstt.cleanup_model
```

To override it explicitly, adding `-Pull` if it still has to be downloaded:

```powershell
C:\Apps\LocalSTT\set-ollama-cleanup-model.ps1 -Model qwen3.5:0.8b -TimeoutSeconds 60
```

`Settings -> AI cleanup` shows the same recommendation with a download button.
