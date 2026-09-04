# Dot-sourced by the other scripts. Resolves where LocalSTT is installed and which
# interpreter to run it with, so nothing has to hard-code an install folder.
$script:LocalSttRoot = Split-Path -Parent $PSCommandPath

function Get-LocalSttRoot {
    return $script:LocalSttRoot
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
        The venv interpreter created by install.ps1, or whatever python is on PATH.
    #>
    param([switch]$Windowed)

    $name = if ($Windowed) { "pythonw.exe" } else { "python.exe" }

    # What install.ps1 builds.
    $venv = Join-Path $script:LocalSttRoot ".venv\Scripts\$name"
    if (Test-Path $venv) { return $venv }

    # What the release archive ships: a Python inside the folder with the packages
    # already in it, so there is nothing to install and no venv to create.
    $bundled = Join-Path $script:LocalSttRoot "python\$name"
    if (Test-Path $bundled) { return $bundled }

    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "No Python found. Expected $venv or $bundled -- run install.ps1 first."
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
