#!/bin/bash
# Direct compilation script for vgmplay_imgui.exe

echo "Building vgmplay_imgui.exe..."

# Source files
IMGUI_SOURCES="
imgui/imgui.cpp
imgui/imgui_draw.cpp
imgui/imgui_tables.cpp
imgui/imgui_widgets.cpp
imgui/imgui_impl_win32.cpp
imgui/imgui_impl_dx11.cpp
"

# Main source
MAIN_SOURCE="vgmplay_imgui.cpp"

# Include paths
INCLUDES="
-I.
-I./imgui
-I/d/msys64/mingw64/include
"

# Library paths and libraries
LIBS="
-L./build/bin
-L/d/msys64/mingw64/lib
-lvgm-audio
-lvgm-player
-lvgm-utils
-lvgm-emu
-ld3d11
-ldxgi
-lole32
-lshlwapi
-ldsound
-lwinmm
-lz
-static-libgcc
-static-libstdc++
"

# Compile flags
CXXFLAGS="-std=c++14 -O2 -Wall -mwindows"

# Compile
echo "Compiling..."
g++ $CXXFLAGS $INCLUDES $MAIN_SOURCE $IMGUI_SOURCES $LIBS -o vgmplay_imgui.exe

if [ $? -eq 0 ]; then
    echo "Build successful! Created vgmplay_imgui.exe"
    ls -lh vgmplay_imgui.exe
else
    echo "Build failed!"
    exit 1
fi
