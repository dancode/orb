@echo off
if exist build rmdir /s /q build
if exist build_modular rmdir /s /q build_modular
if exist build_monolithic rmdir /s /q build_monolithic

echo All build directories cleaned.