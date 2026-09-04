<#
.SYNOPSIS
    Removes what install.ps1 created. The folder itself is left alone.

.DESCRIPTION
    Stops the app, removes the autostart shortcut and deletes the virtual environment.
    Settings, history and logs in %APPDATA%\LocalSTT are kept unless -AppData is given,
    and the downloaded Whisper models in %USERPROFILE%\.cache\huggingface are never
    touched -- other tools use them too.

.PARAMETER AppData
    Also delete %APPDATA%\LocalSTT: config.json, the dictation history, the logs and
    the self-test result.

.PARAMETER Force
    Do not ask before deleting.
#>
[CmdletBinding()]
param(
    [switch]$AppData,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Confirm-Step($text) {
    if ($Force) { return $true }
    $answer = Read-Host "$text [y/N]"
    return $answer -match '^(y|yes)$'
}

& (Join-Path $root "stop-localstt.ps1")

$shortcut = Join-Path ([Environment]::GetFolderPath("Startup")) "LocalSTT.lnk"
if (Test-Path $shortcut) {
    Remove-Item $shortcut
    "Removed the autostart shortcut."
}

$venv = Join-Path $root ".venv"
if (Test-Path $venv) {
    if (Confirm-Step "Delete the virtual environment at $venv?") {
        Remove-Item $venv -Recurse -Force
        "Removed $venv"
    }
}

if ($AppData) {
    $appDataDir = Join-Path $env:APPDATA "LocalSTT"
    if (Test-Path $appDataDir) {
        if (Confirm-Step "Delete settings, history and logs in $appDataDir?") {
            Remove-Item $appDataDir -Recurse -Force
            "Removed $appDataDir"
        }
    }
}

""
"The folder itself and the downloaded models were left in place."
"  This folder:  $root"
"  Models:       $env:USERPROFILE\.cache\huggingface"
"Delete either by hand. The model cache is shared with other tools, so check first."
