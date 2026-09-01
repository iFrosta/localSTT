param(
    [Parameter(Mandatory=$true)]
    [string]$File,
    [string]$ResponseFormat = "json"
)
$ErrorActionPreference = "Stop"
curl.exe -X POST "http://127.0.0.1:7777/v1/audio/transcriptions" `
  -F "file=@$File" `
  -F "model=large-v3-turbo" `
  -F "language=ru" `
  -F "response_format=$ResponseFormat"
