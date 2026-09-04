' Starts the launcher with no console window. Everything is resolved relative to this
' file, so the folder LocalSTT lives in can be renamed or moved.
Set fso = CreateObject("Scripting.FileSystemObject")
appDir = fso.GetParentFolderName(WScript.ScriptFullName)
Set shell = CreateObject("WScript.Shell")
shell.CurrentDirectory = appDir
shell.Run """" & appDir & "\launch-localstt.cmd""", 0, False
