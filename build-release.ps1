<#
.SYNOPSIS
    Builds the "download, unzip, run" archive: the application, a Python and every
    dependency already installed into it.

.DESCRIPTION
    The result needs nothing on the target machine except an NVIDIA driver. There is no
    Python to install, no pip to run and no virtual environment -- the interpreter lives
    in python\ inside the folder and the launchers prefer it when there is no .venv.

    This is a maintainer's script, not a user's. Run it on the machine you release from,
    then attach dist\LocalSTT-<version>-win64.zip to a GitHub release.

    Expect roughly 2.5 GB unpacked and 1.5 GB zipped: the cuBLAS and cuDNN wheels alone
    are 1.3 GB. -SkipCuda builds a ~350 MB archive for people who already have a CUDA 12
    runtime on the search path, which is a much smaller audience than it sounds.

.PARAMETER Version
    Goes into the file name. Defaults to the current git tag, else the date.

.PARAMETER Python
    The interpreter to copy. Defaults to the newest suitable one found; it must be a real
    installation, not a virtual environment (the script resolves it either way).

.PARAMETER SkipCuda
    Leave the NVIDIA wheels out. The archive is named -nocuda so both can sit on the
    same release.

.PARAMETER Both
    Build both archives, one after the other. The second run reuses what pip already
    downloaded, so it costs minutes rather than another gigabyte.

.PARAMETER KeepStaging
    Do not delete the unpacked folder after zipping it, so it can be tried in place.

.EXAMPLE
    .\build-release.ps1 -Version 1.0.0 -Both
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Python,
    [switch]$SkipCuda,
    [switch]$Both,
    [switch]$KeepStaging
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false
$root = $PSScriptRoot

function Write-Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Note($text) { Write-Host "    $text" -ForegroundColor DarkGray }
function Write-Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }

if ($PSVersionTable.Platform -and $PSVersionTable.Platform -ne "Win32NT") {
    throw "The archive contains Windows binaries, so it has to be built on Windows."
}

# --------------------------------------------------------------- version

if (-not $Version) {
    $described = & git -C $root describe --tags --abbrev=0 2>$null
    $Version = if ($LASTEXITCODE -eq 0 -and $described) {
        $described.Trim().TrimStart("v")
    } else {
        Get-Date -Format "yyyy.MM.dd"
    }
    $global:LASTEXITCODE = 0
}
# Both archives live on the same release, so the names have to differ.
$suffix = if ($SkipCuda) { "-nocuda" } else { "" }
$name = "LocalSTT-$Version-win64$suffix"

if ($Both) {
    # Running the script twice rather than building twice inside one run: each archive
    # is then built by exactly the code path a single build uses.
    & $PSCommandPath -Version $Version -Python $Python -KeepStaging:$KeepStaging
    & $PSCommandPath -Version $Version -Python $Python -KeepStaging:$KeepStaging -SkipCuda
    Write-Step "Both archives are in $(Join-Path $root 'dist')"
    Get-ChildItem (Join-Path $root "dist") -Filter "*.zip" |
        ForEach-Object { Write-Host ("    {0,-42} {1,8:N2} GB" -f $_.Name, ($_.Length / 1GB)) -ForegroundColor Gray }
    exit 0
}

Write-Step "Building $name"

# --------------------------------------------------------------- the interpreter to copy

Write-Step "Choosing the Python to bundle"

if (-not $Python) {
    foreach ($candidate in @("py.exe", "python.exe")) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($found) {
            $Python = if ($candidate -eq "py.exe") { "$($found.Source)|-3.12" } else { $found.Source }
            break
        }
    }
}
if (-not $Python) { throw "No Python found. Pass -Python <path to python.exe>." }

$exe, $prefix = $Python -split "\|", 2
$arguments = @()
if ($prefix) { $arguments += $prefix }

# A virtual environment is not copyable: its pyvenv.cfg points at the real installation
# by absolute path. base_prefix is that installation, whichever was handed to us.
$probe = "import sys;print(sys.base_prefix);print('{}.{}.{}'.format(*sys.version_info[:3]))"
$answer = & $exe @arguments -c $probe
if ($LASTEXITCODE -ne 0) { throw "Could not run $exe" }
$basePrefix, $pythonVersion = $answer

if (-not (Test-Path (Join-Path $basePrefix "python.exe"))) {
    throw "$basePrefix does not look like a Python installation."
}
Write-Ok "Python $pythonVersion from $basePrefix"

# tkinter draws the settings window and the tray menu, and the embeddable distribution
# leaves it out. Finding that at the user's first right-click is far too late.
& (Join-Path $basePrefix "python.exe") -c "import tkinter" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "That Python has no tkinter. Use a python.org installation, not the embeddable package."
}
Write-Ok "tkinter present"

# --------------------------------------------------------------- staging

$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
Write-Step "Staging in $stage"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$excludedDirs = @(".git", ".venv", "__pycache__", "Python312", "downloads", "dist",
                  "diagnostics", ".pytest_cache", ".mypy_cache", ".ruff_cache")
robocopy $root $stage /E /XD @excludedDirs /XF "*.pyc" /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying the application failed (robocopy $LASTEXITCODE)" }
$global:LASTEXITCODE = 0

# Maintainer's tools; nothing in the archive should invite a user to rebuild it.
Remove-Item (Join-Path $stage "build-release.ps1") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $stage "docs\capture-screenshots.py") -Force -ErrorAction SilentlyContinue
Write-Ok "Application staged"

robocopy $basePrefix (Join-Path $stage "python") /E /XD "__pycache__" /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying Python failed (robocopy $LASTEXITCODE)" }
$global:LASTEXITCODE = 0
$stagedPython = Join-Path $stage "python\python.exe"
Write-Ok "Python copied"

# --------------------------------------------------------------- dependencies

Write-Step "Installing the dependencies into the copy"

& $stagedPython -m ensurepip --upgrade 2>$null | Out-Null
$global:LASTEXITCODE = 0

$files = @("requirements-lock.txt")
if (-not $SkipCuda) { $files += "requirements-cuda-lock.txt" } else { Write-Warn "Leaving the NVIDIA wheels out." }
foreach ($file in $files) {
    Write-Note "From $file"
    & $stagedPython -m pip install --no-warn-script-location -r (Join-Path $stage $file)
    if ($LASTEXITCODE -ne 0) { throw "Installing $file failed" }
}

# --------------------------------------------------------------- check it actually runs

Write-Step "Checking the bundle"

Push-Location $stage
try {
    & $stagedPython -c "import localstt.config, localstt.backends, tkinter, pynput, pystray; print('imports resolve')"
    if ($LASTEXITCODE -ne 0) { throw "The bundled Python cannot import LocalSTT and its dependencies" }
    Write-Ok "Imports resolve inside the bundle"

    # Reports on this machine, not the user's, so a failure here is worth seeing but is
    # not a reason to refuse to build.
    & $stagedPython -m localstt.preflight
    if ($LASTEXITCODE -ne 0) { Write-Warn "The self-test failed on this machine -- see above." }
} finally {
    Pop-Location
}

Get-ChildItem $stage -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# The self-test writes to %APPDATA%, but a stray config in the archive would ship one
# machine's settings to everyone.
Remove-Item (Join-Path $stage "config.json") -Force -ErrorAction SilentlyContinue

# --------------------------------------------------------------- zip

Write-Step "Compressing"

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true
)

$size = (Get-Item $zip).Length / 1GB
$unpacked = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1GB
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash

if (-not $KeepStaging) { Remove-Item $stage -Recurse -Force }

Write-Step "Done"
Write-Host @"
    $zip
    zipped    $([math]::Round($size, 2)) GB
    unpacked  $([math]::Round($unpacked, 2)) GB
    sha256    $hash

    GitHub refuses release assets over 2 GB.
"@ -ForegroundColor Gray
