$ErrorActionPreference = "Stop"
$root = "C:\Apps\LocalSTT"
$python = "C:\Apps\LocalSTT.venv\Scripts\python.exe"
Set-Location $root
& $python diagnostics.py
