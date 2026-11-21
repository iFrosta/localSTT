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
$nativePrefix = $nativeRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$manifest = Get-Content -LiteralPath (Join-Path $nativeRoot 'dependencies.json') -Raw -Encoding UTF8 | ConvertFrom-Json

function Native-Path([string]$Relative) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $nativeRoot $Relative))
    if (-not $resolved.StartsWith($nativePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Dependency path escapes native directory: $Relative"
    }
    return $resolved
}

function Test-Hash([string]$Path, [string]$Expected) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Expected)
}

function Get-Dependency($Item) {
    $destination = Native-Path $Item.path
    if (Test-Hash $destination $Item.sha256) { return }
    if ($SkipDownload) { throw "Missing or invalid cached dependency: $destination. Run setup.ps1 without -SkipDownload." }
    $curl = Get-Command curl.exe -ErrorAction Stop
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    # Both paths are checked within native/; a failed download never replaces a valid file.
    $partial = Native-Path ($Item.path + '.partial')
    Write-Host "Downloading $($Item.path)"
    & $curl.Source --fail --location --connect-timeout 15 --max-time 1800 --output $partial $Item.url
    if ($LASTEXITCODE -ne 0) { throw "Download failed ($LASTEXITCODE): $($Item.url). Partial file retained for diagnosis." }
    if (-not (Test-Hash $partial $Item.sha256)) { throw "SHA256 mismatch: $($Item.path). Partial file was not installed." }
    Move-Item -LiteralPath $partial -Destination $destination -Force
}

foreach ($item in $manifest.sources) { Get-Dependency $item }
Get-Dependency $manifest.runtime_archive

$runtimeValid = $true
foreach ($item in $manifest.runtime) {
    if (-not (Test-Hash (Native-Path $item.path) $item.sha256)) { $runtimeValid = $false; break }
}
if (-not $runtimeValid) {
    $archive = Native-Path $manifest.runtime_archive.path
    $runtimeRoot = Native-Path 'runtime'
    Write-Host 'Extracting checked whisper.cpp / CUDA runtime'
    Expand-Archive -LiteralPath $archive -DestinationPath $runtimeRoot -Force
    foreach ($item in $manifest.runtime) {
        if (-not (Test-Hash (Native-Path $item.path) $item.sha256)) {
            throw "Extracted runtime checksum mismatch: $($item.path)"
        }
    }
}
Get-Dependency $manifest.vad

# The native CT2 library is taken from a pinned wheel as a ZIP archive. We extract
# only explicitly checksummed native DLLs and notices; no pip or Python is run.
$repositoryRoot = Split-Path -Parent $nativeRoot
function Copy-VerifiedCache([string]$Source, [string]$Destination, [string]$Expected) {
    if (-not (Test-Hash $Source $Expected)) { return $false }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    $partial = $Destination + '.partial'
    if (-not $partial.StartsWith($nativePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid cache destination' }
    # OpenRead follows Hugging Face symbolic links and copies the actual contents.
    $inputStream = [IO.File]::OpenRead($Source)
    try {
        $outputStream = [IO.File]::Create($partial)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
    if (-not (Test-Hash $partial $Expected)) { throw "Cache copy checksum mismatch: $Source" }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
    return $true
}

function Find-MsvcTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required to generate the CT2 import library.' }
    $paths = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC/Tools/MSVC/*/bin/Hostx64/x64/lib.exe')
    if ($LASTEXITCODE -ne 0 -or -not $paths.Count) { throw 'MSVC x64 tools were not found.' }
    return Split-Path -Parent ($paths | Sort-Object -Descending | Select-Object -First 1)
}

$ct2 = $manifest.ctranslate2
$headersValid = $true
foreach ($header in $ct2.headers) {
    if (-not (Test-Hash (Native-Path $header.path) $header.sha256)) { $headersValid = $false; break }
}
if (-not $headersValid) {
    Get-Dependency $ct2.source
    $sourceDirectory = Native-Path 'downloads/ctranslate2-source'
    Expand-Archive -LiteralPath (Native-Path $ct2.source.path) -DestinationPath $sourceDirectory -Force
    $includeSource = Join-Path $sourceDirectory ('CTranslate2-' + $ct2.version + '/include')
    New-Item -ItemType Directory -Force -Path (Native-Path 'vendor/ctranslate2') | Out-Null
    Copy-Item -LiteralPath $includeSource -Destination (Native-Path 'vendor/ctranslate2') -Recurse -Force
    foreach ($header in $ct2.headers) {
        if (-not (Test-Hash (Native-Path $header.path) $header.sha256)) { throw "CT2 header checksum mismatch: $($header.path)" }
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($package in $ct2.packages) {
    $missing = @()
    foreach ($item in $package.files) {
        $destination = Native-Path $item.path
        if (Test-Hash $destination $item.sha256) { continue }
        $fromEnvironment = Join-Path $repositoryRoot ('.venv/Lib/site-packages/' + $item.entry)
        $fromPackage = Join-Path $repositoryRoot ('native-install/' + [IO.Path]::GetFileName($item.path))
        if (Copy-VerifiedCache $fromEnvironment $destination $item.sha256) { continue }
        if (Copy-VerifiedCache $fromPackage $destination $item.sha256) { continue }
        $missing += $item
    }
    if (-not $missing.Count) { continue }
    Get-Dependency $package
    $archive = [IO.Compression.ZipFile]::OpenRead((Native-Path $package.path))
    try {
        foreach ($item in $missing) {
            $entry = $archive.GetEntry($item.entry)
            if (-not $entry) { throw "Expected native file is missing from wheel: $($item.entry)" }
            $destination = Native-Path $item.path
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            $partial = Native-Path ($item.path + '.partial')
            $inputStream = $entry.Open()
            try {
                $outputStream = [IO.File]::Create($partial)
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
            } finally { $inputStream.Dispose() }
            if (-not (Test-Hash $partial $item.sha256)) { throw "Extracted DLL checksum mismatch: $($item.entry)" }
            Move-Item -LiteralPath $partial -Destination $destination -Force
        }
    } finally { $archive.Dispose() }
}

$msvcTools = Find-MsvcTools
$importDirectory = Native-Path 'vendor/ctranslate2'
$dll = Native-Path 'runtime-ct2/ctranslate2.dll'
$exports = & (Join-Path $msvcTools 'dumpbin.exe') /exports $dll
if ($LASTEXITCODE -ne 0) { throw 'Could not read native CT2 exports.' }
$symbols = @($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[A-Fa-f0-9]+\s+[A-Fa-f0-9]+\s+(\S+)') { $Matches[1] } })
if ($symbols.Count -lt 100) { throw 'Unexpectedly few CT2 exports; refusing an invalid import library.' }
$definition = Join-Path $importDirectory 'ctranslate2.def'
[IO.File]::WriteAllLines($definition, @('LIBRARY ctranslate2.dll', 'EXPORTS') + $symbols, [Text.Encoding]::ASCII)
& (Join-Path $msvcTools 'lib.exe') /nologo /machine:x64 "/def:$definition" "/out:$importDirectory/ctranslate2.lib"
if ($LASTEXITCODE -ne 0) { throw 'Could not generate the CT2 C++ import library.' }

# CTranslate2 remains the application's default backend and supplies the tokenizer
# regression fixture, even when adding an optional whisper.cpp ASR model.
    $ct2Models = @($ct2.models | Where-Object name -EQ $Model)
    if ($ct2Models.Count -ne 1) { throw "No pinned CTranslate2 model exists for $Model" }
    $ct2Model = $ct2Models[0]
    $cacheRepo = 'models--' + $ct2Model.repo.Replace('/', '--')
    $hfCacheRoot = if ($env:HF_HUB_CACHE) { $env:HF_HUB_CACHE } elseif ($env:HF_HOME) { Join-Path $env:HF_HOME 'hub' } else { Join-Path $env:USERPROFILE '.cache/huggingface/hub' }
    foreach ($item in $ct2Model.files) {
        $destination = Native-Path $item.path
        if (Test-Hash $destination $item.sha256) { continue }
        $cached = Join-Path $hfCacheRoot ($cacheRepo + '/snapshots/' + $ct2Model.revision + '/' + $item.name)
        if (Copy-VerifiedCache $cached $destination $item.sha256) { continue }
        $bundled = Join-Path $repositoryRoot ('native-install/models/ctranslate2/' + $Model + '/' + $item.name)
        if (Copy-VerifiedCache $bundled $destination $item.sha256) { continue }
        Get-Dependency $item
    }
    Write-Host "Native CTranslate2 $($ct2.version) dependencies and $Model model verified. Python is not required."
    if ($Backend -eq 'ctranslate2') { return }

$modelName = if ($Precision -eq 'float16') { "ggml-$Model.bin" } else { "ggml-$Model-$Precision.bin" }
$modelEntry = @($manifest.models | Where-Object name -EQ $modelName)
if ($modelEntry.Count -eq 1) {
    Get-Dependency $modelEntry[0]
} else {
    # Some model/precision pairs are not published upstream. The quantizer is
    # covered by the pinned runtime archive and its own extracted SHA256.
    $sourceEntry = @($manifest.models | Where-Object name -EQ "ggml-$Model.bin")
    if ($sourceEntry.Count -ne 1) { throw "No pinned float16 source exists for $Model" }
    Get-Dependency $sourceEntry[0]
    $modelPath = Native-Path "models/$modelName"
    $provenancePath = Native-Path "models/$modelName.provenance.json"
    $quantizer = Native-Path 'runtime/Release/quantize.exe'
    $quantizerHash = (Get-FileHash -LiteralPath $quantizer -Algorithm SHA256).Hash
    $valid = $false
    if (Test-Path -LiteralPath $provenancePath) {
        try {
            $provenance = Get-Content -LiteralPath $provenancePath -Raw -Encoding UTF8 | ConvertFrom-Json
            $valid = $provenance.source_sha256 -eq $sourceEntry[0].sha256 -and
                $provenance.quantizer_sha256 -eq $quantizerHash -and $provenance.precision -eq $Precision -and
                (Test-Hash $modelPath $provenance.sha256)
        } catch { $valid = $false }
    }
    if (-not $valid) {
        $sourcePath = Native-Path $sourceEntry[0].path
        $partial = Native-Path "models/$modelName.partial"
        $arguments = '"' + $sourcePath + '" "' + $partial + '" ' + $Precision
        Write-Host "Quantizing $Model to $Precision (no Python)"
        $quantizerProcess = Start-Process -FilePath $quantizer -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Native-Path 'downloads/quantize-output.log') `
            -RedirectStandardError (Native-Path 'downloads/quantize-error.log')
        if (-not $quantizerProcess.WaitForExit(1800000)) {
            $quantizerProcess.Kill()
            $quantizerProcess.WaitForExit()
            throw 'Quantization exceeded 30 minutes. Partial output retained.'
        }
        $quantizerProcess.Refresh()
        if ($quantizerProcess.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $partial)) {
            throw 'Quantization failed; see native/downloads/quantize-error.log.'
        }
        $generatedHash = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
        Move-Item -LiteralPath $partial -Destination $modelPath -Force
        $provenance = [ordered]@{ source_sha256=$sourceEntry[0].sha256; quantizer_sha256=$quantizerHash; precision=$Precision; sha256=$generatedHash }
        [IO.File]::WriteAllText($provenancePath, ($provenance | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    }
}
Write-Host "Dependencies verified. Selected model: $modelName"
Write-Host 'Build with .\native\build.ps1 -SkipDownload using the same -Model and -Precision options.'
