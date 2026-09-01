@echo off
setlocal
set "APP_DIR=C:\Apps\LocalSTT"
set "PYTHON=C:\Apps\LocalSTT.venv\Scripts\python.exe"
set "LOG_DIR=%APPDATA%\LocalSTT\logs"
set "BOOTSTRAP_LOG=%LOG_DIR%\localstt-bootstrap.log"

if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

echo.>> "%BOOTSTRAP_LOG%"
echo ===== %DATE% %TIME% LocalSTT launcher =====>> "%BOOTSTRAP_LOG%"
echo APP_DIR=%APP_DIR%>> "%BOOTSTRAP_LOG%"
echo PYTHON=%PYTHON%>> "%BOOTSTRAP_LOG%"

cd /d "%APP_DIR%" || (
  echo failed to cd to %APP_DIR%>> "%BOOTSTRAP_LOG%"
  exit /b 1
)

if not exist "%PYTHON%" (
  echo python not found: %PYTHON%>> "%BOOTSTRAP_LOG%"
  exit /b 1
)

"%PYTHON%" -m localstt.main >> "%BOOTSTRAP_LOG%" 2>&1
if errorlevel 1 (
  echo LocalSTT exited with failure>> "%BOOTSTRAP_LOG%"
  exit /b 1
)
echo LocalSTT exited normally>> "%BOOTSTRAP_LOG%"
