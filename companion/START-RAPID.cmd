@echo off
setlocal DisableDelayedExpansion
cd /d "%~dp0"
if not exist "%~dp0rapid-telemetry-daemon.exe" goto missing_exe
if not exist "%~dp0daemon.conf" goto missing_config
if not exist "%~dp0telemetry.key" goto missing_key
if not exist "%~dp0recordings\" mkdir "%~dp0recordings" 2>nul
if not exist "%~dp0recordings\" goto not_writable
set "RAPID_TELEMETRY_KEY="
start "" "%~dp0rapid-telemetry-daemon.exe" --config "%~dp0daemon.conf" --auth-key-file "%~dp0telemetry.key"
exit /b 0

:missing_exe
echo The native raPId executable is missing. Extract the complete companion folder.
goto failed
:missing_config
echo daemon.conf is missing. Copy it from your paired demo package.
goto failed
:missing_key
echo telemetry.key is missing. Copy the private key paired with your Pi.
goto failed
:not_writable
echo This folder is not writable. Extract the package into a folder you can write to.
:failed
echo See AC1_DEMO_GUIDE.md for setup and recovery instructions.
pause
exit /b 1
