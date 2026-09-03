# Overrides the cleanup model explicitly. Without -Pull the model has to be installed
# already; the script reports what fits on this GPU either way.
param(
    [Parameter(Mandatory=$true)]
    [string]$Model,
    [double]$TimeoutSeconds = 60,
    [switch]$Pull
)
$ErrorActionPreference = "Stop"
$python = "C:\Apps\LocalSTT\.venv\Scripts\python.exe"
if (-not (Test-Path $python)) {
    throw "LocalSTT venv not found: $python"
}
Set-Location "C:\Apps\LocalSTT"

$arguments = @("-m", "localstt.cleanup_model", "--model", $Model, "--apply", "--timeout", $TimeoutSeconds)
if ($Pull) { $arguments += "--pull" }
& $python @arguments
