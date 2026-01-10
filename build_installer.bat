@echo off
setlocal

REM SlipStream - Installer Build Script
REM Copyright 2025-2026 Daniel Chrobak

REM Find vcpkg installation
set VCPKG=
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set VCPKG=%%~p& goto :found
)
echo vcpkg not found. Set VCPKG_ROOT or install to C:\vcpkg
pause & exit /b 1

:found
echo ============================================
echo          SlipStream Installer Builder
echo ============================================
echo.

REM Check for NSIS
set USE_NSIS=0
where makensis >nul 2>&1 && set USE_NSIS=1
if exist "C:\Program Files (x86)\NSIS\makensis.exe" set USE_NSIS=1
if exist "C:\Program Files\NSIS\makensis.exe" set USE_NSIS=1

if %USE_NSIS%==1 (
    echo [*] NSIS found - will create installer .exe
) else (
    echo [*] NSIS not found - will create portable .zip
    echo     Install NSIS for a proper installer: https://nsis.sourceforge.io
)
echo.

echo [1/4] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows >nul 2>&1
if errorlevel 1 (
    echo Failed. Run: "%VCPKG%\vcpkg.exe" install --triplet x64-windows
    pause & exit /b 1
)

echo [2/4] Configuring...
if exist build rmdir /s /q build >nul 2>&1
mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake" --log-level=ERROR >nul 2>&1
if errorlevel 1 (
    echo CMake failed.
    cd .. & pause & exit /b 1
)

echo [3/4] Building Release...
cmake --build . --config Release -- /v:minimal /nologo
if errorlevel 1 (
    echo Build failed.
    cd .. & pause & exit /b 1
)

echo [4/4] Creating package...
if %USE_NSIS%==1 (
    cpack -G NSIS -C Release
) else (
    cpack -G ZIP -C Release
)
if errorlevel 1 (
    echo Packaging failed. Trying ZIP fallback...
    cpack -G ZIP -C Release
)

cd ..
echo.
echo ============================================
echo                    Done!
echo ============================================
echo.
if exist build\SlipStream-1.0.0-win64.exe (
    echo Installer: build\SlipStream-1.0.0-win64.exe
) else if exist build\SlipStream-1.0.0-win64.zip (
    echo Package:   build\SlipStream-1.0.0-win64.zip
) else (
    echo Package created in: build\
    dir /b build\SlipStream-* 2>nul
)
echo.
pause
