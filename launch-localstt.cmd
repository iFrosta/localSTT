@echo off
setlocal

rem %~dp0 is this file's folder, with a trailing backslash that has to come off.
set "APP_DIR=%~dp0"
if "%APP_DIR:~-1%"=="\" set "APP_DIR=%APP_DIR:~0,-1%"

rem A source checkout builds into native-install; an installed copy or the release
rem archive keeps the executable next to this file.
set "EXE=%APP_DIR%\native-install\LocalSTT.exe"
if not exist "%EXE%" set "EXE=%APP_DIR%\LocalSTT.exe"

set "LOG_DIR=%APPDATA%\LocalSTT\logs"
set "BOOTSTRAP_LOG=%LOG_DIR%\localstt-bootstrap.log"

if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

echo.>> "%BOOTSTRAP_LOG%"
echo ===== %DATE% %TIME% LocalSTT launcher =====>> "%BOOTSTRAP_LOG%"
echo APP_DIR=%APP_DIR%>> "%BOOTSTRAP_LOG%"
echo EXE=%EXE%>> "%BOOTSTRAP_LOG%"

if not exist "%EXE%" (
  echo LocalSTT.exe not found. Run install.ps1 first.>> "%BOOTSTRAP_LOG%"
  echo LocalSTT.exe not found. Run install.ps1 first.
  exit /b 1
)

rem The app writes its own log to %LOG_DIR%\localstt-native.log; start returns at once.
start "" /d "%APP_DIR%" "%EXE%" --root "%APP_DIR%"
