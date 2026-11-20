<#
.SYNOPSIS
    Builds LocalSTT from this checkout and sets it up: native application, model, self-test.

.DESCRIPTION
    Downloads the pinned, hash-checked dependencies (CTranslate2, the CUDA and cuDNN
    libraries, the Whisper model and the Silero voice activity model) into native\,
    compiles the application into native-install\ and runs its tests. Nothing needs
    Python: the native application runs without it, and setup only opens the Python
    wheels it takes DLLs from as ZIP archives.

    By default the application then runs from native-install\ inside this folder. With
    -InstallTo the finished application, without the sources and build files, is copied
    to a folder of its own instead. Nothing touches the registry; the only things written
    outside the folder are %APPDATA%\LocalSTT and, with -Autostart, the Startup shortcut.

    Re-running is safe: downloads already present and verified are not repeated, and
    an existing install keeps the commands, dictionary and cleanup prompt edited there.

    Building needs Visual Studio 2022 Build Tools with "Desktop development with C++"
    (MSVC, Windows SDK, CMake tools). Running the result needs only the NVIDIA driver.

.PARAMETER InstallTo
    Copy the built application to this folder and run it from there. Two locations are
    worth knowing:

        "$env:LOCALAPPDATA\Programs\LocalSTT"   per user, no administrator needed
        "$env:ProgramFiles\LocalSTT"             all users, administrator needed

    Under Program Files the files a user edits are read-only; LocalSTT then keeps an
    edited copy of them in %APPDATA%\LocalSTT, which every later read prefers.

.PARAMETER Model
    The Whisper model to download and bundle. large-v3-turbo is the default and the
    model the settings window selects first.

.PARAMETER SkipBuild
    Use the build already in native-install\ instead of downloading and compiling.

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
    [ValidateSet("large-v3-turbo", "large-v3", "medium", "small")]
    [string]$Model = "large-v3-turbo",
    [switch]$SkipBuild,
    [switch]$Autostart,
    [switch]$SkipSelfTest
)

$ErrorActionPreference = "Stop"
# Native tools write ordinary notices to stderr. On PowerShell 7.3+ that would
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

# The tray icon and the settings window use Segoe UI and the Fluent icon font. They are
# part of Windows 10/11, so this is a sanity check.
$fontDir = Join-Path $env:WINDIR "Fonts"
if (-not (Test-Path (Join-Path $fontDir "segoeui.ttf"))) {
    Write-Warn "Segoe UI was not found; the interface will fall back to another font."
}

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

# --------------------------------------------------------------- build

if ($SkipBuild) {
    Write-Step "Using the existing build"
    Write-Ok (Get-LocalSttNative)
} else {
    Write-Step "Looking for the C++ build tools"

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $tools = $null
    if (Test-Path $vswhere) {
        $tools = & $vswhere -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (-not $tools) {
        Write-Host ""
        Write-Host "Building LocalSTT needs Visual Studio 2022 Build Tools with the C++ workload:" -ForegroundColor Red
        Write-Host "    winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override ``" -ForegroundColor Red
        Write-Host "      `"--passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" -ForegroundColor Red
        Write-Host ""
        Write-Host "Or download the release archive, which needs no build at all." -ForegroundColor Red
        throw "C++ build tools not found"
    }
    Write-Ok "$tools"

    Write-Step "Downloading dependencies and building ($Model)"
    Write-Note "The first run downloads several GB: CUDA and cuDNN libraries and the model."
    & (Join-Path $root "native\build.ps1") -Model $Model
    Write-Ok "Built and tested: $(Get-LocalSttNative)"
}

# --------------------------------------------------------------- copy into place

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

$appRoot = $root
if ($InstallTo) {
    Write-Step "Copying to $InstallTo"

    $InstallTo = [System.IO.Path]::GetFullPath($InstallTo)
    if ($InstallTo -eq $root) {
        throw "-InstallTo is the folder this script is already in; drop the parameter."
    }

    # Program Files is the case worth naming, but any folder the user cannot write is
    # the same problem, and saying so before copying half of it is kinder.
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

    # A copy that is running keeps its executables locked.
    $stop = Join-Path $InstallTo "stop-localstt.ps1"
    if (Test-Path $stop) { & $stop | Out-Null }

    Copy-LocalSttApp -Destination $InstallTo -Model $Model
    $appRoot = $InstallTo
    Write-Ok "Application copied to $appRoot"
}

if ($Autostart) {
    & (Join-Path $appRoot "install-autostart.ps1")
}

# --------------------------------------------------------------- self-test

if (-not $SkipSelfTest) {
    Write-Step "Running the self-test"
    $cli = Join-Path $appRoot "localstt-cli.exe"
    if (-not (Test-Path $cli)) { $cli = Join-Path $appRoot "native-install\localstt-cli.exe" }
    & $cli --root $appRoot --self-test | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Warn "The self-test found something that blocks transcription. Run run-diagnostics.ps1 for the report."
    } else {
        Write-Ok "Model, GPU and microphone are ready. The report is on the Health page in Settings."
    }
}

# --------------------------------------------------------------- done

Write-Step "Done"
Write-Host @"
    Start it:            $appRoot\start-localstt.vbs   (no console)
    Start with a log:    $appRoot\run-dev.ps1
    Start with Windows:  $appRoot\install-autostart.ps1
    Settings:            click the tray icon

    Hold Ctrl+Win and talk. Let go and the text is typed where the cursor is.
    The hotkeys can be changed in Settings > Hotkeys.
"@ -ForegroundColor Gray
