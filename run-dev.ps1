# Runs the app in this console, so log lines and errors are visible.
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "run-native.ps1") -Console
