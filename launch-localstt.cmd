@echo off
setlocal

rem %~dp0 is this file's folder, with a trailing backslash that has to come off.
set "APP_DIR=%~dp0"
if "%APP_DIR:~-1%"=="\" set "APP_DIR=%APP_DIR:~0,-1%"

rem What install.ps1 builds, then what the release archive ships, then the system one.
set "PYTHON=%APP_DIR%\.venv\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=%APP_DIR%\python\python.exe"
if not exist "%PYTHON%" set "PYTHON=python.exe"

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

"%PYTHON%" -m localstt.main >> "%BOOTSTRAP_LOG%" 2>&1
if errorlevel 1 (
  echo LocalSTT exited with failure>> "%BOOTSTRAP_LOG%"
  exit /b 1
)
echo LocalSTT exited normally>> "%BOOTSTRAP_LOG%"
