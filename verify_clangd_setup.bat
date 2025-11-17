@echo off
setlocal

echo ========================================
echo Clangd Setup Verification
echo ========================================
echo.

REM Check if compile_commands.json exists
set BUILD_DIR=build-clang
set COMPILE_DB=%BUILD_DIR%\compile_commands.json

if exist %COMPILE_DB% (
    echo [OK] compile_commands.json found
    echo      Location: %CD%\%COMPILE_DB%
) else (
    echo [ERROR] compile_commands.json NOT found!
    echo         Expected: %CD%\%COMPILE_DB%
    echo         Run build-clang.bat first
    goto :error
)

echo.

REM Check if clangd is available
where clangd >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [OK] clangd found in PATH
    for /f "tokens=*" %%i in ('where clangd') do echo      Location: %%i
) else (
    echo [WARNING] clangd not found in PATH
    echo           Check if LLVM is installed
    echo           Clang Power Tools may still work with bundled clangd
)

echo.

REM Check if .clangd config exists
if exist .clangd (
    echo [OK] .clangd config file found
) else (
    echo [INFO] .clangd config file not found
    echo        Creating default configuration...
    (
        echo CompileFlags:
        echo   CompilationDatabase: %BUILD_DIR%
        echo   Add:
        echo     - -Wno-unused-function
        echo     - -Wno-unused-variable
        echo Diagnostics:
        echo   Suppress:
        echo     - unused-function
    ) > .clangd
    echo [OK] Created .clangd config file
)

echo.

REM Verify compile_commands.json content
echo Checking compile_commands.json entries...
findstr /C:"\"file\"" %COMPILE_DB% >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [OK] compile_commands.json contains compilation entries
    for /f %%a in ('findstr /C:"\"file\"" %COMPILE_DB% ^| find /C /V ""') do (
        echo      Found %%a file entries
    )
) else (
    echo [ERROR] compile_commands.json appears empty or invalid
    goto :error
)

echo.
echo ========================================
echo Setup Verification Complete
echo ========================================
echo.
echo Next Steps:
echo 1. Install Clang Power Tools extension in Visual Studio
echo 2. Disable built-in IntelliSense: Tools ^> Options ^> Text Editor ^> C/C++ ^> Advanced
echo 3. Configure Clang Power Tools to use clangd
echo 4. Open build-clang\hybrid.sln in Visual Studio
echo.
echo Your unity build files should now have proper IntelliSense!
echo.
goto :end

:error
echo.
echo ========================================
echo Verification FAILED
echo ========================================
echo.
echo Please fix the errors above and try again.
echo.
pause
exit /b 1

:end
endlocal
exit /b 0