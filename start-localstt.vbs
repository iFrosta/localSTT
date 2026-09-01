Set shell = CreateObject("WScript.Shell")
shell.CurrentDirectory = "C:\Apps\LocalSTT"
shell.Run """C:\Apps\LocalSTT.venv\Scripts\pythonw.exe"" -m localstt.main", 0, False
