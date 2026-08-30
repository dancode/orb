@echo off
call "%~dp0..\vc_vars_setup.bat" x64
start "" devenv "%~dp0..\build\proj_ms\orb_ms.sln"
