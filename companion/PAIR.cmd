@echo off
setlocal DisableDelayedExpansion
cd /d "%~dp0"
if not exist "%~dp0rapid-telemetry-daemon.exe" goto missing_exe
echo raPId companion pairing
echo.
echo Type the Pi's address and the TLS fingerprint shown on its own display.
echo Both are shown on the Pi's setup page and on the Pi's physical display.
echo.
rapid-telemetry-daemon.exe --pair
if errorlevel 1 (
  echo.
  echo Pairing did not complete. Check the address and the fingerprint and try again.
  pause
)
exit /b %errorlevel%

:missing_exe
echo The native raPId executable is missing. Extract the complete companion folder.
pause
exit /b 1
