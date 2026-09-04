# Asks the running app how it is doing.
param([int]$Port = 0)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

if ($Port -le 0) { $Port = Get-LocalSttPort }
$base = "http://127.0.0.1:$Port"

"===== /health ====="
(Invoke-RestMethod -Uri "$base/health" -TimeoutSec 5) | ConvertTo-Json -Depth 8
"===== /metrics ====="
(Invoke-RestMethod -Uri "$base/metrics" -TimeoutSec 5) | ConvertTo-Json -Depth 8
