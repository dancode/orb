@echo off
setlocal

:: Bootstraps the build tool and immediately generates the Visual Studio solution files.
:: Assumes you are running from a Developer Command Prompt (vcvarsall.bat already loaded).

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

echo Generating Visual Studio project files...
bin\build_tool.exe -gen

if %errorlevel% neq 0 (
    echo FAILED to generate project files!
    pause
    exit /b 1
)

:: echo Done 
endlocal
