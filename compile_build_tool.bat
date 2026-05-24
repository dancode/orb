@echo off
setlocal

:: Recompile build_tool.exe without reading build_tool_debug.args at startup.
:: Use this instead of bootstrap_build_tool.bat when you want to step through
:: build_tool source changes without the debug-inject rerouting your target.
:: Assumes you are running from a Developer Command Prompt (vcvars loaded).

if not exist bin   mkdir bin
if not exist build mkdir build
if not exist build\obj mkdir build\obj

echo Compiling build_tool.exe (no debug inject)...
cl.exe /nologo /W4 /WX /Zi /std:c11 /wd4100 /wd4101 /wd4189 /DBUILD_TOOL_NO_DEBUG_INJECT ^
    source/tools/build_tool/build_tool.c ^
    /I source /Fobuild/obj/ /Fdbuild/obj/ /Fe:bin/build_tool.exe

if %errorlevel% neq 0 (
    echo FAILED to compile build_tool!
    pause
    exit /b 1
)

echo Success! bin/build_tool.exe rebuilt (debug inject disabled).
endlocal
