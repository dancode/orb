@echo off
rem bootstrap_project.bat -- run from a child project directory to install
rem bin\build_tool.bat (forwarder) and clean_build.bat (wipe + restore).
rem
rem Usage (from the child project root):
rem     ..\bootstrap_project.bat
rem
rem After running:
rem     bin\build_tool.bat -gen
rem     bin\build_tool.bat -config Debug
rem     clean_build.bat

rem %~dp0 expands to the dir of this .bat — drive + path with trailing backslash.
rem ENGINE_BIN resolves to e.g. F:\orb\bin regardless of where the child project lives.

setlocal
set ENGINE_BIN=%~dp0bin

if not exist bin mkdir bin

rem Install the build_tool forwarder.
(echo @"%ENGINE_BIN%\build_tool.exe" %%*) > bin\build_tool.bat

rem Install clean_build.bat: wipes build\ and bin\, then restores the forwarder.
(
    echo @echo off
    echo if exist build rmdir /s /q build
    echo if exist bin   rmdir /s /q bin
    echo if not exist bin mkdir bin
    echo ^(echo @"%ENGINE_BIN%\build_tool.exe" %%%%*^) ^> bin\build_tool.bat
    echo echo [orb] clean complete. bin\build_tool.bat restored.
) > clean_build.bat

echo [orb] project bootstrapped.
echo        Run: bin\build_tool.bat -gen
endlocal
