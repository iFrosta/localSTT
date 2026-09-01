param(
    [Parameter(Mandatory=$true, ValueFromRemainingArguments=$true)]
    [string[]]$WavFiles,
    [string]$Output = "C:\Apps\LocalSTT\diagnostics\benchmark-results.csv"
)
$ErrorActionPreference = "Stop"
$root = "C:\Apps\LocalSTT"
$python = "C:\Apps\LocalSTT.venv\Scripts\python.exe"
Set-Location $root
& $python benchmark.py @WavFiles --output $Output
