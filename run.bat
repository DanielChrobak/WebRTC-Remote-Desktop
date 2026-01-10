@echo off
setlocal

REM SlipStream - Run Script
REM Copyright 2025-2026 Daniel Chrobak

REM Check for administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

REM Running with admin rights
cd /d "%~dp0"

if not exist "build\bin\Release\SlipStream.exe" (
    echo Run build.bat first
    pause & exit /b 1
)

echo Running as Administrator...
cd build\bin\Release && SlipStream.exe
cd ..\..\..
pause
