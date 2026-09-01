@echo off
:: cook_fonts.bat -- bake the font list from config\fonts.manifest into assets\font\*.orb_font.
::
:: Calls bin\font_tool.exe directly (no ship_tool, no full ship pipeline), so a dev can re-bake
:: fonts as an inner-loop step.  Runs every manifest line and reports all failures at the end
:: rather than stopping at the first one.  Requires bin\font_tool.exe (build_tool -config Debug
:: -target font_tool).
setlocal
cd /d "%~dp0.."

if not exist bin\font_tool.exe (
    echo cook_fonts: bin\font_tool.exe not found -- build it first ^(build_tool -config Debug -target font_tool^)
    exit /b 1
)

bin\font_tool.exe manifest config\fonts.manifest
if errorlevel 1 (
    echo cook_fonts: FAILED
    exit /b 1
)
exit /b 0
