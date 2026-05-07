@echo off
setlocal

:: set GENERATOR=Visual Studio 18 2026
set GENERATOR=Visual Studio 17 2022
set ARCH=x64

:: This MUST match the name in your project() call in CMakeLists.txt
set SLN_NAME=orb.sln

echo ========================================
echo ORB Engine Project Generator (Dynamic)
echo ========================================
echo.

:: Automatically selecting the Dynamic Build configuration
set BUILD_DIR=build_dynamic
set MONO_FLAG=-DENGINE_MONOLITHIC=OFF

echo Target: %BUILD_DIR%
echo Flag:   %MONO_FLAG%
echo.

if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %BUILD_DIR%

:: Run CMake
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

:: Keeping this choice so it doesn't just force-open VS if you aren't ready
choice /C YN /M "Open in Visual Studio"
if errorlevel 2 goto :end
start %BUILD_DIR%\%SLN_NAME%

:end
endlocal