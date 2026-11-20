# Chooses the cleanup model this machine can actually run, downloads it if needed and
# writes it into config.json. The right model depends on how much VRAM is left once
# Whisper is resident, so the choice is made from the GPU rather than hard-coded.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Invoke-LocalSttCli -Arguments @("--cleanup-model", "--pull", "--apply", "--timeout", "60") `
    -FailureMessage "Cleanup model installation failed"
