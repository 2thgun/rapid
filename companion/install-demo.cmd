@echo off
setlocal DisableDelayedExpansion
set "DESTINATION=%LOCALAPPDATA%\raPId AC1 Demo"

if not exist "%DESTINATION%\" mkdir "%DESTINATION%" 2>nul
if not exist "%DESTINATION%\" (
  echo Could not create %DESTINATION%
  pause
  exit /b 1
)

rem Current packages ship wiki guides; older prepared kits ship one demo guide.
set "GUIDES=GETTING_STARTED.md WINDOWS_COMPANION.md TOUCHSCREEN.md DRIVING_AND_RECORDING.md TROUBLESHOOTING.md"
if not exist "%~dp0GETTING_STARTED.md" set "GUIDES=AC1_DEMO_GUIDE.md"
for %%F in (rapid-telemetry-daemon.exe daemon.conf telemetry.key start-rapid-daemon.vbs START-RAPID.cmd START-HERE.txt %GUIDES%) do (
  copy /y "%~dp0%%F" "%DESTINATION%\%%F" >nul
  if errorlevel 1 (
    echo Could not install %%F
    pause
    exit /b 1
  )
)
if not exist "%DESTINATION%\recordings\" mkdir "%DESTINATION%\recordings" 2>nul
echo raPId AC1 Demo installed in:
echo %DESTINATION%
echo Open START-RAPID.cmd from that folder when the PC is connected to the rapid Wi-Fi network.
start "" explorer.exe "%DESTINATION%"
exit /b 0
