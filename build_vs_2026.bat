@echo off
setlocal
set BUILD_DIR=build
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %BUILD_DIR%
cmake -G "Visual Studio 18 2026" -A x64 ..
cd ..
echo.
echo "Open %CD%\%BUILD_DIR%\hybrid.sln in Visual Studio (or run msbuild from command line)."
endlocal
