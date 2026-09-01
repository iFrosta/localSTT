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

- Hold `Ctrl+Win`: record; release to transcribe and paste.
- Hold `Ctrl+Shift+Win`: record; release to transcribe, polish through local Ollama, and paste.

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
