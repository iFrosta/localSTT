# Dot-sourced by the other scripts. Resolves where LocalSTT is installed and which
# interpreter to run it with, so nothing has to hard-code an install folder.
$script:LocalSttRoot = Split-Path -Parent $PSCommandPath

function Get-LocalSttRoot {
    return $script:LocalSttRoot
}

function Get-LocalSttPython {
    <#
        .SYNOPSIS
        The venv interpreter created by install.ps1, or whatever python is on PATH.
    #>
    param([switch]$Windowed)

    $name = if ($Windowed) { "pythonw.exe" } else { "python.exe" }
    $venv = Join-Path $script:LocalSttRoot ".venv\Scripts\$name"
    if (Test-Path $venv) { return $venv }

    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "No Python found. Expected $venv -- run install.ps1 first."
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
