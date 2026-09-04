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
