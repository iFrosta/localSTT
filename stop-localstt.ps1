$ErrorActionPreference = "Continue"
Get-CimInstance Win32_Process |
    Where-Object {
        ($_.CommandLine -like "*localstt.main*" -or $_.CommandLine -like "*C:\Apps\LocalSTT*") -and
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
