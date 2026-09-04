# Runs the tray app in this console, so log lines and tracebacks are visible.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Set-Location (Get-LocalSttRoot)
& (Get-LocalSttPython) -m localstt.main
