@echo off
:: cook_icons.bat -- bake the built-in icon set from config\icons.manifest into assets\icon\*.png.
::
:: Source SVGs live in assets\icon_source\; nothing bakes automatically, so re-run this whenever
:: they change (then rebuild so the gui backend picks up the new PNGs at next init). Requires
:: bin\image_tool.exe (build_tool -config Debug).
setlocal
cd /d "%~dp0.."

if not exist bin\image_tool.exe (
    echo cook_icons: bin\image_tool.exe not found -- build it first ^(build_tool -config Debug^)
    exit /b 1
)

bin\image_tool.exe icons config\icons.manifest
if errorlevel 1 (
    echo cook_icons: FAILED
    exit /b 1
)
exit /b 0
