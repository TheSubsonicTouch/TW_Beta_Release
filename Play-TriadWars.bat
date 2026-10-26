@echo off
rem Starts the local server, then the game.
rem
rem   Play-TriadWars.bat            normal launch
rem   Play-TriadWars.bat safe       same, with the new screens switched off
rem   Play-TriadWars.bat diagnose   print a setup report, start nothing
rem
rem If the game closes during launch, this window stays open and
rem LocalServer\last-launch.txt says what happened. See LocalServer\README.md.
setlocal
set "ROOT=%~dp0"

set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY (
    where py >nul 2>nul && set "PY=py -3"
)
if not defined PY (
    echo.
    echo Python 3.9 or newer was not found on PATH.
    echo Install it from https://www.python.org/downloads/ and tick
    echo "Add python.exe to PATH" on the first screen, then run this again.
    echo.
    pause
    exit /b 1
)

%PY% "%ROOT%LocalServer\launch.py" --root "%ROOT%" %*
if errorlevel 1 (
    echo.
    echo The game or the server stopped early. The lines above say why;
    echo %ROOT%LocalServer\last-launch.txt has the full report.
    echo Send that file if you want help with it.
    echo.
    pause
)
endlocal
