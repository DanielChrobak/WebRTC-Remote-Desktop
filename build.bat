@echo off
setlocal

:: Find vcpkg
set VCPKG=
for %%p in ("%VCPKG_ROOT%" "C:\vcpkg" "%USERPROFILE%\vcpkg" "vcpkg" "..\vcpkg") do (
    if exist "%%~p\vcpkg.exe" set VCPKG=%%~p& goto :found
)
echo vcpkg not found. Set VCPKG_ROOT or install to C:\vcpkg
pause & exit /b 1

:found
echo vcpkg: %VCPKG%

echo Installing dependencies...
"%VCPKG%\vcpkg.exe" install --triplet x64-windows
if errorlevel 1 (
    echo Dependency installation failed
    pause & exit /b 1
)

echo Building...
if exist build rmdir /s /q build
mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG%/scripts/buildsystems/vcpkg.cmake"
if errorlevel 1 ( echo CMake failed & cd .. & pause & exit /b 1 )

cmake --build . --config Release
if errorlevel 1 ( echo Build failed & cd .. & pause & exit /b 1 )

cd ..
echo Build complete: build\bin\Release\ScreenShare.exe
pause
