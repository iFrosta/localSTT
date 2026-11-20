# Rollback to the Python application, kept on this branch for comparison and for
# machines where the native build is not available. Stop the native app first: both
# use the same hotkeys and API port. Needs the virtual environment that the main
# branch's install.ps1 creates.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Set-Location (Get-LocalSttRoot)
& (Get-LocalSttPython) -m localstt.main
