<#
.SYNOPSIS
    Sets LocalSTT up in place: virtual environment, dependencies, self-test.

.DESCRIPTION
    By default everything lands next to this script, so the folder can sit anywhere and
    be moved later. Nothing touches the registry; the only thing written outside the
    folder is %APPDATA%\LocalSTT, and the Startup shortcut if -Autostart is given.

    Re-running is safe: an existing virtual environment is reused and the packages in it
    are brought up to what the requirements ask for.

.PARAMETER InstallTo
    Copy the application to this folder and install there, instead of running from where
    the sources are. Two locations are worth knowing:

        "$env:LOCALAPPDATA\Programs\LocalSTT"   per user, no administrator needed
        "$env:ProgramFiles\LocalSTT"             all users, administrator needed

    Per user is the better choice for this application. Its dependencies live in a
    virtual environment inside the install folder, so upgrading them means writing
    there -- which under Program Files needs an elevated console every time.

.PARAMETER InstallPython
    Install Python with winget without asking first. Without it the script offers to do
    the same thing interactively when no suitable Python is found.

.PARAMETER SkipCuda
    Do not install the cuBLAS and cuDNN wheels. Only for a machine that already has a
    matching CUDA 12 runtime on the search path -- LocalSTT will not start without one.

.PARAMETER Latest
    Install the newest release of every dependency instead of the tested set in
    requirements-lock.txt.

.PARAMETER Autostart
    Also start LocalSTT when Windows starts, for this user.

.PARAMETER SkipSelfTest
    Do not run the self-test at the end.

.EXAMPLE
    .\install.ps1
.EXAMPLE
    .\install.ps1 -InstallTo "$env:LOCALAPPDATA\Programs\LocalSTT" -Autostart
#>
[CmdletBinding()]
param(
    [string]$InstallTo,
    [switch]$InstallPython,
    [switch]$SkipCuda,
    [switch]$Latest,
    [switch]$Autostart,
    [switch]$SkipSelfTest
)

$ErrorActionPreference = "Stop"
# pip and nvidia-smi write ordinary notices to stderr. On PowerShell 7.3+ that would
# otherwise be a terminating error and abort a perfectly good install.
$PSNativeCommandUseErrorActionPreference = $false
$root = $PSScriptRoot
. (Join-Path $PSScriptRoot "_env.ps1")

# Least to most severe; each step says which it is and the script stops on a failure.
function Write-Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Note($text) { Write-Host "    $text" -ForegroundColor DarkGray }
function Write-Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }

# --------------------------------------------------------------- system requirements

Write-Step "Checking this machine"

if ($PSVersionTable.Platform -and $PSVersionTable.Platform -ne "Win32NT") {
    throw "LocalSTT runs on Windows only. See the README for what a port would involve."
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw "A 64-bit Windows is required; the model runtime has no 32-bit build."
}

$caption = (Get-CimInstance Win32_OperatingSystem).Caption
$currentVersion = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
$build = [int]$currentVersion.CurrentBuildNumber
Write-Ok "$caption (build $build)"
if ($build -lt 19041) {
    Write-Warn "Windows 10 2004 (build 19041) or newer is expected; older builds are untested."
}

# The tray icon, the settings window and the text delivery all use Segoe UI and the
# Fluent icon font. They are part of Windows 10/11, so this is a sanity check.
$fontDir = Join-Path $env:WINDIR "Fonts"
if (-not (Test-Path (Join-Path $fontDir "segoeui.ttf"))) {
    Write-Warn "Segoe UI was not found; the interface will fall back to another font."
}

# --------------------------------------------------------------- python

Write-Step "Looking for Python"


function Install-Python {
    <#
        .SYNOPSIS
        Install Python 3.12 for this user with winget, and find it afterwards.
    #>
    $winget = Get-Command "winget.exe" -ErrorAction SilentlyContinue
    if (-not $winget) { return $null }

    Write-Note "Installing Python 3.12 with winget. This takes a minute."
    & $winget.Source install --exact --id Python.Python.3.12 --scope user --silent `
        --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        Write-Warn "winget could not install Python (exit code $LASTEXITCODE)."
        return $null
    }

    # A fresh install is not on this process's PATH: the environment was captured when
    # the console started. Re-read it, and look in the install folder either way.
    $env:PATH = [Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("PATH", "User")
    return Find-LocalSttPython
}

$python = Find-LocalSttPython
if (-not $python) {
    Write-Warn "No 64-bit Python 3.10 - 3.13 on this machine."

    $shouldInstall = $InstallPython
    if (-not $shouldInstall -and [Environment]::UserInteractive) {
        $answer = Read-Host "    Install Python 3.12 now, for this user only? [Y/n]"
        $shouldInstall = $answer -notmatch '^(n|no)$'
    }
    if ($shouldInstall) { $python = Install-Python }
}

if (-not $python) {
    Write-Host ""
    Write-Host "LocalSTT needs 64-bit Python 3.10 - 3.13. Install it with either of:" -ForegroundColor Red
    Write-Host "    winget install -e --id Python.Python.3.12" -ForegroundColor Red
    Write-Host "    https://www.python.org/downloads/windows/" -ForegroundColor Red
    Write-Host ""
    Write-Host "Then run this script again. It does not need Python on PATH: an install" -ForegroundColor Red
    Write-Host "for the current user, or a Python unpacked into this folder, is found too." -ForegroundColor Red
    throw "Python not found"
}
Write-Ok "Python $($python.Version) at $($python.Path)"

# --------------------------------------------------------------- gpu

Write-Step "Looking for an NVIDIA GPU"

$gpuName = $null
$smi = Get-Command "nvidia-smi.exe" -ErrorAction SilentlyContinue
if ($smi) {
    $gpuName = (& $smi.Source --query-gpu=name,driver_version,memory.total,compute_cap --format=csv,noheader 2>$null |
        Select-Object -First 1)
}
if ($gpuName) {
    Write-Ok "$($gpuName.Trim())"
} else {
    # Not fatal here on purpose: someone may be installing before the card or the driver
    # is in place. The self-test at the end says the same thing, and the app repeats it.
    Write-Warn "No NVIDIA GPU found. LocalSTT transcribes on the GPU and has no CPU"
    Write-Warn "fallback, so it will refuse to start until one is present."
}

# --------------------------------------------------------------- copy into place

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($InstallTo) {
    Write-Step "Copying to $InstallTo"

    $InstallTo = [System.IO.Path]::GetFullPath($InstallTo)
    if ($InstallTo -eq $root) {
        throw "-InstallTo is the folder this script is already in; drop the parameter."
    }

    # Program Files is the case worth naming, but any folder the user cannot write is
    # the same problem, and saying so before copying half of it is kinder.
    $parent = Split-Path -Parent $InstallTo
    $needsAdmin = $false
    foreach ($protected in @([Environment]::GetFolderPath("ProgramFiles"),
                             ${env:ProgramFiles(x86)},
                             $env:ProgramW6432)) {
        if ($protected -and $InstallTo.StartsWith($protected, [StringComparison]::OrdinalIgnoreCase)) {
            $needsAdmin = $true
        }
    }
    if ($needsAdmin -and -not (Test-Administrator)) {
        Write-Host ""
        Write-Host "$InstallTo needs an elevated console." -ForegroundColor Red
        Write-Host "Either start PowerShell as administrator and run this again, or" -ForegroundColor Red
        Write-Host "install for this user only, which needs no elevation:" -ForegroundColor Red
        Write-Host ""
        Write-Host "    .\install.ps1 -InstallTo `"`$env:LOCALAPPDATA\Programs\LocalSTT`"" -ForegroundColor Red
        throw "Administrator rights required for $InstallTo"
    }
    New-Item -ItemType Directory -Force -Path $parent | Out-Null

    # robocopy, not Copy-Item: it takes the exclusions as arguments and does not choke
    # on a long path. 0-7 are its success codes; 8 and up are real failures.
    $excludedDirs = @(".git", ".venv", "__pycache__", "Python312", "downloads",
                      "diagnostics", ".pytest_cache", ".mypy_cache", ".ruff_cache")
    robocopy $root $InstallTo /E /XD @excludedDirs /XF "*.pyc" /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying to $InstallTo failed (robocopy $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0

    $root = $InstallTo
    Write-Ok "Application copied to $root"
    if ($needsAdmin) {
        Write-Warn "Installed for all users. Upgrading the dependencies later means"
        Write-Warn "running this script from an elevated console again."
    }
}

# --------------------------------------------------------------- virtual environment

Write-Step "Creating the virtual environment"

$venv = Join-Path $root ".venv"
$venvPython = Join-Path $venv "Scripts\python.exe"
if (Test-Path $venvPython) {
    Write-Note "Reusing $venv"
} else {
    & $python.Path -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "Could not create the virtual environment in $venv" }
    Write-Ok "Created $venv"
}

& $venvPython -m pip install --upgrade pip --quiet --disable-pip-version-check
if ($LASTEXITCODE -ne 0) { throw "Could not upgrade pip" }

# --------------------------------------------------------------- dependencies

Write-Step "Installing dependencies"

$core = if ($Latest) { "requirements.txt" } else { "requirements-lock.txt" }
$cuda = if ($Latest) { "requirements-cuda.txt" } else { "requirements-cuda-lock.txt" }

Write-Note "From $core"
& $venvPython -m pip install --disable-pip-version-check -r (Join-Path $root $core)
if ($LASTEXITCODE -ne 0) { throw "Installing $core failed" }

if ($SkipCuda) {
    Write-Warn "Skipping cuBLAS and cuDNN as asked. CTranslate2 needs them on the search path."
} else {
    Write-Note "From $cuda (cuBLAS and cuDNN, about 1 GB)"
    & $venvPython -m pip install --disable-pip-version-check -r (Join-Path $root $cuda)
    if ($LASTEXITCODE -ne 0) { throw "Installing $cuda failed" }
}
Write-Ok "Dependencies installed"

# --------------------------------------------------------------- configuration

Write-Step "Writing the configuration"

# Creates %APPDATA%\LocalSTT\config.json with the defaults if it is not there yet, and
# leaves an existing one alone.
Set-Location $root
& $venvPython -c "from localstt.config import load_config, save_config, CONFIG_PATH; save_config(load_config()); print(CONFIG_PATH)"
if ($LASTEXITCODE -ne 0) { throw "Could not write config.json" }

if ($Autostart) {
    & (Join-Path $root "install-autostart.ps1")
}

# --------------------------------------------------------------- self-test

if (-not $SkipSelfTest) {
    Write-Step "Running the self-test"
    Write-Note "The model is downloaded on the first run; this can take a few minutes."
    & $venvPython -m localstt.preflight
    if ($LASTEXITCODE -ne 0) {
        Write-Warn "The self-test found something that blocks transcription -- see above."
    }
}

# --------------------------------------------------------------- done

Write-Step "Done"
Write-Host @"
    Start it:            $root\start-localstt.vbs   (no console)
    Start with a log:    $root\run-dev.ps1
    Start with Windows:  $root\install-autostart.ps1
    Settings:            right-click the tray icon

    Hold Ctrl+Win and talk. Let go and the text is typed where the cursor is.
"@ -ForegroundColor Gray
