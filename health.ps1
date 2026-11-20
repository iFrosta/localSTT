# Asks the running app how it is doing.
param([int]$Port = 0)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

if ($Port -le 0) { $Port = Get-LocalSttPort }
$base = "http://127.0.0.1:$Port"

try {
    $health = Invoke-RestMethod -Uri "$base/health" -TimeoutSec 5
} catch {
    # 503 is the app answering while the model is still loading.
    $response = $_.Exception.Response
    if ($response -and [int]$response.StatusCode -eq 503) {
        "LocalSTT is running and still loading the model."
        return
    }
    throw "Nothing answers on $base. Is LocalSTT running? Start it with start-localstt.vbs, or run run-diagnostics.ps1 for a self-test without the app."
}
"===== /health ====="
$health | ConvertTo-Json -Depth 8
"===== /metrics ====="
(Invoke-RestMethod -Uri "$base/metrics" -TimeoutSec 5) | ConvertTo-Json -Depth 8
