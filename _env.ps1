# Dot-sourced by the other scripts. Resolves where LocalSTT is installed, where its
# native executables are and, for the Python rollback, which interpreter to run, so
# nothing has to hard-code an install folder.
$script:LocalSttRoot = Split-Path -Parent $PSCommandPath

function Get-LocalSttRoot {
    return $script:LocalSttRoot
}

function Get-LocalSttNativeDir {
    <#
        .SYNOPSIS
        The folder holding LocalSTT.exe and localstt-cli.exe.

        A source checkout builds into native-install\; an installed copy or the release
        archive keeps the executables next to this script.
    #>
    foreach ($dir in @((Join-Path $script:LocalSttRoot "native-install"), $script:LocalSttRoot)) {
        if (Test-Path (Join-Path $dir "LocalSTT.exe")) { return $dir }
    }
    throw "LocalSTT.exe not found in $script:LocalSttRoot. Run .\install.ps1 first."
}

function Get-LocalSttNative {
    <#
        .SYNOPSIS
        The tray application, or with -Console the console build of the same program.
    #>
    param([switch]$Console)
    $name = if ($Console) { "localstt-cli.exe" } else { "LocalSTT.exe" }
    return Join-Path (Get-LocalSttNativeDir) $name
}

function Invoke-LocalSttCli {
    <#
        .SYNOPSIS
        Runs localstt-cli.exe against this folder's commands.json and dictionary.json,
        and throws with -FailureMessage when it exits with an error.
    #>
    param([string[]]$Arguments = @(), [string]$FailureMessage = "localstt-cli.exe failed")
    & (Get-LocalSttNative -Console) --root (Get-LocalSttRoot) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$FailureMessage (exit code $LASTEXITCODE)" }
}

function Copy-LocalSttApp {
    <#
        .SYNOPSIS
        Copies the runnable application into a folder of its own.

        The executables and their runtime DLLs, one recognition model with the voice
        activity model, the licenses, the files a user edits and the scripts that run it.
        install.ps1 -InstallTo and build-release.ps1 both use this, so an installed copy
        and the release archive have the same layout. Files a user edits are only
        written when the destination has none yet, unless -ReplaceUserFiles is given.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$Model = "large-v3-turbo",
        [switch]$ReplaceUserFiles
    )
    $native = Get-LocalSttNativeDir
    $root = $script:LocalSttRoot
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null

    # Test binaries stay behind, and so do renamed leftovers such as *.alt.dll.
    Get-ChildItem -LiteralPath $native -File | Where-Object {
        ($_.Extension -eq ".exe" -and $_.Name -notlike "test_*") -or
        ($_.Extension -eq ".dll" -and $_.Name -notlike "*.alt.dll") -or
        ($_.Name -in @("dependencies.json", "localstt.ico"))
    } | Copy-Item -Destination $Destination -Force

    $modelDir = Join-Path $native "models\ctranslate2\$Model"
    if (-not (Test-Path (Join-Path $modelDir "model.bin"))) {
        throw "The $Model model is not in $modelDir. Build with native\build.ps1 -Model $Model first."
    }
    $copies = @(
        @{ From = $modelDir; To = (Join-Path $Destination "models\ctranslate2\$Model") },
        @{ From = (Join-Path $native "licenses"); To = (Join-Path $Destination "licenses") }
    )
    foreach ($copy in $copies) {
        # robocopy: 0-7 are its success codes, 8 and up are real failures.
        robocopy $copy.From $copy.To /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Copying $($copy.From) failed (robocopy $LASTEXITCODE)" }
    }
    $global:LASTEXITCODE = 0
    Get-ChildItem -LiteralPath (Join-Path $native "models") -Filter "ggml-silero-*.bin" -File |
        Copy-Item -Destination (Join-Path $Destination "models") -Force

    foreach ($name in @("commands.json", "dictionary.json", "cleanup-prompt.txt")) {
        $target = Join-Path $Destination $name
        if ($ReplaceUserFiles -or -not (Test-Path $target)) {
            Copy-Item -LiteralPath (Join-Path $root $name) -Destination $target -Force
        }
    }
    $scripts = @("_env.ps1", "start-localstt.vbs", "launch-localstt.cmd", "run-native.ps1", "run-dev.ps1",
                 "stop-localstt.ps1", "install-autostart.ps1", "uninstall-autostart.ps1", "uninstall.ps1",
                 "health.ps1", "transcribe-file.ps1", "benchmark.ps1", "run-diagnostics.ps1",
                 "install-qwen-cleanup-model.ps1", "set-ollama-cleanup-model.ps1",
                 "LICENSE", "README.md", "README.ru.md")
    foreach ($name in $scripts) {
        Copy-Item -LiteralPath (Join-Path $root $name) -Destination (Join-Path $Destination $name) -Force
    }
}

function Find-LocalSttPython {
    <#
        .SYNOPSIS
        A 64-bit CPython between 3.10 and 3.13, preferring the newest one installed.

        Being on PATH is not required. A Python installed for the current user only, or
        unpacked next to this script to keep the whole thing in one folder, is just as
        good and neither of those puts itself on PATH.
    #>
    # Objects, not nested arrays: PowerShell flattens @($exe, @("-3.12")) into two
    # loose strings, and the argument list stops being an argument list.
    $candidates = [System.Collections.Generic.List[object]]::new()

    function Add-Candidate($path, $prefix = @()) {
        if ($path -and (Test-Path $path)) {
            $candidates.Add([pscustomobject]@{ Exe = $path; Prefix = $prefix })
        }
    }

    # A Python kept inside the application folder wins: it is the one someone chose to
    # put there, and it travels with the folder.
    foreach ($dir in @("Python312", "Python313", "Python311", "Python310", "python")) {
        Add-Candidate (Join-Path $script:LocalSttRoot "$dir\python.exe")
    }

    $launcher = Get-Command "py.exe" -ErrorAction SilentlyContinue
    if ($launcher) {
        foreach ($version in @("3.12", "3.13", "3.11", "3.10")) {
            $candidates.Add([pscustomobject]@{ Exe = $launcher.Source; Prefix = @("-$version") })
        }
    }
    foreach ($name in @("python.exe", "python3.exe")) {
        $found = Get-Command $name -ErrorAction SilentlyContinue
        if ($found) { $candidates.Add([pscustomobject]@{ Exe = $found.Source; Prefix = @() }) }
    }

    # Where the python.org installer puts things. "Install for all users" was not ticked
    # and "Add to PATH" was not either -- both are off by default.
    foreach ($base in @("$env:LOCALAPPDATA\Programs\Python", $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $base -or -not (Test-Path $base)) { continue }
        Get-ChildItem -Path $base -Filter "Python3*" -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Add-Candidate (Join-Path $_.FullName "python.exe") }
    }

    # chr(80) is "P", written this way so no quote in the probe needs escaping.
    $probe = "import struct,sys; print(f'{sys.version_info.major}.{sys.version_info.minor} {struct.calcsize(chr(80))*8} {sys.executable}')"
    foreach ($candidate in $candidates) {
        $arguments = @($candidate.Prefix) + @("-c", $probe)
        # A candidate that is not installed makes the launcher fail; that is a normal
        # answer here, not an error worth stopping for.
        $output = $null
        try {
            $output = & $candidate.Exe @arguments 2>$null | Select-Object -Last 1
        } catch {
            continue
        }
        if ($LASTEXITCODE -ne 0 -or -not $output) { continue }

        $version, $bits, $path = "$output".Trim() -split " ", 3
        if ($bits -ne "64") { continue }
        try {
            $parsed = [version]$version
        } catch {
            continue
        }
        if ($parsed -lt [version]"3.10" -or $parsed -ge [version]"3.14") { continue }
        return [pscustomobject]@{ Path = $path; Version = $version }
    }
    return $null
}

function Get-LocalSttPython {
    <#
        .SYNOPSIS
        The venv interpreter for the Python application, or whatever python is on PATH.

        Only run-python-legacy.ps1 needs it on this branch: the native application runs
        without Python. The main branch's install.ps1 creates the venv.
    #>
    param([switch]$Windowed)

    $name = if ($Windowed) { "pythonw.exe" } else { "python.exe" }

    # What the main branch's install.ps1 builds.
    $venv = Join-Path $script:LocalSttRoot ".venv\Scripts\$name"
    if (Test-Path $venv) { return $venv }

    # What the release archive ships: a Python inside the folder with the packages
    # already in it, so there is nothing to install and no venv to create.
    $bundled = Join-Path $script:LocalSttRoot "python\$name"
    if (Test-Path $bundled) { return $bundled }

    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "No Python found. Expected $venv or $bundled. The Python application is installed by install.ps1 on the main branch."
}

function Get-LocalSttPort {
    <#
        .SYNOPSIS
        The API port from config.json, so the helper scripts follow a changed setting.
    #>
    $config = Join-Path $env:APPDATA "LocalSTT\config.json"
    if (Test-Path $config) {
        try {
            $port = (Get-Content $config -Raw | ConvertFrom-Json).api_port
            if ($port) { return [int]$port }
        } catch {
            # A hand-edited config.json that no longer parses should not stop a script
            # whose real job is something else.
        }
    }
    return 7777
}
