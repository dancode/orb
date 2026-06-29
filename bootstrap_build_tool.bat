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

if not exist bin    mkdir bin
if not exist build  mkdir build
if not exist build\obj mkdir build\obj

:: --- Manifest embedding toggle ---
:: Set to 1 to compile the version-info resource and embed the app manifest into
:: build_tool.exe, which reduces AV heuristic false-positive flags on the unsigned
:: binary.  Set to 0 (or delete the line) to skip both steps.
set EMBED_MANIFEST=1

:: --- Step 1: Compile version-info resource (.rc -> .res) ---
:: rc.exe is part of the Windows SDK and is on PATH after vcvarsall.
:: Suppress the rc.exe logo (/nologo) and write the .res into the obj dir.

set "RES_FILE="
if "%EMBED_MANIFEST%"=="1" (
    echo [bootstrap] Compiling version resource...
    rc.exe /nologo /fo build\obj\build_tool.res source\tools\build_tool\build_tool.rc
    if %errorlevel% neq 0 (
        echo [bootstrap] WARNING: rc.exe failed -- continuing without version resource.
    ) else (
        set "RES_FILE=build\obj\build_tool.res"
    )
)

:: --- Step 2: Compile build_tool.exe (unity build) ---
:: cl.exe accepts .res files alongside .c files on the same command line.
:: The linker automatically includes the compiled resource in the output binary.
:: /DBUILD_TOOL_EMBED_MANIFEST activates the rc/manifest code paths inside the
:: self-hosted -bootstrap build (must match the EMBED_MANIFEST toggle above).

echo [bootstrap] Compiling build_tool.exe...
if "%EMBED_MANIFEST%"=="1" (
    cl.exe /nologo /W4 /WX /Zi /std:c11 /wd4100 /wd4101 /wd4189 /DBUILD_TOOL_EMBED_MANIFEST source/tools/build_tool/build_tool.c %RES_FILE% /I source /Fobuild/obj/ /Fdbuild/obj/ /Fe:bin/build_tool.exe /link /MANIFEST:EMBED /MANIFESTINPUT:source\tools\build_tool\build_tool.manifest
) else (
    cl.exe /nologo /W4 /WX /Zi /std:c11 /wd4100 /wd4101 /wd4189 source/tools/build_tool/build_tool.c /I source /Fobuild/obj/ /Fdbuild/obj/ /Fe:bin/build_tool.exe
)

if %errorlevel% neq 0 (
    echo [bootstrap] FAILED -- cl.exe returned error
    pause
    exit /b 1
)

:: Manifest is embedded by the linker via /MANIFEST:EMBED /MANIFESTINPUT: above.
:: No separate mt.exe post-link step needed.

echo [bootstrap] OK -- build_tool.exe created in bin/
endlocal