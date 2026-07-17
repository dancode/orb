@echo off
REM ship_sample_game.bat -- run the full ship pipeline for sample_game.
REM
REM Thin wrapper over ship_tool: build -> cook -> stage -> package -> deploy,
REM staging into build\ship\sample_game (monolithic Release by default).
REM Extra arguments pass straight through to ship_tool, e.g.:
REM
REM   ship_sample_game.bat -modular              ship host_game.exe + module DLLs
REM   ship_sample_game.bat -only stage -pdb      restage prebuilt output with pdbs
REM   ship_sample_game.bat -config Debug         ship a Debug build
REM
REM Must run from the engine root (ship_tool validates via orb.targets).

cd /d "%~dp0"

echo [ship] bin\ship_tool.exe sample_game %*
bin\ship_tool.exe sample_game %* -modular -only stage -pdb

if %ERRORLEVEL% NEQ 0 (
    echo [ship] FAILED
    exit /b %ERRORLEVEL%
)
echo [ship] OK -- build\ship\sample_game
