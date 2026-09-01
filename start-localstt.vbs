Set shell = CreateObject("WScript.Shell")
shell.CurrentDirectory = "C:\Apps\LocalSTT"
shell.Run """C:\Apps\LocalSTT\launch-localstt.cmd""", 0, False
