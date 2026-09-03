@echo off
:: import_icons.bat -- import the built-in icon set: config\icons.manifest rasterizes the SVGs
:: under source_content\ui\icon\ into content\ui\icon\*.png (checked in).
::
:: Nothing bakes automatically, so re-run this whenever the source art changes. Requires
:: bin\image_tool.exe (build_tool -config Debug).
setlocal
cd /d "%~dp0.."

if not exist bin\image_tool.exe (
    echo import_icons: bin\image_tool.exe not found -- build it first ^(build_tool -config Debug^)
    exit /b 1
)

bin\image_tool.exe icons config\icons.manifest
if errorlevel 1 (
    echo import_icons: FAILED
    exit /b 1
)
exit /b 0
