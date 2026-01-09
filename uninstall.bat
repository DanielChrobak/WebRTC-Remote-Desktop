@echo off
setlocal

:: Check for admin rights
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

echo ==========================================
echo  Uninstalling ScreenShare Input Helper
echo ==========================================
echo.

:: Check if service exists
sc query ScreenShareInput >nul 2>&1
if %errorlevel% neq 0 (
    echo InputHelper service is not installed.
    goto :cleanup_process
)

:: Try to stop the service with a timeout
echo Stopping InputHelper service...
sc stop ScreenShareInput >nul 2>&1

:: Wait up to 5 seconds for service to stop
set /a count=0
:wait_stop
timeout /t 1 /nobreak >nul
sc query ScreenShareInput | find "STOPPED" >nul 2>&1
if %errorlevel% equ 0 goto :service_stopped
set /a count+=1
if %count% lss 5 goto :wait_stop

echo Service did not stop gracefully, forcing termination...

:cleanup_process
:: Force kill any running InputHelper.exe processes
echo Terminating InputHelper.exe processes...
taskkill /F /IM InputHelper.exe >nul 2>&1
taskkill /F /IM ScreenShare.exe >nul 2>&1
timeout /t 1 /nobreak >nul

:service_stopped
:: Delete the service
echo Removing InputHelper service...
sc delete ScreenShareInput >nul 2>&1

:: Wait a moment for deletion to complete
timeout /t 1 /nobreak >nul

:: Verify removal
sc query ScreenShareInput >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo [OK] InputHelper service has been uninstalled.
) else (
    echo.
    echo [WARN] Service may still exist. Try rebooting and running again.
)

:done
echo.
pause
