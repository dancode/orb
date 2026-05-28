@echo off
rem bootstrap_project.bat -- install build tool forwarder and helpers in a child project.
rem
rem Usage (from the child project root):
rem     ..\bootstrap_project.bat
rem
rem After running:
rem     bin\build_tool.bat -gen
rem     bin\build_tool.bat -config Debug
rem     clean_build.bat

rem %~dp0 expands to the engine root with a trailing backslash; strip it.
setlocal
set ENGINE_ROOT=%~dp0
if "%ENGINE_ROOT:~-1%"=="\" set ENGINE_ROOT=%ENGINE_ROOT:~0,-1%

if not exist bin mkdir bin

rem Record engine root for clean_build.bat (no trailing backslash).
(echo %ENGINE_ROOT%)> .orb_engine

rem Install the build_tool forwarder.
(echo @"%ENGINE_ROOT%\bin\build_tool.exe" %%*) > bin\build_tool.bat

rem Copy the clean script from the engine. It reads .orb_engine at runtime
rem so it works regardless of where this project or the engine lives.
copy /y "%ENGINE_ROOT%\clean_build_project.bat" clean_build.bat > nul

echo [orb] project bootstrapped.
echo        Run: bin\build_tool.bat -gen
endlocal
