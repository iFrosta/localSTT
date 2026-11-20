[CmdletBinding()]
param(
    [ValidateSet('ctranslate2', 'whisper')]
    [string]$Backend = 'ctranslate2',
    [ValidateSet('large-v3-turbo', 'large-v3', 'medium', 'small')]
    [string]$Model = 'large-v3-turbo',
    [ValidateSet('float16', 'q8_0', 'q5_0')]
    [string]$Precision = 'q8_0',
    [switch]$SkipDownload
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$projectRoot = Split-Path -Parent $nativeRoot
$buildDirectory = Join-Path $projectRoot 'build-native'
$installDirectory = Join-Path $projectRoot 'native-install'

& (Join-Path $nativeRoot 'setup.ps1') -Backend $Backend -Model $Model -Precision $Precision -SkipDownload:$SkipDownload

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmakePath) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $candidates = @(& $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
        if ($LASTEXITCODE -eq 0 -and $candidates.Count) { $cmakePath = $candidates[0] }
    }
}
if (-not $cmakePath -or -not (Test-Path -LiteralPath $cmakePath)) {
    throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++ (MSVC, Windows SDK, CMake tools).'
}
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctestPath)) { $ctestPath = (Get-Command ctest.exe -ErrorAction Stop).Source }

& $cmakePath -S $nativeRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON `
    "-DCT2_INCLUDE_DIR=$nativeRoot/vendor/ctranslate2/include" "-DCT2_IMPORT_LIBRARY=$nativeRoot/vendor/ctranslate2/ctranslate2.lib" `
    "-DLOCALSTT_TEST_MODEL=$Model"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
& $cmakePath --build $buildDirectory --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Native compilation failed ($LASTEXITCODE)" }

function Copy-Changed([IO.FileInfo]$Source, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $existing = Get-Item -LiteralPath $Destination
        if ($existing.Length -eq $Source.Length -and $existing.LastWriteTimeUtc -eq $Source.LastWriteTimeUtc) { return }
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath $Source.FullName -Destination $Destination -Force
}

New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'runtime/Release') -Filter '*.dll' -File) {
    if (Test-Path -LiteralPath (Join-Path $nativeRoot ('runtime-ct2/' + $file.Name))) { continue }
    Copy-Changed $file (Join-Path $installDirectory $file.Name)
}
# Use the newer CT2 CUDA libraries when both native engines share a DLL name.
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'runtime-ct2') -Filter '*.dll' -File) {
    Copy-Changed $file (Join-Path $installDirectory $file.Name)
}
$modelsRoot = Join-Path $nativeRoot 'models'
foreach ($file in Get-ChildItem -LiteralPath $modelsRoot -File -Recurse) {
    if ($file.Extension -in @('.bin', '.json', '.txt')) {
        $relative = $file.FullName.Substring($modelsRoot.Length + 1)
        Copy-Changed $file (Join-Path $installDirectory ('models/' + $relative))
    }
}
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'licenses') -File) {
    Copy-Changed $file (Join-Path $installDirectory ('licenses/' + $file.Name))
}
Copy-Changed (Get-Item -LiteralPath (Join-Path $nativeRoot 'dependencies.json')) (Join-Path $installDirectory 'dependencies.json')

# App-local MSVC runtime makes the copied package work without a global redist
# installation. Build Tools already provides these redistributable DLLs.
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = @(& $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0 -or -not $vsInstall.Count) { throw 'Visual Studio redist directory was not found.' }
$redistRoot = Join-Path $vsInstall[0] 'VC/Redist/MSVC'
$redist = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object Name -Match '^\d+\.' | Sort-Object Name -Descending | Select-Object -First 1
if (-not $redist) { throw 'MSVC redistributable runtime is missing from Build Tools.' }
$crtDirectory = Join-Path $redist.FullName 'x64/Microsoft.VC143.CRT'
foreach ($file in Get-ChildItem -LiteralPath $crtDirectory -File -Filter '*.dll') {
    Copy-Changed $file (Join-Path $installDirectory $file.Name)
}
# The icon the notification identity points Windows at (see apply_shell_identity).
Copy-Changed (Get-Item -LiteralPath (Join-Path $nativeRoot 'assets/localstt.ico')) (Join-Path $installDirectory 'localstt.ico')
# Editable files are copied once: a portable install keeps the user's edits.
foreach ($name in @('commands.json', 'dictionary.json', 'cleanup-prompt.txt')) {
    $destination = Join-Path $installDirectory $name
    if (-not (Test-Path -LiteralPath $destination)) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $destination
    }
}
& $ctestPath --test-dir $buildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed ($LASTEXITCODE); do not distribute this build" }
$selectedName = if ($Precision -eq 'float16') { "ggml-$Model.bin" } else { "ggml-$Model-$Precision.bin" }
Write-Host "Built and tested: $installDirectory\LocalSTT.exe"
if ($Backend -eq 'ctranslate2') { Write-Host "Selected model is bundled: models\ctranslate2\$Model" }
else { Write-Host "Selected model is bundled: models\$selectedName" }
Write-Host 'Select that model and precision in Settings if changing an existing installation.'
