#!/bin/bash
# Build script for YM2163 Piano GUI v14
# Uses CMake for incremental compilation - only changed files are recompiled.

set -e

BUILD_DIR="build"

echo "============================================"
echo " YM2163 Piano GUI v14 - CMake Build"
echo "============================================"

# Configure (only needed on first run or after CMakeLists.txt changes)
if [ ! -f "${BUILD_DIR}/Makefile" ] && [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "[1/2] Configuring CMake..."
    cmake -B "${BUILD_DIR}" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_C_COMPILER=gcc
else
    echo "[1/2] CMake already configured (skipping)"
fi

# Build - only recompiles changed files
echo "[2/2] Building (incremental)..."
cmake --build "${BUILD_DIR}" --parallel $(nproc)

echo ""
echo "============================================"
echo " Build successful!"
echo "============================================"
echo " Executable: ym2163_piano_gui_v14.exe"
echo ""
echo " Tip: Run this script again after code changes"
echo "      - only modified files will be recompiled."
echo " To force full rebuild: rm -rf ${BUILD_DIR} && ./build_v14.sh"
echo ""
