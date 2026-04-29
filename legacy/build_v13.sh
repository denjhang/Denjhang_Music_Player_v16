#!/bin/bash
# Build script for YM2163 Piano GUI v13 (libvgm simulation player)

echo "Building YM2163 Piano GUI v13 (libvgm Edition)..."

# Compiler settings
CXX=g++
LIBVGM_DIR=libvgm-modizer
CFLAGS="-Wall -Wextra -O2 -std=c++11 -DAUDDRV_DSOUND"
CFLAGS="$CFLAGS -I. -Iftdi_driver -Iimgui -Imidifile/include"
CFLAGS="$CFLAGS -I${LIBVGM_DIR} -I${LIBVGM_DIR}/player -I${LIBVGM_DIR}/audio -I${LIBVGM_DIR}/emu -I${LIBVGM_DIR}/utils"

# libvgm-modizer precompiled libraries
LIBVGM_OBJS=""

LDFLAGS="ftdi_driver/amd64/libftd2xx.a"
LDFLAGS="$LDFLAGS ${LIBVGM_DIR}/build/bin/libvgm-player.a"
LDFLAGS="$LDFLAGS ${LIBVGM_DIR}/build/bin/libvgm-audio.a"
LDFLAGS="$LDFLAGS ${LIBVGM_DIR}/build/bin/libvgm-emu.a"
LDFLAGS="$LDFLAGS ${LIBVGM_DIR}/build/bin/libvgm-utils.a"
LDFLAGS="$LDFLAGS -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lws2_32 -lgdi32"
LDFLAGS="$LDFLAGS -ldsound -lwinmm -lole32 -luuid -lz -liconv"
LDFLAGS="$LDFLAGS -lcomdlg32 -static -mwindows"

TARGET=ym2163_piano_gui_v13.exe

echo "============================================"
echo "Step 1: Compiling ImGui sources..."
echo "============================================"

$CXX $CFLAGS -c imgui/imgui.cpp           -o imgui/imgui.o           || exit 1
$CXX $CFLAGS -c imgui/imgui_draw.cpp      -o imgui/imgui_draw.o      || exit 1
$CXX $CFLAGS -c imgui/imgui_tables.cpp    -o imgui/imgui_tables.o    || exit 1
$CXX $CFLAGS -c imgui/imgui_widgets.cpp   -o imgui/imgui_widgets.o   || exit 1
$CXX $CFLAGS -c imgui/imgui_impl_win32.cpp -o imgui/imgui_impl_win32.o || exit 1
$CXX $CFLAGS -c imgui/imgui_impl_dx11.cpp  -o imgui/imgui_impl_dx11.o  || exit 1

echo "============================================"
echo "Step 2: Compiling MidiFile library..."
echo "============================================"

$CXX $CFLAGS -c midifile/src/Binasc.cpp       -o midifile/Binasc.o       || exit 1
$CXX $CFLAGS -c midifile/src/MidiEvent.cpp     -o midifile/MidiEvent.o     || exit 1
$CXX $CFLAGS -c midifile/src/MidiEventList.cpp -o midifile/MidiEventList.o || exit 1
$CXX $CFLAGS -c midifile/src/MidiFile.cpp      -o midifile/MidiFile.o      || exit 1
$CXX $CFLAGS -c midifile/src/MidiMessage.cpp   -o midifile/MidiMessage.o   || exit 1
$CXX $CFLAGS -c midifile/src/Options.cpp       -o midifile/Options.o       || exit 1

echo "============================================"
echo "Step 3: Compiling application modules..."
echo "============================================"

$CXX $CFLAGS -c ym2163_control.cpp -o ym2163_control.o || exit 1
$CXX $CFLAGS -c config_manager.cpp -o config_manager.o || exit 1
$CXX $CFLAGS -c midi_player.cpp    -o midi_player.o    || exit 1
$CXX $CFLAGS -c gui_renderer.cpp   -o gui_renderer.o   || exit 1
$CXX $CFLAGS -c opl3_renderer.cpp  -o opl3_renderer.o  || exit 1
$CXX $CFLAGS -c vgm_parser.cpp     -o vgm_parser.o     || exit 1
$CXX $CFLAGS -c vgm_window.cpp     -o vgm_window.o     || exit 1
$CXX $CFLAGS -c main.cpp           -o main.o           || exit 1

echo "============================================"
echo "Step 4: Linking..."
echo "============================================"

$CXX -o $TARGET \
    ym2163_control.o config_manager.o midi_player.o gui_renderer.o \
    opl3_renderer.o vgm_parser.o vgm_window.o main.o \
    imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o \
    imgui/imgui_widgets.o imgui/imgui_impl_win32.o imgui/imgui_impl_dx11.o \
    midifile/Binasc.o midifile/MidiEvent.o midifile/MidiEventList.o \
    midifile/MidiFile.o midifile/MidiMessage.o midifile/Options.o \
    $LDFLAGS || exit 1

echo ""
echo "============================================"
echo "Build successful!"
echo "============================================"
echo "Executable: $TARGET"
echo ""
