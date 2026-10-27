@echo off
rem Puts back the menu DLL that was there before the free camera was installed.
setlocal
pushd "%~dp0"

echo.
if not exist "dinput8.original.dll" (
    echo   There is no dinput8.original.dll here, so there is nothing to put back.
    echo   To remove the menu entirely, delete dinput8.dll from this folder.
    echo.
    pause
    popd
    exit /b 1
)

copy /y "dinput8.original.dll" "dinput8.dll" >nul
if errorlevel 1 (
    echo   Could not write dinput8.dll. Close the game and try again.
    echo.
    pause
    popd
    exit /b 1
)

echo   Your original menu DLL is back in place. The free camera is gone.
echo.
pause
popd
endlocal
