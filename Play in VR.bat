@echo off
rem Resident Evil Survivor VR: start your VR runtime (SteamVR or the Meta app) first, with the headset and both controllers on
cd /d "%~dp0"
set EXE=resurvivor.exe
if not exist "%EXE%" set EXE=build_release\Release\resurvivor.exe
if not exist "%EXE%" (
    echo resurvivor.exe not found
    pause
    exit /b 1
)
start "" "%EXE%" --vr
