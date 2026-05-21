@echo off
setlocal

:: This script bootstraps the custom build tool.
:: It assumes you are running from a Developer Command Prompt.

if not exist bin mkdir bin
if not exist build_new mkdir build_new
if not exist build_new\obj mkdir build_new\obj

echo Bootstrapping Build Tool...
cl.exe /nologo /W4 /WX /Zi /std:c11 source/tools/build_tool/build_tool.c /I source /Fobuild_new/obj/ /Fdbuild_new/obj/ /Fe:bin/build_tool.exe

if %errorlevel% neq 0 (
    echo FAILED to bootstrap build tool!
    pause
    exit /b 1
)

echo Success! build_tool.exe created in bin/
endlocal