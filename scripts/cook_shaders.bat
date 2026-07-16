@echo off
:: cook_shaders.bat -- cook the engine's HLSL shaders into bin\shaders\*.oshd.
::
:: The cooked files are OPTIONAL: gui and draw prefer them when present next to the exe and
:: fall back to their embedded SPIR-V arrays when absent -- delete bin\shaders to turn the
:: cooked path off.  Requires bin\asset_tool.exe and bin\shader_tool.exe (build_tool -config
:: Debug) plus dxc.exe from %%VULKAN_SDK%%.  asset_tool derives each dxc profile from the
:: .vs/.ps stage tag in the filename and forwards to shader_tool.
setlocal
cd /d "%~dp0.."

if not exist bin\asset_tool.exe (
    echo cook_shaders: bin\asset_tool.exe not found -- build it first ^(build_tool -config Debug^)
    exit /b 1
)
if not exist bin\shaders mkdir bin\shaders

set FAILED=0
call :cook source\runtime_service\gui\shaders\gui.vs.hlsl          bin\shaders\gui.vs.oshd
call :cook source\runtime_service\gui\shaders\gui.ps.hlsl          bin\shaders\gui.ps.oshd
call :cook source\runtime_service\draw\shaders\draw_solid.vs.hlsl  bin\shaders\draw_solid.vs.oshd
call :cook source\runtime_service\draw\shaders\draw_solid.ps.hlsl  bin\shaders\draw_solid.ps.oshd
call :cook source\runtime_service\draw\shaders\draw_tex.vs.hlsl    bin\shaders\draw_tex.vs.oshd
call :cook source\runtime_service\draw\shaders\draw_tex.ps.hlsl    bin\shaders\draw_tex.ps.oshd

if %FAILED% neq 0 (
    echo cook_shaders: %FAILED% shader^(s^) FAILED
    exit /b 1
)
echo cook_shaders: all shaders cooked into bin\shaders
exit /b 0

:cook
bin\asset_tool.exe cook %1 %2
if errorlevel 1 set /a FAILED+=1
goto :eof
