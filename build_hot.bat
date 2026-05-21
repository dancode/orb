@echo off
REM build_hot.bat -- rebuild a single module for hot-reload.
REM
REM Called by Visual Studio External Tools (or directly) while the debugger
REM remains attached. The build is routed through our own build_tool.exe
REM rather than CMake/MSBuild, because:
REM   - build_tool's PDB rotation (Fix #1) lets the linker write a fresh
REM     symbol file without touching the one the debugger has locked.
REM   - It honors the same incremental + /showIncludes dep tracking the
REM     full build uses, so hot rebuilds match a normal rebuild exactly.
REM   - It skips the CMake reconfigure step entirely.
REM
REM Usage: build_hot.bat <target> [config]
REM   target  -- module name as it appears in g_targets[] (e.g. render, audio)
REM   config  -- Debug | Release  (default: Debug)
REM
REM Legacy call shape "build_hot.bat <build_dir> <target> <config>" is
REM still accepted; the build_dir argument is ignored.

setlocal

set TARGET=%1
set CONFIG=%2

REM Legacy 3-arg form: shift the build_dir away.
if not "%3"=="" (
    set TARGET=%2
    set CONFIG=%3
)

if "%TARGET%"=="" (
    echo [hot-build] Usage: build_hot.bat ^<target^> [Debug^|Release]
    exit /b 2
)
if "%CONFIG%"=="" set CONFIG=Debug

echo [hot-build] bin\build_tool.exe -no-deps -config %CONFIG% -target %TARGET%
bin\build_tool.exe -no-deps -config %CONFIG% -target %TARGET%

if %ERRORLEVEL% NEQ 0 (
    echo [hot-build] FAILED
    exit /b %ERRORLEVEL%
)
echo [hot-build] OK -- file watcher will reload %TARGET%
endlocal
