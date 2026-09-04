# Starts LocalSTT with Windows, as a shortcut in this user's Startup folder.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

$root = Get-LocalSttRoot
$launcher = Join-Path $root "start-localstt.vbs"
if (-not (Test-Path $launcher)) { throw "Launcher not found: $launcher" }

$shortcut = Join-Path ([Environment]::GetFolderPath("Startup")) "LocalSTT.lnk"
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($shortcut)
$lnk.TargetPath = $launcher
$lnk.WorkingDirectory = $root
$lnk.IconLocation = "$launcher,0"
$lnk.Save()
"Created per-user autostart shortcut: $shortcut"
