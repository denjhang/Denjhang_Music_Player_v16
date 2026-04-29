#!/bin/bash
# Build script for YM2163 Piano GUI v11 with MIDI Player

echo "Building YM2163 Piano GUI v11 (Quad YM2163 + 2MHz Edition)..."

# Compiler settings
CC=g++
CFLAGS="-Wall -Wextra -O2 -I. -Iftdi_driver -Iimgui -Imidifile/include -std=c++11"
LDFLAGS="ftdi_driver/amd64/libftd2xx.a -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lws2_32 -lgdi32 -static -lcomdlg32 -mwindows"

# Output
TARGET=ym2163_piano_gui_v11.exe

echo "============================================"
echo "Step 1: Compiling ImGui sources..."
echo "============================================"

$CC $CFLAGS -c imgui/imgui.cpp -o imgui/imgui.o || exit 1
$CC $CFLAGS -c imgui/imgui_draw.cpp -o imgui/imgui_draw.o || exit 1
$CC $CFLAGS -c imgui/imgui_tables.cpp -o imgui/imgui_tables.o || exit 1
$CC $CFLAGS -c imgui/imgui_widgets.cpp -o imgui/imgui_widgets.o || exit 1
$CC $CFLAGS -c imgui/imgui_impl_win32.cpp -o imgui/imgui_impl_win32.o || exit 1
$CC $CFLAGS -c imgui/imgui_impl_dx11.cpp -o imgui/imgui_impl_dx11.o || exit 1

echo "============================================"
echo "Step 2: Compiling MidiFile library..."
echo "============================================"

$CC $CFLAGS -c midifile/src/Binasc.cpp -o midifile/Binasc.o || exit 1
$CC $CFLAGS -c midifile/src/MidiEvent.cpp -o midifile/MidiEvent.o || exit 1
$CC $CFLAGS -c midifile/src/MidiEventList.cpp -o midifile/MidiEventList.o || exit 1
$CC $CFLAGS -c midifile/src/MidiFile.cpp -o midifile/MidiFile.o || exit 1
$CC $CFLAGS -c midifile/src/MidiMessage.cpp -o midifile/MidiMessage.o || exit 1
$CC $CFLAGS -c midifile/src/Options.cpp -o midifile/Options.o || exit 1

echo "============================================"
echo "Step 3: Compiling main application..."
echo "============================================"

$CC $CFLAGS -c ym2163_piano_gui_v11.cpp -o ym2163_piano_gui_v11.o || exit 1

echo "============================================"
echo "Step 4: Linking..."
echo "============================================"

$CC -o $TARGET ym2163_piano_gui_v11.o \
    imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o \
    imgui/imgui_widgets.o imgui/imgui_impl_win32.o \
    imgui/imgui_impl_dx11.o \
    midifile/Binasc.o midifile/MidiEvent.o midifile/MidiEventList.o \
    midifile/MidiFile.o midifile/MidiMessage.o midifile/Options.o \
    $LDFLAGS || exit 1

echo ""
echo "============================================"
echo "Build successful!"
echo "============================================"
echo "Executable: $TARGET"
echo ""
echo "Make sure ym2163_midi_config.ini is in the same directory!"
echo ""
