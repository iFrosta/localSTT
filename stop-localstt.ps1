$ErrorActionPreference = "Continue"
Get-CimInstance Win32_Process |
    Where-Object {
        ($_.CommandLine -like "*localstt.main*" -or $_.CommandLine -like "*C:\Apps\LocalSTT*") -and
        ($_.Name -in @("python.exe", "pythonw.exe"))
    } |
    ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force
        "Stopped LocalSTT process $($_.ProcessId)"
    }
