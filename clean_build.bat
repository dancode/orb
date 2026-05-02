@echo off
if exist build rmdir /s /q build
if exist build_dynamic rmdir /s /q build_dynamic
if exist build_monolithic rmdir /s /q build_monolithic

echo All build directories cleaned.