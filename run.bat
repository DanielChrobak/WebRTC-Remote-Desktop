@echo off
if not exist "build\bin\Release\ScreenShare.exe" (
    echo Run build.bat first
    pause & exit /b 1
)
cd build\bin\Release && ScreenShare.exe
cd ..\..\.. && pause
