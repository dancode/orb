@echo off
REM run_gui_bench.bat -- build and run the gui performance benchmark suite.
REM
REM Builds sb_gui_bench through build_tool.exe (the vcvars cache makes this work from a plain
REM terminal), runs the whole suite unattended, and leaves a date-stamped report in
REM artifacts\bench\.  Any other argument passes through to the exe (-case, -frames, -settle).
REM
REM Usage: run_gui_bench.bat [Debug|Release] [extra args...]
REM   config defaults to Release -- Debug CPU numbers are not comparable to Release ones,
REM   and the report header records which config produced it.

setlocal

set CONFIG=Release
set ARGS=

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="Debug"   ( set CONFIG=Debug&   shift& goto parse )
if /i "%~1"=="Release" ( set CONFIG=Release& shift& goto parse )
set ARGS=%ARGS% %1
shift
goto parse
:parsed

echo [gui-bench] bin\build_tool.exe -config %CONFIG% -target sb_gui_bench
bin\build_tool.exe -config %CONFIG% -target sb_gui_bench
if %ERRORLEVEL% NEQ 0 (
    echo [gui-bench] BUILD FAILED
    exit /b %ERRORLEVEL%
)

bin\sb_gui_bench.exe -run%ARGS%
exit /b %ERRORLEVEL%
