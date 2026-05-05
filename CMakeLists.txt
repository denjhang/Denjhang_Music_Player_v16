cmake_minimum_required(VERSION 3.16)
project(DenjhangMusicPlayerV16 CXX C)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# Output exe to bin/ directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# -------------------------------------------------------
# Directories
# -------------------------------------------------------
set(LIBVGM_DIR  ${CMAKE_SOURCE_DIR}/libvgm-modizer)
set(FTDI_DIR    ${CMAKE_SOURCE_DIR}/ftdi_driver)

# -------------------------------------------------------
# Common include directories (shared by all targets)
# -------------------------------------------------------
set(COMMON_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/midi
    ${CMAKE_SOURCE_DIR}/src/windows/ym2163
    ${CMAKE_SOURCE_DIR}/src/windows/opl3
    ${CMAKE_SOURCE_DIR}/src/windows/vgm
    ${CMAKE_SOURCE_DIR}/src/windows/gigatron
    ${CMAKE_SOURCE_DIR}/src/windows/sn76489
    ${CMAKE_SOURCE_DIR}/src/windows/ym2413
    ${CMAKE_SOURCE_DIR}/src/windows/spfm
    ${CMAKE_SOURCE_DIR}/src/windows/ay8910
    ${CMAKE_SOURCE_DIR}
    ${FTDI_DIR}
    ${CMAKE_SOURCE_DIR}/imgui
    ${CMAKE_SOURCE_DIR}/midifile/include
    ${LIBVGM_DIR}
    ${LIBVGM_DIR}/player
    ${LIBVGM_DIR}/audio
    ${LIBVGM_DIR}/emu
    ${LIBVGM_DIR}/utils
)

# -------------------------------------------------------
# ImGui - built as a static library for incremental builds
# -------------------------------------------------------
add_library(imgui_lib STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_impl_win32.cpp
    imgui/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PRIVATE ${COMMON_INCLUDES})
target_compile_options(imgui_lib PRIVATE -O2 -DAUDDRV_DSOUND)

# -------------------------------------------------------
# MidiFile - built as a static library for incremental builds
# -------------------------------------------------------
add_library(midifile_lib STATIC
    midifile/src/Binasc.cpp
    midifile/src/MidiEvent.cpp
    midifile/src/MidiEventList.cpp
    midifile/src/MidiFile.cpp
    midifile/src/MidiMessage.cpp
    midifile/src/Options.cpp
)
target_include_directories(midifile_lib PRIVATE ${COMMON_INCLUDES})
target_compile_options(midifile_lib PRIVATE -O2)

# -------------------------------------------------------
# Application sources (the parts you change frequently)
# -------------------------------------------------------
set(APP_SOURCES
    src/main.cpp
    src/app_icon.rc
    # core
    src/core/vgm_sync.cpp
    src/core/config_manager.cpp
    src/core/gui_renderer.cpp
    src/core/modizer_viz.cpp
    # midi
    src/midi/midi_player.cpp
    # windows/ym2163
    src/windows/ym2163/chip_control.cpp
    src/windows/ym2163/chip_window_ym2163.cpp
    # windows/opl3
    src/windows/opl3/opl3_renderer.cpp
    src/windows/opl3/opl3_window.cpp
    # windows/vgm
    src/windows/vgm/vgm_parser.cpp
    src/windows/vgm/vgm_window.cpp
    # windows/gigatron
    src/windows/gigatron/gigatron_emu.c
    src/windows/gigatron/winmm.c
    src/windows/gigatron/gigatron_window.cpp
    # windows/sn76489
    src/windows/sn76489/spfm_lite.c
    src/windows/sn76489/sn76489_window.cpp
    # windows/ym2413
    src/windows/ym2413/ym2413_window.cpp
    # windows/ay8910
    src/windows/ay8910/ay8910_window.cpp
    # windows/spfm
    src/windows/spfm/spfm_manager.cpp
    src/windows/spfm/spfm_window.cpp
)

add_executable(denjhang_music_player WIN32 ${APP_SOURCES})

target_include_directories(denjhang_music_player PRIVATE ${COMMON_INCLUDES})

target_compile_options(denjhang_music_player PRIVATE
    -Wall -Wextra -O2
    -DAUDDRV_DSOUND
)

target_link_options(denjhang_music_player PRIVATE -static)

# -------------------------------------------------------
# Scope core overrides - recompile fmopn.c/ym2151.c with modizer
# modifications. Linked BEFORE the prebuilt .a to override symbols.
# -------------------------------------------------------
set(SCOPE_CORE_DIR ${LIBVGM_DIR}/emu/cores)
add_library(scope_core_lib STATIC
    ${SCOPE_CORE_DIR}/fmopn.c
    ${SCOPE_CORE_DIR}/ym2151.c
    ${SCOPE_CORE_DIR}/scope_chip_reg.c
    ${SCOPE_CORE_DIR}/emu2149.c
    ${SCOPE_CORE_DIR}/ay8910.c
    ${SCOPE_CORE_DIR}/sn76496.c
    ${SCOPE_CORE_DIR}/k051649.c
    ${SCOPE_CORE_DIR}/fmopl.c
    ${SCOPE_CORE_DIR}/ymf262.c
    ${SCOPE_CORE_DIR}/adlibemu_opl2.c
    ${SCOPE_CORE_DIR}/adlibemu_opl3.c
    ${SCOPE_CORE_DIR}/emu2413.c
    ${SCOPE_CORE_DIR}/segapcm.c
    ${SCOPE_CORE_DIR}/okim6258.c
    ${SCOPE_CORE_DIR}/okim6295.c
    ${SCOPE_CORE_DIR}/okiadpcm.c
    ${SCOPE_CORE_DIR}/saa1099_vb.c
    ${SCOPE_CORE_DIR}/Ootake_PSG.c
    ${SCOPE_CORE_DIR}/c6280_mame.c
    ${SCOPE_CORE_DIR}/nesintf.c
    ${SCOPE_CORE_DIR}/np_nes_apu.c
    ${SCOPE_CORE_DIR}/np_nes_dmc.c
    ${SCOPE_CORE_DIR}/gb.c
    ${SCOPE_CORE_DIR}/pwm.c
    ${SCOPE_CORE_DIR}/ws_audio.c
)
# Match the defines used by the prebuilt libvgm-emu.a
target_compile_definitions(scope_core_lib PRIVATE
    SNDDEV_SELECT
    SNDDEV_SN76496 EC_SN76496_MAME EC_SN76496_MAXIM
    SNDDEV_YM2413 EC_YM2413_MAME EC_YM2413_EMU2413 EC_YM2413_NUKED
    SNDDEV_YM2612 EC_YM2612_GENS
    SNDDEV_YM2151 EC_YM2151_MAME EC_YM2151_NUKED
    SNDDEV_YM2203 SNDDEV_YM2608 SNDDEV_YM2610
    SNDDEV_YM3812 EC_YM3812_MAME EC_YM3812_ADLIBEMU EC_YM3812_NUKED
    SNDDEV_YM3526 SNDDEV_Y8950 SNDDEV_YMF262
    SNDDEV_AY8910 EC_AY8910_MAME EC_AY8910_NUKED EC_AY8910_KSS
    SNDDEV_OKIM6258 SNDDEV_OKIM6295
    SNDDEV_SEGAPCM SNDDEV_RF5C68
    SNDDEV_SCSP EC_SCSP_MAME
    SNDDEV_C6280 SNDDEV_NES_APU EC_NES_MAME EC_NES_NSFPLAY EC_NES_NSFP_FDS
    SNDDEV_GB_DMG EC_GB_DMG_MAME
    SNDDEV_HuC6280
    SNDDEV_SAA1099
    SNDDEV_POKEY
    SNDDEV_QSOUND EC_QSOUND_MAME
    SNDDEV_UPD7759
    SNDDEV_YM2151_OPM EC_YM2151_OPM_MAME EC_YM2151_OPM_NUKED
    SNDDEV_K051649 SNDDEV_K054539
    SNDDEV_K053260
    SNDDEV_GA20 SNDDEV_MIKEY
    SNDDEV_C352 SNDDEV_X1_010
    SNDDEV_WSWAN SNDDEV_VBOY_VSU
    SNDDEV_ES5503 SNDDEV_ES5506
    SNDDEV_YMW258
    SNDDEV_YMF271
    SNDDEV_YMF278B
)
target_include_directories(scope_core_lib PRIVATE
    ${SCOPE_CORE_DIR}
    ${LIBVGM_DIR}
    ${LIBVGM_DIR}/emu
    ${LIBVGM_DIR}/libs/include
)
target_compile_options(scope_core_lib PRIVATE -O2)

target_link_libraries(denjhang_music_player PRIVATE
    imgui_lib
    midifile_lib
    -Wl,--whole-archive
    scope_core_lib
    -Wl,--no-whole-archive
    ${FTDI_DIR}/amd64/libftd2xx.a
    ${LIBVGM_DIR}/build/bin/libvgm-player.a
    ${LIBVGM_DIR}/build/bin/libvgm-audio.a
    ${LIBVGM_DIR}/build/bin/libvgm-emu.a
    ${LIBVGM_DIR}/build/bin/libvgm-utils.a
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ws2_32
    gdi32
    comdlg32
    dsound
    winmm
    ole32
    uuid
    z
    iconv
)
