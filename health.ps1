$ErrorActionPreference = "Stop"
$health = Invoke-RestMethod -Uri "http://127.0.0.1:7777/health" -TimeoutSec 5
$metrics = Invoke-RestMethod -Uri "http://127.0.0.1:7777/metrics" -TimeoutSec 5
"===== /health ====="
$health | ConvertTo-Json -Depth 8
"===== /metrics ====="
$metrics | ConvertTo-Json -Depth 8
