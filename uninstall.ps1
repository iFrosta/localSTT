<#
.SYNOPSIS
    Removes what install.ps1 created. The folder itself is left alone.

.DESCRIPTION
    Stops the app, removes the autostart shortcut and the notification identity in the
    registry. In a source checkout it also offers to delete the build output and the
    downloaded dependencies (several GB: CUDA and cuDNN libraries, the models). An
    installed copy or an unpacked release archive is deleted by deleting its folder.

    Settings, history and logs in %APPDATA%\LocalSTT are kept unless -AppData is given.

.PARAMETER AppData
    Also delete %APPDATA%\LocalSTT: config.json, the dictation history, the logs, the
    self-test result and any edited copies of commands.json, dictionary.json and
    cleanup-prompt.txt.

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

# The one registry key LocalSTT writes: the name and icon Windows puts on its
# notifications. Removing it costs nothing if it was never created.
$appId = "HKCU:\Software\Classes\AppUserModelId\iFrosta.LocalSTT"
if (Test-Path $appId) {
    Remove-Item $appId -Recurse -Force
    "Removed the notification identity from the registry."
}

# Only a source checkout has these. The downloads are verified by hash, so deleting
# them costs a download on the next build and nothing else.
$build = @("native-install", "build-native") | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
if ($build -and (Confirm-Step "Delete the build output ($($build -join ', '))?")) {
    $build | ForEach-Object { Remove-Item $_ -Recurse -Force; "Removed $_" }
}
$downloads = @("downloads", "vendor", "runtime", "runtime-ct2", "models") |
    ForEach-Object { Join-Path $root "native\$_" } | Where-Object { Test-Path $_ }
if ($downloads -and (Confirm-Step "Delete the downloaded dependencies and models in $(Join-Path $root 'native')?")) {
    $downloads | ForEach-Object { Remove-Item $_ -Recurse -Force; "Removed $_" }
}

# The Python rollback's environment, if one was ever created here.
$venv = Join-Path $root ".venv"
if (Test-Path $venv) {
    if (Confirm-Step "Delete the Python virtual environment at $venv?") {
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
"The folder itself was left in place: $root"
"Delete it by hand once nothing in it is needed."
