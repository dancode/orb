@echo off
rem bootstrap_project.bat -- run from a child project directory to create a
rem forwarding bin\build_tool.bat that calls this engine's build_tool.exe.
rem
rem Usage (from the child project root):
rem     ..\bootstrap_project.bat
rem
rem After running:
rem     bin\build_tool.bat -gen
rem     bin\build_tool.bat -config Debug

setlocal
set ENGINE_BIN=%~dp0bin

if not exist bin mkdir bin
(echo @"%ENGINE_BIN%\build_tool.exe" %%*) > bin\build_tool.bat

echo [orb] project bootstrapped.
echo        Run: bin\build_tool.bat -gen
endlocal
