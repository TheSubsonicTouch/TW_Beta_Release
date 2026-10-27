@echo off
rem Undo Block-Online.bat: put the hosts file back the way it was.
setlocal
set "ROOT=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%LocalServer\hosts.ps1" restore
echo.
pause
endlocal
