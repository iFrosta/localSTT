# Sends one audio file to the running app through its OpenAI-compatible endpoint.
param(
    [Parameter(Mandatory=$true)]
    [string]$File,
    [string]$Model = "large-v3-turbo",
    [string]$Language = "ru",
    [string]$ResponseFormat = "json",
    [int]$Port = 0
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

if ($Port -le 0) { $Port = Get-LocalSttPort }

curl.exe -X POST "http://127.0.0.1:$Port/v1/audio/transcriptions" `
  -F "file=@$File" `
  -F "model=$Model" `
  -F "language=$Language" `
  -F "response_format=$ResponseFormat"
