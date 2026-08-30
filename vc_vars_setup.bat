@echo off
:: Load the MSVC x64 build environment into the CURRENT shell.
::
:: Usage:  vc_vars_setup.bat [x64|x86|arm64]      (default x64)
::
:: Discovers Visual Studio through vswhere.exe rather than a hardcoded path, so
:: Insiders, Preview and Release installs all work and a VS upgrade does not
:: break the script.  Set ORB_VS_PATH to an installation root to force a
:: specific instance:
::
::     set ORB_VS_PATH=C:\Program Files\Microsoft Visual Studio\18\Community
::
:: No setlocal here on purpose -- the environment must survive into the caller.

set "ORB_VC_ARCH=%~1"
if "%ORB_VC_ARCH%"=="" set "ORB_VC_ARCH=x64"

:: Already loaded for this arch? vcvarsall sets VSCMD_ARG_TGT_ARCH.
if /i "%VSCMD_ARG_TGT_ARCH%"=="%ORB_VC_ARCH%" goto :vc_done

:: Caller-supplied override wins.
if defined ORB_VS_PATH (
    set "ORB_VC_ROOT=%ORB_VS_PATH%"
    goto :vc_have_root
)

:: Resolve the vswhere directory before any if-block: the ) inside
:: %ProgramFiles(x86)% would otherwise be read by the batch parser as the end of
:: the block.  The directory (not the full exe path) is what gets used below --
:: pushd'ing into it lets vswhere be invoked as .\vswhere.exe, which sidesteps
:: the cmd.exe rule that strips the outer quotes off a `for /f` backquote
:: command when it starts with a quoted path containing spaces.  The .\ prefix
:: is required: NoDefaultCurrentDirectoryInExePath, when set, keeps cmd from
:: resolving a bare exe name against the current directory.
set "ORB_VSWHERE_DIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%ORB_VSWHERE_DIR%\vswhere.exe" set "ORB_VSWHERE_DIR=%ProgramFiles%\Microsoft Visual Studio\Installer"
if not exist "%ORB_VSWHERE_DIR%\vswhere.exe" goto :vc_probe

set "ORB_VC_ROOT="
pushd "%ORB_VSWHERE_DIR%"

:: -prerelease so Insiders/Preview instances are considered alongside Release.
for /f "usebackq delims=" %%i in (`.\vswhere.exe -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "ORB_VC_ROOT=%%i"

:: Retry without -prerelease in case an older vswhere rejects the switch.
if not defined ORB_VC_ROOT for /f "usebackq delims=" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "ORB_VC_ROOT=%%i"

popd
if defined ORB_VC_ROOT goto :vc_have_root

:vc_probe
:: vswhere missing or unhelpful -- probe the well-known install roots, newest first.
for %%p in (
    "C:\Program Files\Microsoft Visual Studio\18\Insiders"
    "C:\Program Files\Microsoft Visual Studio\18\Preview"
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
) do if not defined ORB_VC_ROOT if exist "%%~p\VC\Auxiliary\Build\vcvarsall.bat" set "ORB_VC_ROOT=%%~p"

if not defined ORB_VC_ROOT (
    echo [vcvars] ERROR: no Visual Studio install with the C++ x64 toolset was found.
    echo [vcvars] Install "Desktop development with C++" or set ORB_VS_PATH to the VS root.
    goto :vc_cleanup
)

:vc_have_root
if not exist "%ORB_VC_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo [vcvars] ERROR: vcvarsall.bat not found under "%ORB_VC_ROOT%".
    echo [vcvars] The VS install is missing the "Desktop development with C++" workload.
    goto :vc_cleanup
)

call "%ORB_VC_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" %ORB_VC_ARCH%

:vc_done
:vc_cleanup
set "ORB_VSWHERE_DIR="
set "ORB_VC_ROOT="
set "ORB_VC_ARCH="
