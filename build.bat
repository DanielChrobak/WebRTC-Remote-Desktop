@echo off
setlocal

:: Change to the script's directory (handles paths with spaces)
cd /d "%~dp0"

:: Find vcpkg
set VCPKG=
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set VCPKG=%%~p& goto :found
)
echo vcpkg not found. Set VCPKG_ROOT or install to C:\vcpkg
pause & exit /b 1

:found
echo [1/3] Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows
if errorlevel 1 (
    echo Failed to install dependencies.
    pause & exit /b 1
)

echo [2/3] Configuring...
if exist "build" rmdir /s /q "build"
mkdir "build"
pushd "build"

cmake "%~dp0." -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake"
if errorlevel 1 (
    echo.
    echo CMake configuration failed. See errors above.
    popd
    pause & exit /b 1
)

echo [3/3] Building...
cmake --build . --config Release -- /v:minimal /nologo
if errorlevel 1 (
    echo Build failed.
    popd
    pause & exit /b 1
)

popd
echo.
echo Done: build\bin\Release\ScreenShare.exe
pause
