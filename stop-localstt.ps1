# Stops every LocalSTT process started from this folder: the native executables and,
# for the Python rollback, an interpreter running localstt.main.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "_env.ps1")

$root = Get-LocalSttRoot
$native = @()
try {
    $native = @((Get-LocalSttNative), (Get-LocalSttNative -Console))
} catch {
    # Nothing built here yet; a Python instance may still be running.
}
Get-CimInstance Win32_Process |
    Where-Object {
        ($_.ExecutablePath -and $native -contains $_.ExecutablePath) -or
        (($_.Name -in @("python.exe", "pythonw.exe")) -and
         ($_.CommandLine -like "*localstt.main*" -or $_.CommandLine -like "*$root*"))
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
