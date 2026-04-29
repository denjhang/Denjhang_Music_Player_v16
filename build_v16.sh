#!/bin/bash
# Build script for Denjhang's Music Player v16
# Uses CMake for incremental compilation - only changed files are recompiled.

set -e

BUILD_DIR="build"

echo "============================================"
echo " Denjhang's Music Player v16 - CMake Build"
echo "============================================"

# Remove .o files from prebuilt lib that we override with scope_core_lib
PREBUILT_LIB="libvgm-modizer/build/bin/libvgm-emu.a"
if [ -f "$PREBUILT_LIB" ]; then
    ar d "$PREBUILT_LIB" segapcm.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" okim6258.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" okim6295.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" okiadpcm.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" saa1099_vb.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" Ootake_PSG.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" c6280_mame.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" nesintf.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" np_nes_apu.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" np_nes_dmc.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" gb.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" pwm.c.o 2>/dev/null || true
    ar d "$PREBUILT_LIB" ws_audio.c.o 2>/dev/null || true
fi

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
echo " Executable: bin/denjhang_music_player.exe"
echo ""
echo " Tip: Run this script again after code changes"
echo "      - only modified files will be recompiled."
echo " To force full rebuild: rm -rf ${BUILD_DIR} && ./build_v16.sh"
echo ""
