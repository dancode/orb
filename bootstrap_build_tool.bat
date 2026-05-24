@echo off
setlocal

:: This script bootstraps the custom build tool.
:: It assumes you are running from a Developer Command Prompt.

if not exist bin mkdir bin
if not exist build mkdir build
if not exist build\obj mkdir build\obj

echo Bootstrapping Build Tool...
cl.exe /nologo /W4 /WX /Zi /std:c11 /wd4100 /wd4101 /wd4189 source/tools/build_tool/build_tool.c /I source /Fobuild/obj/ /Fdbuild/obj/ /Fe:bin/build_tool.exe

if %errorlevel% neq 0 (
    echo FAILED to bootstrap build tool!
    pause
    exit /b 1
)

echo Success! build_tool.exe created in bin/
endlocal