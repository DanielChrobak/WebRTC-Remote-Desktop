@echo off
setlocal

REM WebRTC Remote Desktop - Build Script
REM Copyright 2025-2026 Daniel Chrobak

REM Find vcpkg installation
set VCPKG=
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set VCPKG=%%~p& goto :found
)
echo vcpkg not found. Set VCPKG_ROOT or install to C:\vcpkg
pause & exit /b 1

:found
echo [1/3] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows >nul 2>&1
if errorlevel 1 (
    echo Failed. Run with verbose output:
    echo   "%VCPKG%\vcpkg.exe" install --triplet x64-windows
    pause & exit /b 1
)

echo [2/3] Configuring...
if exist build rmdir /s /q build >nul 2>&1
mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" --log-level=ERROR >nul 2>&1
if errorlevel 1 (
    echo CMake failed. Run manually for details.
    cd .. & pause & exit /b 1
)

echo [3/3] Building...
cmake --build . --config Release -- /v:minimal /nologo
if errorlevel 1 (
    echo Build failed.
    cd .. & pause & exit /b 1
)

cd ..
echo.
echo Done: build\bin\Release\WebRTCRemoteDesktop.exe
pause
