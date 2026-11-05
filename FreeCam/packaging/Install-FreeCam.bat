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

if not exist "FreeCam\menu-plugin.dll" (
    echo   The FreeCam folder is missing. Extract the whole zip, folder included,
    echo   then run this file from the game folder.
    echo.
    pause
    popd
    exit /b 1
)

rem Windows marks files that came out of a downloaded archive, and warns the first time
rem such a DLL is loaded. Clearing that mark here is the same as ticking "Unblock" in the
rem file's properties, and saves you doing it by hand for each file.
powershell -NoProfile -Command "try { Get-ChildItem -LiteralPath '%~dp0' -Recurse -File | Unblock-File } catch { }" >nul 2>&1

if exist "dinput8.dll" (
    if not exist "dinput8.original.dll" (
        copy /y "dinput8.dll" "dinput8.original.dll" >nul
        echo   Kept your current menu DLL as dinput8.original.dll
    ) else (
        echo   dinput8.original.dll is already there, leaving it alone
    )
)

rem menu-plugin.dll is the same DLL under a name that is not "dinput8" until it is in
rem place: archives holding a file called dinput8.dll are the shape scanners look for.
copy /y "FreeCam\menu-plugin.dll" "dinput8.dll" >nul
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
echo     L      record a keyframe    J  play them back    M  clear them
echo     left/right arrows  how fast the playback runs (0.1x - 4x)
echo     O      freeze the camera and let the player move; O again to fly
echo     P      follow the player (frozen: pan to keep them in frame)
echo     F10    put the camera back and switch off
echo     B      spawn an NPC 3 m in front    N  next model    H  hostile/friendly
echo     (the same three choices are in the menu: NPC model, attitude, spawn)
echo.
echo   To undo this, run Uninstall-FreeCam.bat.
echo.
pause
popd
endlocal
