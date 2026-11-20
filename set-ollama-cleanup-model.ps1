# Overrides the cleanup model explicitly. Without -Pull the model has to be installed
# already; the script reports what fits on this GPU either way.
param(
    [Parameter(Mandatory=$true)]
    [string]$Model,
    [double]$TimeoutSeconds = 60,
    [switch]$Pull
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

$arguments = @("--cleanup-model", $Model, "--apply", "--timeout", $TimeoutSeconds)
if ($Pull) { $arguments += "--pull" }
Invoke-LocalSttCli -Arguments $arguments -FailureMessage "Cleanup model configuration failed"
