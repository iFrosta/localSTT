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

$rows = foreach ($file in $WavFiles) {
    $data = Invoke-LocalSttCli -Arguments @("--transcribe", $file) -FailureMessage "Transcription failed: $file" | ConvertFrom-Json
    [pscustomobject]@{
        file            = $file
        duration        = $data.duration
        processing_time = $data.processing_time
        realtime_factor = $(if ($data.duration) { $data.processing_time / $data.duration } else { 0 })
        backend         = $data.backend
        model           = $data.model
        compute_type    = $data.compute_type
        text            = $data.text
    }
}
$rows | Export-Csv -LiteralPath $Output -NoTypeInformation -Encoding UTF8 -Append
$rows
