$ErrorActionPreference = "Stop"
$ollama = "$env:LOCALAPPDATA\Programs\Ollama\ollama.exe"
if (-not (Test-Path $ollama)) {
    $ollama = "ollama"
}

$model = "qwen3:4b-instruct"
& $ollama pull $model
& "C:\Apps\LocalSTT\set-ollama-cleanup-model.ps1" -Model $model -TimeoutSeconds 60
"Restart LocalSTT after installing the cleanup model."
