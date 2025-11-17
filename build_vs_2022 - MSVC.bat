@echo off
setlocal

set BUILD_DIR=build
set GENERATOR=Visual Studio 17 2022
set ARCH=x64

echo ========================================
echo Building with MSVC
echo ========================================
echo.

if exist %BUILD_DIR% (
    rmdir /s /q %BUILD_DIR%
)

mkdir %BUILD_DIR%
cd %BUILD_DIR%

cmake -G "%GENERATOR%" -A %ARCH% ..

if errorlevel 1 (
    echo ERROR: CMake failed
    cd ..
    pause
    exit /b 1
)

cd ..

echo.
echo Success! Solution: %CD%\%BUILD_DIR%\hybrid.sln
echo.

choice /C YN /M "Open in Visual Studio"
if errorlevel 2 goto :end
start %BUILD_DIR%\hybrid.sln

:end
endlocal