@echo off
rem Same as Play-TriadWars.bat, but starts the game through FreeCam-Inject.exe so the
rem free camera DLL is loaded into it.
setlocal
set "ROOT=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%LocalServer\start.ps1"
if errorlevel 1 (
    echo.
    echo The local server could not be started. See the message above.
    pause
    exit /b 1
)

"%ROOT%FreeCam-Inject.exe"
if errorlevel 1 (
    echo.
    echo The free camera could not be loaded. See the message above.
    pause
    exit /b 1
)

endlocal
