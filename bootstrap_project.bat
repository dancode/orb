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

rem %~dp0 expands to the dir of the .bat file itself — drive + path, with trailing backslash.
rem Since bootstrap_project.bat lives at F:\orb\, %~dp0 is F:\orb\, 
rem so ENGINE_BIN becomes rem F:\orb\bin.

setlocal
set ENGINE_BIN=%~dp0bin


rem (echo ...) — the thing being written
rem > bin\build_tool.bat — redirects that output into the file
rem %%* passes through all arguemnts
rem \bin\build_tool.bat = @"F:\orb\bin\build_tool.exe" %*

if not exist bin mkdir bin
(echo @"%ENGINE_BIN%\build_tool.exe" %%*) > bin\build_tool.bat

echo [orb] project bootstrapped.
echo        Run: bin\build_tool.bat -gen
endlocal
