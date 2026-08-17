@echo off
setlocal

powershell.exe -STA -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-OBSFlightControlsOverlay.ps1"
set "exitCode=%ERRORLEVEL%"

echo.
if not "%exitCode%"=="0" (
  echo Installation failed. Review the message above, then run this installer again.
) else (
  echo Installation completed. You can now start OBS Studio.
)
pause
exit /b %exitCode%
