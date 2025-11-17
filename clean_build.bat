@echo off
if exist build rmdir /s /q build
if exist build-clang rmdir /s /q build-clang
if exist build-msvc rmdir /s /q build-msvc
echo All build directories cleaned.