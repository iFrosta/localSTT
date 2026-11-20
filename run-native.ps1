# Starts the native tray application, or with -Console runs localstt-cli.exe in this
# console so its log lines and errors are visible.
[CmdletBinding()]
param([switch]$Console)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

$root = Get-LocalSttRoot
$binary = Get-LocalSttNative -Console:$Console
if ($Console) {
    & $binary --root $root
    exit $LASTEXITCODE
}
Start-Process -FilePath $binary -ArgumentList ('--root "' + $root + '"') -WorkingDirectory $root -WindowStyle Hidden
