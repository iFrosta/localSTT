# Starts LocalSTT with Windows, as a shortcut in this user's Startup folder. The same
# shortcut the "Start with Windows" switch in Settings writes.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

$root = Get-LocalSttRoot
$binary = Get-LocalSttNative
$shortcut = Join-Path ([Environment]::GetFolderPath("Startup")) "LocalSTT.lnk"

# Kept so an older shortcut (the Python launcher, say) can be put back by hand.
$state = Join-Path $env:APPDATA "LocalSTT"
New-Item -ItemType Directory -Path $state -Force | Out-Null
if (Test-Path -LiteralPath $shortcut) {
    Copy-Item -LiteralPath $shortcut -Destination (Join-Path $state "autostart.previous.lnk") -Force
}

$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($shortcut)
$lnk.TargetPath = $binary
$lnk.Arguments = '--root "' + $root + '"'
$lnk.WorkingDirectory = Split-Path -Parent $binary
$lnk.IconLocation = "$binary,0"
$lnk.Save()
"Created per-user autostart shortcut: $shortcut"
