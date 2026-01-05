@echo off
setlocal

:: Check for admin rights
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Running as admin now
cd /d "%~dp0"

if not exist "build\bin\Release\ScreenShare.exe" (
    echo Run build.bat first
    pause & exit /b 1
)

echo Running as Administrator...
cd build\bin\Release && ScreenShare.exe
cd ..\..\..
pause
