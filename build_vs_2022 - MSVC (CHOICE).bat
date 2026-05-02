@echo off
setlocal

set GENERATOR=Visual Studio 17 2022
set ARCH=x64

:: This MUST match the name in your project() call in CMakeLists.txt
set SLN_NAME=hybrid.sln

echo ========================================
echo ORB Engine Project Generator
echo ========================================
echo.
echo [1] Dynamic Build (DLLs / Hot-Reload)
echo [2] Monolithic Build (Static / Shipping)
echo.

choice /C 12 /M "Select build configuration:"

if errorlevel 2 (
    set BUILD_DIR=build_monolithic
    set MONO_FLAG=-DENGINE_MONOLITHIC=ON
) else (
    set BUILD_DIR=build_dynamic
    set MONO_FLAG=-DENGINE_MONOLITHIC=OFF
)

echo.
echo Target: %BUILD_DIR%
echo.

if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %BUILD_DIR%

:: Run CMake with the selected flag
cmake -G "%GENERATOR%" -A %ARCH% %MONO_FLAG% ..

if errorlevel 1 (
    echo ERROR: CMake failed
    cd ..
    pause
    exit /b 1
)

cd ..

echo.
echo Success! Solution: %CD%\%BUILD_DIR%\%SLN_NAME%
echo.

choice /C YN /M "Open in Visual Studio"
if errorlevel 2 goto :end
start %BUILD_DIR%\%SLN_NAME%

:end
endlocal