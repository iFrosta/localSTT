# Stops every LocalSTT process started from this folder.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "_env.ps1")

$root = Get-LocalSttRoot
Get-CimInstance Win32_Process |
    Where-Object {
        ($_.CommandLine -like "*localstt.main*" -or $_.CommandLine -like "*$root*") -and
        ($_.Name -in @("python.exe", "pythonw.exe"))
    } |
    Sort-Object ProcessId -Unique |
    ForEach-Object {
        try {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop
            "Stopped LocalSTT process $($_.ProcessId)"
        } catch {
            "LocalSTT process $($_.ProcessId) was already stopped"
        }
    }
