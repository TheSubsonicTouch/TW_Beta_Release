@echo off
rem Triad Wars - Free Camera installer.
rem Keeps the menu DLL that is already installed as dinput8.original.dll, then puts
rem the menu with the Free Camera entry in its place.
setlocal
pushd "%~dp0"

echo.
echo   Triad Wars - Free Camera
echo   ------------------------
echo.

if not exist "TriadWars.exe" (
    echo   TriadWars.exe is not in this folder:
    echo.
    echo       %CD%
    echo.
    echo   Put this installer in your game folder - the one that holds
    echo   TriadWars.exe and Play-TriadWars.bat - and run it again.
    echo.
    pause
    popd
    exit /b 1
)

if not exist "FreeCam\dinput8.dll" (
    echo   The FreeCam folder is missing. Extract the whole zip, folder included,
    echo   then run this file from the game folder.
    echo.
    pause
    popd
    exit /b 1
)

if exist "dinput8.dll" (
    if not exist "dinput8.original.dll" (
        copy /y "dinput8.dll" "dinput8.original.dll" >nul
        echo   Kept your current menu DLL as dinput8.original.dll
    ) else (
        echo   dinput8.original.dll is already there, leaving it alone
    )
)

copy /y "FreeCam\dinput8.dll" "dinput8.dll" >nul
if errorlevel 1 (
    echo.
    echo   Could not write dinput8.dll. If the game is running, close it and
    echo   run this again.
    echo.
    pause
    popd
    exit /b 1
)

echo   Installed.
echo.
echo   Next:
echo     1. Start the game with Play-TriadWars.bat
echo     2. Load into the world - not the title or loading screens
echo     3. Press Insert to open the menu
echo     4. Arrow down to "Free Camera" and press Enter
echo.
echo   While flying:
echo     W A S D  move      Space / C  up / down
echo     hold right mouse   look around (natural, not inverted)
echo     Shift  faster      Ctrl       slower
echo     P      follow the player    L  record a keyframe    K  play them back
echo     F10    put the camera back and switch off
echo.
echo   To undo this, run Uninstall-FreeCam.bat.
echo.
pause
popd
endlocal
