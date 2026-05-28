@echo off
if exist build rmdir /s /q build
if exist bin   rmdir /s /q bin
if not exist bin mkdir bin
(echo @"F:\orb\bin\build_tool.exe" %%*) > bin\build_tool.bat
echo [orb] clean complete. bin\build_tool.bat restored.
