@echo off
setlocal

:: Bootstrap build_tool.exe. Calls vcvarsall automatically via vswhere so
:: a plain cmd.exe or PowerShell window works -- no Dev Prompt required.

:: --- Auto-setup VC environment if cl.exe is not already on PATH ---
:: Resolve vswhere.exe path before any if-block to avoid the batch parser
:: misreading the ) in %ProgramFiles(x86)% as closing the if block.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

where /q cl.exe
if not errorlevel 1 goto :have_cl

    if not exist "%VSWHERE%" (
        echo [bootstrap] ERROR: vswhere.exe not found. Install Visual Studio 2022 or run from a Dev Prompt.
        exit /b 1
    )
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
    if not defined VS_PATH (
        echo [bootstrap] ERROR: No Visual Studio installation with VC tools found.
        exit /b 1
    )
    call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    where /q cl.exe
    if errorlevel 1 (
        echo [bootstrap] ERROR: vcvarsall.bat ran but cl.exe still not found.
        exit /b 1
    )

:have_cl

if not exist bin mkdir bin
if not exist build mkdir build
if not exist build\obj mkdir build\obj

echo [bootstrap] Compiling build_tool.exe...
cl.exe /nologo /W4 /WX /Zi /std:c11 /wd4100 /wd4101 /wd4189 source/tools/build_tool/build_tool.c /I source /Fobuild/obj/ /Fdbuild/obj/ /Fe:bin/build_tool.exe

if %errorlevel% neq 0 (
    echo [bootstrap] FAILED -- cl.exe returned error
    pause
    exit /b 1
)

echo [bootstrap] OK -- build_tool.exe created in bin/
endlocal