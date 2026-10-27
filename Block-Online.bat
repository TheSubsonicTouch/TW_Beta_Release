@echo off
rem Keep the game on this computer.
rem
rem Triad Wars has two hosts baked into the executable that ignore UserOptions.dat: its
rem telemetry (prod.services.triadwars.com) and three challenge images (tw.cdn.sqexeu.com).
rem Both shut down with the game, but the client still asks them as it starts and waits on
rem the answers. This points both at 127.0.0.1, where the local server already answers
rem them - so there is no DNS lookup, no dead connection and nothing leaving the machine.
rem
rem Run it once (it asks for Administrator). Restore-Online.bat undoes it; the hosts file
rem is backed up first. This changes nothing else on the computer.
setlocal
set "ROOT=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%LocalServer\hosts.ps1" block
echo.
pause
endlocal
