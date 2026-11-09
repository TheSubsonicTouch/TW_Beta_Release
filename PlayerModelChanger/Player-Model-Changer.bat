@echo off
setlocal
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo Python with Tkinter is required. Install Python from python.org and enable Add to PATH.
  pause
  exit /b 1
)
python "%~dp0model_manager.py"
if errorlevel 1 (
  echo The changer could not start. Check the error above and the README.
  pause
)
