@echo off
:: clean_outputs.bat -- clean build OUTPUTS only (bin\ and build\obj).
:: Unlike clean_build.bat, this keeps the generated solution/project files
:: (build\proj*, build\generated, build\.vcvars_x64) so Visual Studio can
:: stay open across the clean.
if exist build\obj rmdir /s /q build\obj
if exist bin       rmdir /s /q bin

echo Output directories cleaned (solution/project files kept).

:: Restore third-party runtime files (freetype dll/lib for font_tool, etc.)
:: that must always live in bin\ alongside the build outputs.
if exist third_party\bin (
    mkdir bin
    xcopy /y /q third_party\bin\* bin\ >nul
    echo Restored third_party\bin files into bin\.
)
