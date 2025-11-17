#!/bin/bash
set -e

BUILD_DIR=build

echo "========================================"
echo "Building for Linux"
echo "========================================"
echo

if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

mkdir "$BUILD_DIR"
cd "$BUILD_DIR"

cmake ..

if [ $? -ne 0 ]; then
    echo "ERROR: CMake failed"
    cd ..
    exit 1
fi

cmake --build .

if [ $? -ne 0 ]; then
    echo "ERROR: Build failed"
    cd ..
    exit 1
fi

cd ..

echo
echo "Success! Executable: $PWD/$BUILD_DIR/bin/engine"
echo
