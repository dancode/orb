@echo off
if not exist .orb_engine (
    echo [orb error] .orb_engine not found. Re-run bootstrap_project.bat to restore.
    exit /b 1
)
set /p ENGINE_ROOT=<.orb_engine
if exist build rmdir /s /q build
if exist bin   rmdir /s /q bin
if not exist bin mkdir bin
(echo @"%ENGINE_ROOT%\bin\build_tool.exe" %%*) > bin\build_tool.bat
echo [orb] clean complete. bin\build_tool.bat restored.
