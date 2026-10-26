@echo off
rem Starts the local Triad Wars service emulator (if it is not already running)
rem and then the game. See LocalServer\README.md.
setlocal
set "ROOT=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%LocalServer\start.ps1"
if errorlevel 1 (
    echo.
    echo The local server could not be started. See the message above.
    pause
    exit /b 1
)

start "" /D "%ROOT%" "%ROOT%TriadWars.exe"
endlocal
