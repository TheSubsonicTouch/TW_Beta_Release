@echo off
rem Pick one of the curated outfits for your enforcer, then start the game with
rem Play-TriadWars.bat. Run this with the game and the local server closed: the
rem server holds your profile in memory while it runs.
rem See LocalServer\README.md ("The outfit lookbook").
setlocal
set "ROOT=%~dp0"

where python >nul 2>nul
if errorlevel 1 (
    echo Python was not found on PATH. Install Python from python.org, tick
    echo "Add python.exe to PATH", then try again.
    echo.
    pause
    exit /b 1
)

python "%ROOT%LocalServer\tools\playermodel.py" menu
echo.
pause
endlocal
