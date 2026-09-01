$ErrorActionPreference = "Stop"
$startup = [Environment]::GetFolderPath("Startup")
$shortcut = Join-Path $startup "LocalSTT.lnk"
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($shortcut)
$lnk.TargetPath = "C:\Apps\LocalSTT\start-localstt.vbs"
$lnk.WorkingDirectory = "C:\Apps\LocalSTT"
$lnk.IconLocation = "C:\Apps\LocalSTT\start-localstt.vbs,0"
$lnk.Save()
"Created per-user autostart shortcut: $shortcut"
