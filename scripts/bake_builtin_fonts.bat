@echo off
rem ============================================================================
rem  bake_builtin_fonts.bat -- Bake every built-in GUI font preset through
rem  font_tool (FreeType) into assets\font\.
rem
rem  Keep this list in sync with s_builtin_font_path[] in
rem      source\runtime_service\gui\backend\resource\gui_font.c
rem
rem  font_tool resolves a bare name via assets\font_source\ then the OS fonts,
rem  and (with no output arg) writes assets\font\<stem>_<size>px.orb_font --
rem  the exact path the engine loads each preset from.
rem ============================================================================

setlocal
cd /d "%~dp0.."

set "FONT_TOOL=bin\font_tool.exe"
if not exist "%FONT_TOOL%" (
    echo ERROR: "%FONT_TOOL%" not found.
    echo   Build it first:  bin\build_tool.exe -config Debug -target font_tool
    exit /b 1
)

set /a FAIL=0

rem  <font name or file>            <size_px>
call :bake JetBrainsMonoNL-Regular  16
call :bake Roboto-Regular           16
call :bake CascadiaMono             12
call :bake CascadiaMono             16
call :bake CascadiaMono             20
call :bake CascadiaCode             16

echo.
if %FAIL% gtr 0 (
    echo [bake_builtin_fonts] %FAIL% font^(s^) FAILED.
    exit /b 1
)
echo [bake_builtin_fonts] all built-in fonts baked OK.
exit /b 0

rem ----------------------------------------------------------------------------
rem  :bake <font> <size>  -- run one font through font_tool, tally failures.
rem ----------------------------------------------------------------------------
:bake
echo.
echo === baking %1 @ %2px ===
"%FONT_TOOL%" %1 %2
if errorlevel 1 (
    echo *** FAILED: %1 %2
    set /a FAIL+=1
)
goto :eof
