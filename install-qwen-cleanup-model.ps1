# Chooses the cleanup model this machine can actually run, downloads it if needed and
# writes it into config.json. The right model depends on how much VRAM is left once
# Whisper is resident, so the choice is made by the self-test rather than hard-coded.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Set-Location (Get-LocalSttRoot)
& (Get-LocalSttPython) -m localstt.cleanup_model --pull --apply --timeout 60
