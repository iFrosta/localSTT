# Times the model on real recordings and appends a row per file to a CSV.
param(
    [Parameter(Mandatory=$true, ValueFromRemainingArguments=$true)]
    [string[]]$WavFiles,
    [string]$Output
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

if (-not $Output) {
    $Output = Join-Path (Get-LocalSttRoot) "diagnostics\benchmark-results.csv"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null

Set-Location (Get-LocalSttRoot)
& (Get-LocalSttPython) benchmark.py @WavFiles --output $Output
