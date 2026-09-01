param(
    [Parameter(Mandatory=$true)]
    [string]$Model,
    [double]$TimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
$configDir = Join-Path $env:APPDATA "LocalSTT"
$configPath = Join-Path $configDir "config.json"
New-Item -ItemType Directory -Force -Path $configDir | Out-Null

if (Test-Path $configPath) {
    $config = Get-Content $configPath -Raw | ConvertFrom-Json
} else {
    $config = [pscustomobject]@{}
}

$config | Add-Member -NotePropertyName "ollama_model" -NotePropertyValue $Model -Force
$config | Add-Member -NotePropertyName "ollama_timeout_seconds" -NotePropertyValue $TimeoutSeconds -Force
$config | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 $configPath
"Configured LocalSTT Ollama cleanup model: $Model"
"Config: $configPath"
