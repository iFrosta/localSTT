# Prints what CUDA, cuDNN and CTranslate2 see on this machine.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Set-Location (Get-LocalSttRoot)
& (Get-LocalSttPython) diagnostics.py
