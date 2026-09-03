# Chooses the cleanup model this machine can actually run, downloads it if needed and
# writes it into config.json. The right model depends on how much VRAM is left once
# Whisper is resident, so the choice is made by the self-test rather than hard-coded.
$ErrorActionPreference = "Stop"
$python = "C:\Apps\LocalSTT\.venv\Scripts\python.exe"
if (-not (Test-Path $python)) {
    throw "LocalSTT venv not found: $python"
}
Set-Location "C:\Apps\LocalSTT"
& $python -m localstt.cleanup_model --pull --apply --timeout 60
