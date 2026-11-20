<#
.SYNOPSIS
    Builds the "download, unzip, run" archive: the native application, its runtime
    and one model, with nothing to install.

.DESCRIPTION
    The result needs nothing on the target machine except an NVIDIA driver: no Python,
    no CUDA Toolkit, no Visual C++ redistributable (the MSVC runtime DLLs ship inside).
    The archive holds what native\build.ps1 put into native-install\: LocalSTT.exe,
    localstt-cli.exe, the CTranslate2, CUDA and cuDNN DLLs, the selected CTranslate2
    model and the Silero voice activity model, plus the licenses, the files a user edits
    (commands.json, dictionary.json, cleanup-prompt.txt) and the scripts that run it.

    This is a maintainer's script, not a user's. Run it on the machine you release from,
    then attach dist\LocalSTT-<version>-win64.zip to a GitHub release.

    Expect roughly 3.8 GB unpacked: the runtime DLLs are about 2.2 GB (cuBLAS and cuDNN
    most of it) and the large-v3-turbo model 1.6 GB. GitHub refuses release assets over
    2 GB, which the script checks before you find out by uploading.

.PARAMETER Version
    Goes into the file name. Defaults to __version__ in localstt\__init__.py, the same
    number the application reports and the update check compares.

.PARAMETER Model
    The CTranslate2 model to bundle.

.PARAMETER SkipBuild
    Package the existing native-install\ instead of running native\build.ps1 first.

.PARAMETER KeepStaging
    Do not delete the unpacked folder after zipping it, so it can be tried in place.

.EXAMPLE
    .\build-release.ps1
#>
[CmdletBinding()]
param(
    [string]$Version,
    [ValidateSet("large-v3-turbo", "large-v3", "medium", "small")]
    [string]$Model = "large-v3-turbo",
    [switch]$SkipBuild,
    [switch]$KeepStaging
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false
$root = $PSScriptRoot
. (Join-Path $PSScriptRoot "_env.ps1")

function Write-Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Note($text) { Write-Host "    $text" -ForegroundColor DarkGray }
function Write-Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }

if ($PSVersionTable.Platform -and $PSVersionTable.Platform -ne "Win32NT") {
    throw "The archive contains Windows binaries, so it has to be built on Windows."
}

# --------------------------------------------------------------- version

if (-not $Version) {
    $line = Select-String -Path (Join-Path $root "localstt\__init__.py") -Pattern '^__version__\s*=\s*"([^"]+)"' |
        Select-Object -First 1
    if (-not $line) { throw "No __version__ in localstt\__init__.py; pass -Version." }
    $Version = $line.Matches[0].Groups[1].Value
}
$name = "LocalSTT-$Version-win64"
Write-Step "Building $name"

# --------------------------------------------------------------- build

if ($SkipBuild) {
    Write-Note "Packaging the existing build in $(Get-LocalSttNativeDir)"
} else {
    # Compiles, bundles the runtime and runs the native tests; a failing test stops here.
    & (Join-Path $root "native\build.ps1") -Model $Model
}

# The executable has to report the version the archive is named after, or the update
# check inside it would compare against the wrong number.
$reported = & (Get-LocalSttNative -Console) --version
if ("$reported" -notmatch [regex]::Escape("LocalSTT $Version ")) {
    throw "localstt-cli.exe reports '$reported', not $Version. Rebuild, or pass the matching -Version."
}
Write-Ok "$reported"

# --------------------------------------------------------------- staging

$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
Write-Step "Staging in $stage"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
# The same copy install.ps1 -InstallTo makes, so the archive is an installed copy.
# The shipped versions of the editable files, never this machine's edits.
Copy-LocalSttApp -Destination $stage -Model $Model -ReplaceUserFiles
Write-Ok "Application staged"

# --------------------------------------------------------------- check it actually runs

Write-Step "Checking the bundle"

# A state folder of its own: the check must not read, or write into, this machine's
# %APPDATA%\LocalSTT, and must not ship anything from it either.
$probeState = Join-Path $dist "$name-probe-state"
if (Test-Path $probeState) { Remove-Item $probeState -Recurse -Force }
& (Join-Path $stage "localstt-cli.exe") --root $stage --state-dir $probeState --self-test | Out-Null
if ($LASTEXITCODE -ne 0) {
    # Reports on this machine, not the user's: worth seeing, not a reason to refuse.
    Write-Warn "The self-test failed on this machine. See $probeState\preflight.json."
} else {
    Write-Ok "The staged copy loads its model on the GPU without anything outside its folder"
    Remove-Item $probeState -Recurse -Force
}
foreach ($stray in @("config.json", "preflight.json", "logs")) {
    Remove-Item (Join-Path $stage $stray) -Recurse -Force -ErrorAction SilentlyContinue
}

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
"@ -ForegroundColor Gray
if ($size -ge 2) {
    Write-Warn "GitHub refuses release assets over 2 GB. Split the archive or bundle a smaller -Model."
}
