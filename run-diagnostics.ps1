# Runs the self-test: model and CUDA backend, microphones, Ollama and voice commands.
# The report is also saved to %APPDATA%\LocalSTT\preflight.json for the Health page.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Invoke-LocalSttCli -Arguments @("--self-test") -FailureMessage "The self-test reported a blocking failure"
