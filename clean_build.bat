@echo off
if exist build rmdir /s /q build
if exist bin   rmdir /s /q bin

echo All build directories cleaned.

:: Restore third-party runtime files (freetype dll/lib for font_tool, etc.)
:: that must always live in bin\ alongside the build outputs.
if exist third_party\bin (
    mkdir bin
    xcopy /y /q third_party\bin\* bin\ >nul
    echo Restored third_party\bin files into bin\.
)
