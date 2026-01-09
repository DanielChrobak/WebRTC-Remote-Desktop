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

if not exist "build\bin\Release\InputHelper.exe" (
    echo InputHelper.exe not found. Run build.bat first
    pause & exit /b 1
)

:: Install the input helper service if not already installed
sc query ScreenShareInput >nul 2>&1
if %errorlevel% neq 0 (
    echo Installing InputHelper service...
    build\bin\Release\InputHelper.exe --install
    if %errorlevel% neq 0 (
        echo Failed to install InputHelper service
        pause & exit /b 1
    )
    timeout /t 2 >nul
)

:: Ensure the service is running
sc query ScreenShareInput | find "RUNNING" >nul
if %errorlevel% neq 0 (
    echo Starting InputHelper service...
    net start ScreenShareInput >nul 2>&1
    if %errorlevel% neq 0 (
        :: Try to start it again after a brief pause
        timeout /t 1 >nul
        net start ScreenShareInput >nul 2>&1
    )
    timeout /t 1 >nul
)

:: Verify service is running
sc query ScreenShareInput | find "RUNNING" >nul
if %errorlevel% equ 0 (
    echo [OK] InputHelper service is running ^(UAC input support enabled^)
) else (
    echo [WARN] InputHelper service not running ^(UAC input may not work^)
)

echo.
echo Running ScreenShare as Administrator...
cd build\bin\Release && ScreenShare.exe
cd ..\..\..
pause
