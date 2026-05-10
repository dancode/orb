@echo off
REM build_hot.bat — rebuild a single module for hot-reload.
REM Called by Visual Studio External Tools. The VS debugger stays attached.
REM
REM Usage: build_hot.bat <build_dir> <target> <config>

set BUILD_DIR=%1
set TARGET=%2
set CONFIG=%3
if "%CONFIG%"=="" set CONFIG=Debug

echo [hot-build] cmake --build %BUILD_DIR% --target %TARGET% --config %CONFIG%
cmake --build "%BUILD_DIR%" --target %TARGET% --config %CONFIG%

if %ERRORLEVEL% NEQ 0 (
    echo [hot-build] FAILED
    exit /b %ERRORLEVEL%
)
echo [hot-build] OK -- file watcher will reload %TARGET%