// sn76489_window.cpp - SN76489 (DCSG) Hardware Control Window
// Controls real SN76489 chip via FTDI/SPFM Light interface
// VGM hardware playback + debug test functions + file browser

#include "sn76489_window.h"
#include "sn76489/spfm.h"
#include "sn76489/sn76489.h"
#include "chip_control.h"
#include "chip_window_ym2163.h"
#include "midi_player.h"
#include "modizer_viz.h"
#include "libvgm-modizer/emu/cores/ModizerVoicesData.h"
#include "imgui/imgui.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <math.h>
#include <commdlg.h>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <chrono>

namespace SN76489Window {

// ============ Constants ============
static const int SN_SAMPLE_RATE = 44100;

static ImU32 kChColors[5] = {
    IM_COL32(160, 200, 160, 255), // Ch0: green
    IM_COL32(160, 160, 200, 255), // Ch1: blue
    IM_COL32(200, 160, 160, 255), // Ch2: red
    IM_COL32(160, 160, 160, 255), // Ch3: periodic noise (gray)
    IM_COL32(200, 200, 160, 255),  // Ch4: white noise (yellow)
};
static ImU32 kCh2Colors[5] = {
    IM_COL32(200, 160, 200, 255), // Ch0: purple
    IM_COL32(200, 200, 160, 255), // Ch1: yellow
    IM_COL32(160, 200, 200, 255), // Ch2: cyan
    IM_COL32(200, 160, 160, 255), // Ch3: periodic noise (pink)
    IM_COL32(160, 200, 200, 255),  // Ch4: white noise (cyan)
};
static const char* kChNames[4] = { "Tone0", "Tone1", "Tone2", "Noise" };

// ============ Connection State ============
static bool s_connected = false;
static bool s_connected2 = true; // 2nd SN76489 (slot 1, T6W28 noise chip)
static bool s_manualDisconnect = false;
static ImU32 kChColorsCustom[10] = {}; // 0=use default

// ============ Test State ============
static bool s_testRunning = false;
static int s_testType = 0;
static int s_testStep = 0;
static double s_testStepMs = 0.0;
static LARGE_INTEGER s_testStartTime;
static LARGE_INTEGER s_perfFreq;

// ============ VGM Playback State ============
static FILE* s_vgmFile = nullptr;
static char s_vgmPath[MAX_PATH] = "";
static bool s_vgmLoaded = false;
static bool s_vgmPlaying = false;
static HANDLE s_vgmThread = nullptr;
static volatile bool s_vgmThreadRunning = false;
static bool s_vgmPaused = false;
static bool s_vgmTrackEnded = false;
static UINT32 s_vgmTotalSamples = 0;
static UINT32 s_vgmCurrentSamples = 0;
static UINT32 s_vgmDataOffset = 0;
static UINT32 s_vgmLoopOffset = 0;
static UINT32 s_vgmLoopSamples = 0;
static int s_vgmLoopCount = 0;
static int s_vgmMaxLoops = 2;  // max internal loop count (0=infinite)
static bool s_isT6W28 = false; // VGM uses T6W28 (NeoGeoPocket) noise chip
static int s_t6w28Mode = 2;    // 0=passthrough, 1=force sf2, 2=dual chip (default)
static uint16_t s_t6w28NoiseCh2Period = 0; // T6W28 noise chip 的独立 ch2 频率
static int s_t6w28LastLatchReg = -1;       // 0x30 最后 latch 的寄存器号

// Seek & Fadeout
static int s_seekMode = 0;           // 0=fast-forward, 1=direct (reset + resume)
static float s_fadeoutDuration = 3.0f; // fadeout duration in seconds (0=disabled)
static bool s_fadeoutActive = false;
static float s_fadeoutLevel = 1.0f;   // 1.0=normal, 0.0=mute
static UINT32 s_fadeoutStartSample = 0;
static UINT32 s_fadeoutEndSample = 0;

// Channel mute/solo
static bool s_chMuted[8] = {};       // per-channel mute (8 channels: slot0×4 + slot1×4)
static int s_soloCh = -1;            // solo channel (-1=none)

// GD3 tags
static std::string s_trackName, s_gameName, s_systemName, s_artistName;
static UINT32 s_vgmVersion = 0;

// ============ Channel State (slot 0 / main) ============
static uint8_t s_vol[4] = {15, 15, 15, 15};
static uint8_t s_noiseType = 0;
static uint8_t s_noiseFreq = 0;
static bool s_noiseUseCh2 = false;
static int s_dcsgLfsrWidth = 16; // 15=TI, 16=Sega VDP, 17=extended
static uint16_t s_fullPeriod[3] = {0, 0, 0};

// ============ Channel State (slot 1 / 2nd SN76489) ============
static uint8_t s2_vol[4] = {15, 15, 15, 15};
static uint8_t s2_noiseType = 0;
static uint8_t s2_noiseFreq = 0;
static bool s2_noiseUseCh2 = false;
static uint16_t s2_fullPeriod[3] = {0, 0, 0};
static int s2_lastToneLatchCh = -1;

// ============ Piano State ============
static const int SN_PIANO_LOW = 24;
static const int SN_PIANO_HIGH = 107;
static const int SN_PIANO_KEYS = SN_PIANO_HIGH - SN_PIANO_LOW + 1;
static bool s_pianoKeyOn[SN_PIANO_KEYS] = {};
static float s_pianoKeyLevel[SN_PIANO_KEYS] = {};
static int s_pianoKeyChannel[SN_PIANO_KEYS] = {};
static int s_pianoKeyNoiseType[SN_PIANO_KEYS] = {}; // -1=not noise, 0=periodic, 1=white
static const bool s_isBlackNote[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
static int s_shiftNoteMap[3] = {96, 84, 72}; // sf0→C7, sf1→C6, sf2→C5

// ============ Level Meter State ============
static float s_channelLevel[4] = {};
static float s2_channelLevel[4] = {};

// ============ Scope State ============
static ModizerViz s_scope;
static bool s_showScope = false;
static float s_scopeHeight = 80.0f;
static int s_voiceCh[4] = {-1, -1, -1, -1};
static int s_scopeSamples = 441;
static float s_scopeAmplitude = 3.0f;
static int s_lastToneLatchCh = 0;  // for VGM 0x50 unlatched data

// ============ Log ============
static std::string s_log;
static char s_logDisplay[65536] = "";
static bool s_logAutoScroll = true;
static bool s_logScrollToBottom = false;
static size_t s_logLastSize = 0;

// ============ File Browser State ============
static char s_currentPath[MAX_PATH] = "";
static char s_pathInput[MAX_PATH] = "";
static bool s_pathEditMode = false;
static bool s_pathEditModeJustActivated = false;
static std::vector<std::string> s_navHistory;
static int s_navPos = -1;
static bool s_navigating = false;
static std::vector<std::string> s_folderHistory;
static std::vector<MidiPlayer::FileEntry> s_fileList;
static int s_selectedFileIndex = -1;
static std::string s_currentPlayingFilePath;
static std::map<std::string, float> s_pathScrollPositions;
static std::map<std::string, MidiPlayer::TextScrollState> s_tagScrollStates;
static char s_histFilter[128] = "";
static char s_fileBrowserFilter[256] = "";
static int s_histSortMode = 0;  // 0=time, 1=frequency
static std::vector<std::string> s_playlist;
static int s_playlistIndex = -1;
static bool s_autoPlayNext = true;
static bool s_isSequentialPlayback = true;

// ============ UI Collapse State ============
static bool s_snPlayerCollapsed = false;
static bool s_snHistoryCollapsed = false;
static bool s_showTestPopup = false;

// ============ Config ============
static char s_configPath[MAX_PATH] = "";

// ============ Forward Declarations ============
static void sn76489_mute_all(void);
static void sn76489_set_noise(uint8_t ntype, uint8_t shift_freq);
static void sn76489_set_tone(uint8_t ch, uint16_t period);
static void InitHardware(void);
static void ResetState(void);
static void StopTest(void);
static void LoadConfig(void);
static void SaveConfig(void);
static void GetExeDir(char* buf, int bufSize);
static void AddToFolderHistory(const char* path);
static void RefreshFileList(void);
static void NavigateTo(const char* rawPath);
static void NavBack(void);
static void NavForward(void);
static void NavToParent(void);
static void UpdateChannelLevels(void);
static void RenderPianoKeyboard(void);
static void RenderLevelMeters(void);
static void RenderScopeArea(void);
static void RenderTestPopup(void);

// ============ Log ============
static void DcLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_log += buf;
    if (s_log.size() > 64000) s_log = s_log.substr(s_log.size() - 32000);
    s_logScrollToBottom = true;
}

// ============ DrawScrollingText ============
static void DrawScrollingText(const char* id, const char* text, ImU32 col, float maxWidth = 0.0f) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float availWidth = (maxWidth > 0.0f) ? maxWidth : ImGui::GetContentRegionAvail().x;
    bool needsScrolling = textSize.x > availWidth;

    if (needsScrolling) {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::Dummy(ImVec2(availWidth, lineH));

        auto& st = s_tagScrollStates[id];
        if (st.lastUpdateTime.time_since_epoch().count() == 0) {
            st.scrollOffset = 0.0f; st.scrollDirection = 1.0f;
            st.pauseTimer = 1.0f;
            st.lastUpdateTime = std::chrono::steady_clock::now();
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - st.lastUpdateTime).count();
        st.lastUpdateTime = now;
        if (st.pauseTimer > 0.0f) st.pauseTimer -= dt;
        else {
            float scrollSpeed = 30.0f;
            st.scrollOffset += st.scrollDirection * scrollSpeed * dt;
            float maxScroll = textSize.x - availWidth + 20.0f;
            if (st.scrollOffset >= maxScroll) { st.scrollOffset = maxScroll; st.scrollDirection = -1.0f; st.pauseTimer = 1.0f; }
            else if (st.scrollOffset <= 0.0f) { st.scrollOffset = 0.0f; st.scrollDirection = 1.0f; st.pauseTimer = 1.0f; }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + lineH), true);
        dl->AddText(ImVec2(cursorPos.x - st.scrollOffset, cursorPos.y), col, text);
        dl->PopClipRect();
    } else {
        ImGui::TextColored(ImVec4(
            ((col >>  0) & 0xFF) / 255.0f,
            ((col >>  8) & 0xFF) / 255.0f,
            ((col >> 16) & 0xFF) / 255.0f,
            ((col >> 24) & 0xFF) / 255.0f), "%s", text);
        s_tagScrollStates.erase(id);
    }
}

// ============ Hardware Helpers ============
static FILE* sn_fopen(const char* path, const char* mode) {
    std::wstring wPath = MidiPlayer::UTF8ToWide(std::string(path));
    std::wstring wMode = MidiPlayer::UTF8ToWide(std::string(mode));
    return _wfopen(wPath.c_str(), wMode.c_str());
}

static void sn76489_write(uint8_t data) {
    ::spfm_write_data(0, data);
    ::spfm_hw_wait(3);
}

static void sn76489_write2(uint8_t data) {
    ::spfm_write_data(1, data);
    ::spfm_hw_wait(3);
}

static void safe_flush(void) {
    ::spfm_flush();
}

static void sn76489_set_tone(uint8_t ch, uint16_t period) {
    sn76489_write(sn76489_tone_latch(ch, (uint8_t)(period & 0x0F)));
    sn76489_write(sn76489_tone_data((uint8_t)(period >> 4)));
}

static void sn76489_set_vol(uint8_t ch, uint8_t vol) {
    if (ch < 3) sn76489_write(sn76489_vol_latch(ch, vol));
    else sn76489_write(sn76489_noise_vol_latch(vol));
}

static void sn76489_mute_all(void) {
    // Tone channels: activate at max vol, reset period to 0, then mute
    for (uint8_t ch = 0; ch < 3; ch++) {
        sn76489_write(sn76489_vol_latch(ch, 0x0F));   // vol = max (activate)
        sn76489_set_tone(ch, 0);                       // period = 0
    }
    sn76489_write(0x9F); sn76489_write(0xBF); sn76489_write(0xDF);  // mute
    // Noise channel: activate at max vol, set safe params, then mute
    sn76489_write(0xF0);              // noise vol = max (activate channel)
    sn76489_write(sn76489_noise_latch(0, 0));  // ntype=0, shift_freq=0
    sn76489_write(0xFF);              // noise vol = 0 (mute)
}

static void sn76489_mute_all2(void) {
    // 先激活所有通道再静音（和 sn76489_mute_all 一样）
    sn76489_write2(0x8F); // ch0 vol=max (activate)
    sn76489_write2(0x9F); // ch0 vol=0 (mute)
    sn76489_write2(0xAF); // ch1 vol=max (activate)
    sn76489_write2(0xBF); // ch1 vol=0 (mute)
    sn76489_write2(0xCF); // ch2 vol=max (activate)
    sn76489_write2(0xDF); // ch2 vol=0 (mute)
    sn76489_write2(0xF0); // ch3 noise vol=max (activate)
    sn76489_write2(sn76489_noise_latch(1, 0)); // white noise, shift=0
    sn76489_write2(0xFF); // ch3 noise vol=0 (mute)
}

static void sn76489_set_noise(uint8_t ntype, uint8_t shift_freq) {
    sn76489_write(sn76489_noise_latch(ntype, shift_freq));
}

// Periodic noise Ch2 frequency fix (MegaGRRL method)
// When Periodic noise uses Ch2 freq on a Sega VDP DCSG (16-bit LFSR),
// the freq must be scaled to match the different LFSR width.
// Returns true if fix was applied and hardware was written.
static bool ApplyPeriodicNoiseFix(void) {
    if (s_dcsgLfsrWidth == 15) return false;  // TI = no fix needed
    if (s_noiseType != 0) return false;       // not periodic
    if (!s_noiseUseCh2) return false;         // not using Ch2 freq
    if (s_vol[2] != 15) return false;         // Ch2 not muted (optional strictness)

    uint32_t freq = s_fullPeriod[2];
    if (freq == 0) freq = 1;
    if (s_dcsgLfsrWidth == 16) {
        freq = freq * 10625 / 10000;  // +6.25%
    } else if (s_dcsgLfsrWidth == 17) {
        freq = freq * 11333 / 10000;  // +13.33%
    }
    if (freq > 1023) freq = 1023;
    sn76489_set_tone(2, (uint16_t)freq);
    safe_flush();
    return true;
}

static void InitHardware(void) {
    sn76489_mute_all();
    sn76489_set_noise(0, 0);
    sn76489_set_tone(0, 0); sn76489_set_tone(1, 0); sn76489_set_tone(2, 0);
    if (s_connected2) sn76489_mute_all2();
    safe_flush();
}

static void ResetState(void) {
    s_testRunning = false; s_testType = 0; s_testStep = 0; s_testStepMs = 0.0;
    s_vol[0] = s_vol[1] = s_vol[2] = s_vol[3] = 15;
    s_noiseType = 0; s_noiseFreq = 0; s_noiseUseCh2 = false; s_dcsgLfsrWidth = 16;
    s_fullPeriod[0] = s_fullPeriod[1] = s_fullPeriod[2] = 0;
}

// ============ Connection ============
static void ConnectHardware(void) {
    if (s_connected) return;
    StopTest();
    if (s_vgmPlaying) { s_vgmPlaying = false; }
    if (YM2163::g_hardwareConnected) YM2163::DisconnectHardware();
    YM2163::g_manualDisconnect = true;
    if (spfm_init(0) == 0) {
        s_connected = true; s_manualDisconnect = false;
        ResetState(); InitHardware();
        DcLog("[SN] Hardware connected\n");
    } else {
        YM2163::g_manualDisconnect = false;
        DcLog("[SN] Hardware connection failed\n");
    }
}

static void DisconnectHardware(void) {
    if (s_testRunning) s_testRunning = false;
    if (s_vgmPlaying) s_vgmPlaying = false;
    if (s_connected) {
        InitHardware();
        safe_flush();
        Sleep(50);
        spfm_cleanup();
        s_connected = false; s_connected2 = false; s_manualDisconnect = true;
    }
    YM2163::g_manualDisconnect = false;
    DcLog("[SN] Hardware disconnected\n");
}

// ============ Config Persistence ============
static void GetExeDir(char* buf, int bufSize) {
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(wbuf, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    std::string s = MidiPlayer::WideToUTF8(std::wstring(wbuf));
    snprintf(buf, bufSize, "%s", s.c_str());
}

static void LoadConfig(void) {
    char exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    snprintf(s_configPath, MAX_PATH, "%s\\sn76489_config.ini", exeDir);

    char buf[MAX_PATH] = "";
    GetPrivateProfileStringA("Settings", "CurrentPath", "", buf, MAX_PATH, s_configPath);
    if (buf[0]) snprintf(s_currentPath, MAX_PATH, "%s", buf);

    s_snPlayerCollapsed = GetPrivateProfileIntA("Settings", "PlayerCollapsed", 0, s_configPath) != 0;
    s_snHistoryCollapsed = GetPrivateProfileIntA("Settings", "HistoryCollapsed", 0, s_configPath) != 0;
    s_autoPlayNext = GetPrivateProfileIntA("Settings", "AutoPlayNext", 1, s_configPath) != 0;
    s_isSequentialPlayback = GetPrivateProfileIntA("Settings", "SequentialPlayback", 1, s_configPath) != 0;

    // Folder history
    s_folderHistory.clear();
    for (int i = 0; i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        char val[MAX_PATH] = "";
        GetPrivateProfileStringA("SnFolderHistory", key, "", val, MAX_PATH, s_configPath);
        if (val[0] == '\0') break;
        s_folderHistory.push_back(std::string(val));
    }

    DcLog("[SN] Config loaded\n");

    // Channel colors
    for (int i = 0; i < 10; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ChColor%d", i);
        char val[32] = "";
        GetPrivateProfileStringA("Colors", key, "", val, sizeof(val), s_configPath);
        if (val[0]) {
            unsigned int c = (unsigned int)strtoul(val, NULL, 16);
            if (c > 0) kChColorsCustom[i] = (ImU32)c;
        }
    }

    // Shift note map
    for (int i = 0; i < 3; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ShiftNote%d", i);
        s_shiftNoteMap[i] = GetPrivateProfileIntA("Noise", key, s_shiftNoteMap[i], s_configPath);
        if (s_shiftNoteMap[i] < 0) s_shiftNoteMap[i] = 0;
        if (s_shiftNoteMap[i] > 127) s_shiftNoteMap[i] = 127;
    }

    // Seek & fadeout
    s_seekMode = GetPrivateProfileIntA("Settings", "SeekMode", 0, s_configPath);
    if (s_seekMode < 0 || s_seekMode > 1) s_seekMode = 0;
    {
        char val[32] = "";
        GetPrivateProfileStringA("Settings", "FadeoutDuration", "3.0", val, sizeof(val), s_configPath);
        s_fadeoutDuration = (float)atof(val);
        if (s_fadeoutDuration < 0.0f) s_fadeoutDuration = 0.0f;
    }
}

static void SaveConfig(void) {
    WritePrivateProfileStringA("Settings", "CurrentPath", s_currentPath, s_configPath);
    WritePrivateProfileStringA("Settings", "PlayerCollapsed", s_snPlayerCollapsed ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "HistoryCollapsed", s_snHistoryCollapsed ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "AutoPlayNext", s_autoPlayNext ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "SequentialPlayback", s_isSequentialPlayback ? "1" : "0", s_configPath);

    WritePrivateProfileStringA("SnFolderHistory", NULL, NULL, s_configPath);
    for (int i = 0; i < (int)s_folderHistory.size() && i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        WritePrivateProfileStringA("SnFolderHistory", key, s_folderHistory[i].c_str(), s_configPath);
    }

    // Channel colors
    for (int i = 0; i < 10; i++) {
        char key[64], val[32];
        snprintf(key, sizeof(key), "ChColor%d", i);
        if (kChColorsCustom[i] != 0) {
            snprintf(val, sizeof(val), "%08X", kChColorsCustom[i]);
            WritePrivateProfileStringA("Colors", key, val, s_configPath);
        }
    }

    // Shift note map
    for (int i = 0; i < 3; i++) {
        char key[64], val[32];
        snprintf(key, sizeof(key), "ShiftNote%d", i);
        snprintf(val, sizeof(val), "%d", s_shiftNoteMap[i]);
        WritePrivateProfileStringA("Noise", key, val, s_configPath);
    }

    // Seek & fadeout
    WritePrivateProfileStringA("Settings", "SeekMode", std::to_string(s_seekMode).c_str(), s_configPath);
    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f", s_fadeoutDuration);
        WritePrivateProfileStringA("Settings", "FadeoutDuration", val, s_configPath);
    }
}

// ============ File Browser ============
static bool IsSupportedExt(const wchar_t* filename) {
    const wchar_t* dot = wcsrchr(filename, L'.');
    if (!dot) return false;
    return (_wcsicmp(dot, L".vgm") == 0 || _wcsicmp(dot, L".vgz") == 0);
}

static void AddToFolderHistory(const char* path) {
    for (int i = (int)s_folderHistory.size() - 1; i >= 0; i--) {
        if (s_folderHistory[i] == std::string(path)) {
            s_folderHistory.erase(s_folderHistory.begin() + i);
            break;
        }
    }
    s_folderHistory.insert(s_folderHistory.begin(), std::string(path));
    if (s_folderHistory.size() > 50) s_folderHistory.resize(50);
}

static void RefreshFileList(void) {
    s_fileList.clear();
    s_selectedFileIndex = -1;
    s_playlist.clear();
    s_playlistIndex = -1;

    // Parent entry
    if (strlen(s_currentPath) > 3) {
        MidiPlayer::FileEntry parent;
        parent.name = "..";
        parent.fullPath = "";
        parent.isDirectory = true;
        s_fileList.push_back(parent);
    }

    std::wstring wCurrentPath = MidiPlayer::UTF8ToWide(s_currentPath);
    std::wstring wSearchPath = wCurrentPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<MidiPlayer::FileEntry> dirs, files;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        MidiPlayer::FileEntry entry;
        entry.name = MidiPlayer::WideToUTF8(std::wstring(fd.cFileName));
        std::wstring wFullPath = wCurrentPath + L"\\" + fd.cFileName;
        entry.fullPath = MidiPlayer::WideToUTF8(wFullPath);
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (entry.isDirectory) {
            dirs.push_back(entry);
        } else if (IsSupportedExt(fd.cFileName)) {
            files.push_back(entry);
            s_playlist.push_back(entry.fullPath);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(dirs.begin(), dirs.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b) { return a.name < b.name; });
    std::sort(files.begin(), files.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b) { return a.name < b.name; });
    for (auto& d : dirs) s_fileList.push_back(d);
    for (auto& f : files) s_fileList.push_back(f);
}

static void NavigateTo(const char* rawPath) {
    std::wstring wRaw = MidiPlayer::UTF8ToWide(rawPath);
    wchar_t wCanon[MAX_PATH];
    if (_wfullpath(wCanon, wRaw.c_str(), MAX_PATH) == nullptr)
        wcsncpy(wCanon, wRaw.c_str(), MAX_PATH);
    std::string canon = MidiPlayer::WideToUTF8(std::wstring(wCanon));
    snprintf(s_currentPath, MAX_PATH, "%s", canon.c_str());

    if (!s_navigating) {
        if (s_navPos < (int)s_navHistory.size() - 1)
            s_navHistory.erase(s_navHistory.begin() + s_navPos + 1, s_navHistory.end());
        s_navHistory.push_back(canon);
        s_navPos++;
    }
    s_navigating = false;

    RefreshFileList();
    AddToFolderHistory(s_currentPath);
    SaveConfig();
}

static void NavBack(void) {
    if (s_navPos > 0) {
        s_navPos--;
        s_navigating = true;
        NavigateTo(s_navHistory[s_navPos].c_str());
    }
}

static void NavForward(void) {
    if (s_navPos < (int)s_navHistory.size() - 1) {
        s_navPos++;
        s_navigating = true;
        NavigateTo(s_navHistory[s_navPos].c_str());
    }
}

static void NavToParent(void) {
    char parentPath[MAX_PATH];
    strncpy(parentPath, s_currentPath, MAX_PATH);
    int len = (int)strlen(parentPath);
    while (len > 0 && parentPath[len - 1] == '\\') { parentPath[--len] = '\0'; }
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        *lastSlash = '\0';
        NavigateTo(parentPath);
    }
}

// ============ Piano / Level Helpers ============
static int period_to_midi_note(uint16_t period) {
    if (period == 0) return -1;
    double freq = SN76489_CLOCK_NTSC / (32.0 * period);
    return (int)round(69.0 + 12.0 * log2(freq / 440.0));
}

static void UpdateChannelLevels(void) {
    // Clear all piano keys first
    for (int i = 0; i < SN_PIANO_KEYS; i++) {
        s_pianoKeyOn[i] = false;
        s_pianoKeyLevel[i] = 0.0f;
        s_pianoKeyChannel[i] = -1;
        s_pianoKeyNoiseType[i] = -1;
    }

    // Slot 0 (main SN76489)
    for (int ch = 0; ch < 4; ch++) {
        float target = (15.0f - (float)s_vol[ch]) / 15.0f;
        s_channelLevel[ch] += (target - s_channelLevel[ch]) * 0.3f;
        if (s_channelLevel[ch] < 0.001f) s_channelLevel[ch] = 0.0f;

        if (ch < 3 && s_channelLevel[ch] > 0.01f) {
            int midi = period_to_midi_note(s_fullPeriod[ch]);
            if (midi >= SN_PIANO_LOW && midi <= SN_PIANO_HIGH) {
                int idx = midi - SN_PIANO_LOW;
                if (!s_pianoKeyOn[idx] || s_channelLevel[ch] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = s_channelLevel[ch];
                    s_pianoKeyChannel[idx] = ch; // 0-2
                }
            }
        } else if (ch == 3 && s_channelLevel[3] > 0.01f) {
            // Noise channel piano (参考 VGM: white noise→固定音高, periodic→ch2 period)
            int midi = -1;
            if (s_noiseType == 1) {
                // 白噪音: 固定映射音符
                midi = s_shiftNoteMap[0];
            } else if (s_noiseUseCh2) {
                // 周期性噪音 ch2 模式: LFSR 输出频率 = 方波频率 / 16，低3个八度
                if (s_fullPeriod[2] > 0)
                    midi = period_to_midi_note(s_fullPeriod[2]) - 36;
            } else {
                // 周期性噪音 shift 模式: 用分频映射
                midi = s_shiftNoteMap[s_noiseFreq & 3];
            }
            if (midi >= SN_PIANO_LOW && midi <= SN_PIANO_HIGH) {
                int idx = midi - SN_PIANO_LOW;
                if (!s_pianoKeyOn[idx] || s_channelLevel[3] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = s_channelLevel[3];
                    s_pianoKeyChannel[idx] = 3; // slot0 noise
                    s_pianoKeyNoiseType[idx] = s_noiseType;
                }
            }
        }
    }

    // Slot 1 (2nd SN76489)
    for (int ch = 0; ch < 4; ch++) {
        float target = (15.0f - (float)s2_vol[ch]) / 15.0f;
        s2_channelLevel[ch] += (target - s2_channelLevel[ch]) * 0.3f;
        if (s2_channelLevel[ch] < 0.001f) s2_channelLevel[ch] = 0.0f;

        if (ch < 3 && s2_channelLevel[ch] > 0.01f) {
            int midi = period_to_midi_note(s2_fullPeriod[ch]);
            if (midi >= SN_PIANO_LOW && midi <= SN_PIANO_HIGH) {
                int idx = midi - SN_PIANO_LOW;
                if (!s_pianoKeyOn[idx] || s2_channelLevel[ch] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = s2_channelLevel[ch];
                    s_pianoKeyChannel[idx] = ch + 4; // 4-6
                }
            }
        } else if (ch == 3 && s2_channelLevel[3] > 0.01f) {
            // Slot1 noise (参考 VGM: white→固定, periodic→ch2 period)
            int midi = -1;
            if (s2_noiseType == 1) {
                midi = s_shiftNoteMap[0];
            } else if (s2_noiseUseCh2) {
                if (s2_fullPeriod[2] > 0)
                    midi = period_to_midi_note(s2_fullPeriod[2]) - 36;
            } else {
                midi = s_shiftNoteMap[s2_noiseFreq & 3];
            }
            if (midi >= SN_PIANO_LOW && midi <= SN_PIANO_HIGH) {
                int idx = midi - SN_PIANO_LOW;
                if (!s_pianoKeyOn[idx] || s2_channelLevel[3] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = s2_channelLevel[3];
                    s_pianoKeyChannel[idx] = 7; // slot1 noise
                    s_pianoKeyNoiseType[idx] = s2_noiseType;
                }
            }
        }
    }
}

// ============ VGM Player ============
static UINT32 ReadLE32(FILE* f) {
    UINT8 b[4]; fread(b, 1, 4, f);
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

static std::string ReadGD3String(const UINT8*& ptr, const UINT8* end) {
    std::string result;
    while (ptr + 1 < end) {
        UINT16 ch = ptr[0] | (ptr[1] << 8); ptr += 2;
        if (ch == 0) break;
        if (ch < 128) result += (char)ch;
        else if (ch < 0x800) { result += (char)(0xC0 | (ch >> 6)); result += (char)(0x80 | (ch & 0x3F)); }
        else { result += (char)(0xE0 | (ch >> 12)); result += (char)(0x80 | ((ch >> 6) & 0x3F)); result += (char)(0x80 | (ch & 0x3F)); }
    }
    if ((ptr - (end - (end - ptr))) % 2 != 0) ptr++;
    return result;
}

static void ParseGD3Tags(FILE* f, UINT32 offset) {
    if (offset == 0) return;
    fseek(f, offset, SEEK_SET);
    char sig[4]; fread(sig, 1, 4, f);
    if (memcmp(sig, "gd3", 3) != 0) return;
    fseek(f, offset + 4, SEEK_SET);
    UINT32 strLen = ReadLE32(f);
    UINT32 dataOff = offset + 12;
    fseek(f, dataOff, SEEK_SET);
    std::vector<UINT8> buf(strLen);
    fread(buf.data(), 1, strLen, f);
    const UINT8* ptr = buf.data();
    const UINT8* end = buf.data() + strLen;
    s_trackName = ReadGD3String(ptr, end);
    std::string trackJp = ReadGD3String(ptr, end);
    if (s_trackName.empty() && !trackJp.empty()) s_trackName = trackJp;
    s_gameName = ReadGD3String(ptr, end);
    std::string gameJp = ReadGD3String(ptr, end);
    if (s_gameName.empty() && !gameJp.empty()) s_gameName = gameJp;
    s_systemName = ReadGD3String(ptr, end);
    std::string sysJp = ReadGD3String(ptr, end);
    if (s_systemName.empty() && !sysJp.empty()) s_systemName = sysJp;
    s_artistName = ReadGD3String(ptr, end);
    std::string artJp = ReadGD3String(ptr, end);
    if (s_artistName.empty() && !artJp.empty()) s_artistName = artJp;
}

static bool LoadVGMFile(const char* path) {
    StopTest();
    if (s_vgmPlaying) { s_vgmPlaying = false; s_vgmPaused = false; }
    if (s_vgmFile) { fclose(s_vgmFile); s_vgmFile = nullptr; }
    s_vgmLoaded = false;
    s_trackName.clear(); s_gameName.clear(); s_systemName.clear(); s_artistName.clear();
    s_vgmTotalSamples = 0; s_vgmCurrentSamples = 0;
    s_vgmTrackEnded = false;
    s_isT6W28 = false;
    s_t6w28NoiseCh2Period = 0;
    s_t6w28LastLatchReg = -1;
    s2_vol[0] = s2_vol[1] = s2_vol[2] = s2_vol[3] = 15;
    s2_noiseType = 0; s2_noiseFreq = 0; s2_noiseUseCh2 = false;
    s2_fullPeriod[0] = s2_fullPeriod[1] = s2_fullPeriod[2] = 0;
    s2_lastToneLatchCh = -1;

    FILE* f = sn_fopen(path, "rb");
    if (!f) { DcLog("[VGM] Cannot open: %s\n", path); return false; }

    char sig[4]; fread(sig, 1, 4, f);
    if (memcmp(sig, "Vgm ", 4) != 0) { fclose(f); DcLog("[VGM] Not a VGM file\n"); return false; }

    fseek(f, 0x08, SEEK_SET);
    s_vgmVersion = ReadLE32(f);
    UINT32 snClock = ReadLE32(f);
    s_isT6W28 = (snClock & 0x80000000) != 0;
    snClock &= ~0x80000000;
    fseek(f, 0x14, SEEK_SET);
    UINT32 gd3RelOff = ReadLE32(f);
    UINT32 gd3Off = gd3RelOff ? (gd3RelOff + 0x14) : 0;  // relative offset from 0x14
    s_vgmTotalSamples = ReadLE32(f);
    UINT32 loopRelOff = ReadLE32(f);
    s_vgmLoopOffset = loopRelOff ? (loopRelOff + 0x1C) : 0;  // relative offset from 0x1C
    s_vgmLoopSamples = ReadLE32(f);

    UINT32 dataOff = 0x40;
    if (s_vgmVersion >= 0x150) {
        fseek(f, 0x34, SEEK_SET);
        UINT32 hdrDataOff = ReadLE32(f);
        if (hdrDataOff > 0) dataOff = hdrDataOff + 0x34;
    }
    s_vgmDataOffset = dataOff;

    DcLog("[VGM] hdr: ver=0x%X dataAbs=0x%X loopAbs=0x%X gd3Abs=0x%X T6W28=%d\n",
        s_vgmVersion, s_vgmDataOffset, s_vgmLoopOffset, gd3Off, s_isT6W28);

    ParseGD3Tags(f, gd3Off);

    fclose(f);
    s_vgmFile = sn_fopen(path, "rb");
    if (!s_vgmFile) { DcLog("[VGM] Reopen failed\n"); return false; }

    snprintf(s_vgmPath, MAX_PATH, "%s", path);
    s_vgmLoaded = true;
    s_vgmCurrentSamples = 0;
    s_vgmLoopCount = 0;
    s_vgmPaused = false;
    s_currentPlayingFilePath = path;
    for (int i = 0; i < (int)s_playlist.size(); i++) {
        if (s_playlist[i] == std::string(path)) { s_playlistIndex = i; break; }
    }

    DcLog("[VGM] Loaded: %s\n", path);
    DcLog("[VGM] Clock=%u Total=%.1fs\n", snClock, (double)s_vgmTotalSamples / 44100.0);
    if (!s_trackName.empty()) DcLog("[VGM] Track: %s\n", s_trackName.c_str());
    if (!s_gameName.empty()) DcLog("[VGM] Game: %s\n", s_gameName.c_str());
    return true;
}

static int s_vgmCmdCount = 0;

// VGM Command Length Table — from libvgm _CMD_INFO[0x100] (vgmplayer_cmdhandler.cpp)
// cmdLen = total bytes including opcode. 0 = invalid/special.
static const UINT8 VGM_CMD_LEN[0x100] = {
    // 0x00-0x1F: invalid
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x20-0x2F: 20=AY8910(3), 22-2F=unknown(2)
    0x03,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    // 0x30-0x3F: 30=SN76489_2nd(2), 31=AY8910_stereo(2), 3F=GG_stereo_2nd(2)
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    // 0x40-0x4F: 40=Mikey(3), 4F=GG_stereo(2)
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x02,
    // 0x50-0x5F: 50=SN76489(2), 51-5F=YM chips(3)
    0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    // 0x60-0x6F: 61=wait_N(3), 62=735(1), 63=882(1), 66=end(special), 67=datablock(special), 68=PCM_RAM(12)
    0x00,0x03,0x01,0x01,0x00,0x00,0x00,0x00, 0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x70-0x7F: short wait (op&0x0F)+1 samples, 1 byte each
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    // 0x80-0x8F: YM2612 DAC write + wait (2 bytes each)
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    // 0x90-0x9F: DAC stream control
    0x05,0x05,0x06,0x0B,0x02,0x05, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0xA0-0xAF: 2nd chip writes (3 bytes each)
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    // 0xB0-0xBF: block chip writes (3 bytes each)
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    // 0xC0-0xCF: memory/bank writes (4-5 bytes)
    0x05,0x05,0x05,0x04,0x04,0x04,0x04,0x04, 0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    // 0xD0-0xDF: port/register writes (4-5 bytes)
    0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x04, 0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    // 0xE0-0xFF: PCM seek(5), C352(5), unknown(5)
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05, 0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05, 0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
};

// Table-driven VGM command processor based on libvgm _CMD_INFO
// Returns: wait samples (>0), 0 = no wait, -1 = EOF/error
static int VGMProcessCommand(void) {
    UINT8 cmd;
    if (fread(&cmd, 1, 1, s_vgmFile) != 1) return -1;

    if (s_vgmCmdCount < 50) {
        DcLog("[VGM] cmd=0x%02X at %ld\n", cmd, ftell(s_vgmFile) - 1);
    }

    // Special commands with unique behavior
    switch (cmd) {
        case 0x30: { // 2nd SN76489 write (T6W28 noise chip)
            if (!s_isT6W28) break;
            UINT8 data; if (fread(&data, 1, 1, s_vgmFile) != 1) return -1;
            s_vgmCmdCount++;
            // 始终提取 shadow state（不管模式）
            if (data & 0x80) {
                // Latch byte: 更新 noise 芯片内部 latch 状态
                int reg = (data >> 4) & 7;
                s_t6w28LastLatchReg = reg;
                int ch = reg >> 1;
                if (ch == 3) {
                    if (data & 0x10) {
                        s2_vol[3] = data & 0x0F;
                    } else {
                        s2_noiseType = (data >> 2) & 1;
                        uint8_t sf = data & 0x03;
                        if (sf == 3) s2_noiseUseCh2 = true;
                        else { s2_noiseUseCh2 = false; s2_noiseFreq = sf; }
                    }
                } else if (ch == 2) {
                    if (data & 0x10) {
                        s2_vol[2] = data & 0x0F;
                    } else {
                        s_t6w28NoiseCh2Period = (s_t6w28NoiseCh2Period & 0x3F0) | (data & 0x0F);
                        s2_fullPeriod[2] = (s2_fullPeriod[2] & 0x3F0) | (data & 0x0F);
                        s2_lastToneLatchCh = 2;
                    }
                } else if (ch < 2) {
                    if (data & 0x10) {
                        s2_vol[ch] = data & 0x0F;
                    } else {
                        s2_fullPeriod[ch] = (s2_fullPeriod[ch] & 0x3F0) | (data & 0x0F);
                        s2_lastToneLatchCh = ch;
                    }
                }
            } else {
                // Data byte
                if (s_t6w28LastLatchReg == 4) {
                    s_t6w28NoiseCh2Period = (s_t6w28NoiseCh2Period & 0x00F) | ((data & 0x3F) << 4);
                    s2_fullPeriod[2] = (s2_fullPeriod[2] & 0x00F) | ((data & 0x3F) << 4);
                } else if (s2_lastToneLatchCh >= 0 && s2_lastToneLatchCh < 3) {
                    s2_fullPeriod[s2_lastToneLatchCh] = (s2_fullPeriod[s2_lastToneLatchCh] & 0x00F) | ((data & 0x3F) << 4);
                }
            }
            // 屏蔽 slot1 通道（0x30 的通道偏移 +4）
            if ((data & 0x80) && (data & 0x10)) {
                int ch = (data >> 5) & 3;
                if (s_chMuted[4 + ch]) {
                    data = (data & 0xF0) | 0x0F;
                }
            }
            if (!s_connected) return 0;
            if (s_t6w28Mode == 0) {
                // Passthrough: 全部转发到 slot0
                sn76489_write(data); safe_flush();
            } else if (s_t6w28Mode == 2) {
                // Dual Chip: slot1 只接收噪音相关数据
                if (s_connected2) {
                    if ((data & 0x80)) {
                        int ch = (data >> 5) & 3;
                        bool isVol = (data & 0x10) != 0;
                        if (ch == 0 || ch == 1) {
                            // ch0/1 方波: 始终屏蔽
                        } else if (ch == 2) {
                            if (isVol) {
                                // ch2 方波音量: 不发（无意义）
                            } else if (s2_noiseUseCh2) {
                                // ch2 tone: 只在噪音用 ch2 模式时传
                                sn76489_write2(data); safe_flush();
                            }
                        } else {
                            // ch3 噪音: 始终传（ctrl + volume 都属于 noise chip）
                            sn76489_write2(data); safe_flush();
                        }
                    } else {
                        // Data byte: 只在 lastLatch 是 ch2 tone 或 ch3 时传
                        if (s_t6w28LastLatchReg == 4 && s2_noiseUseCh2) {
                            sn76489_write2(data); safe_flush();
                        } else if (s_t6w28LastLatchReg >= 6) {
                            sn76489_write2(data); safe_flush();
                        }
                    }
                }
            } else {
                // Force SF2 (mode 1): 只转发 ch3 噪音到 slot0，跳过 ch0-2 方波
                if ((data & 0x80)) {
                    int ch = (data >> 5) & 3;
                    if (ch == 3) {
                        if (s2_noiseUseCh2) {
                            uint8_t sf = 1;
                            if (s_t6w28NoiseCh2Period > 0) {
                                if (s_t6w28NoiseCh2Period < 24) sf = 0;
                                else if (s_t6w28NoiseCh2Period < 48) sf = 1;
                                else sf = 2;
                            }
                            s_noiseFreq = sf;
                            uint8_t nctrl = 0xE0 | (s2_noiseType << 2) | sf;
                            sn76489_write(nctrl); safe_flush();
                        } else {
                            sn76489_write(data); safe_flush();
                        }
                    }
                }
            }
            return 0;
        }
        case 0x50: { // SN76489 write (libvgm: Cmd_SN76489, 2 bytes)
            UINT8 data; if (fread(&data, 1, 1, s_vgmFile) != 1) return -1;
            s_vgmCmdCount++;
            bool tone2Updated = false;
            bool noiseCtrlUpdated = false;
            bool tone2VolUpdated = false;
            if (data & 0x80) {
                int ch = (data >> 5) & 3;
                if (data & 0x10) {
                    if (ch < 4) {
                        // Dual Chip: ch3 噪音音量不发到 slot0，不更新 shadow
                        if (!(s_t6w28Mode == 2 && s_isT6W28 && ch == 3)) {
                            s_vol[ch] = data & 0x0F;
                            if (ch == 2) tone2VolUpdated = true;
                        }
                    }
                } else if (ch == 3) {
                    // Dual Chip: ch3 噪音控制不发到 slot0，不更新 shadow
                    if (!(s_t6w28Mode == 2 && s_isT6W28)) {
                        s_noiseType = (data >> 2) & 1;
                        uint8_t sf = data & 0x03;
                        if (sf == 3) { s_noiseUseCh2 = true; }
                        else { s_noiseUseCh2 = false; s_noiseFreq = sf; }
                        noiseCtrlUpdated = true;
                    }
                } else {
                    if (ch < 3) { s_fullPeriod[ch] = (s_fullPeriod[ch] & 0x3F0) | (data & 0x0F); s_lastToneLatchCh = ch; if (ch == 2) tone2Updated = true; }
                }
            } else {
                if (s_lastToneLatchCh < 3) { s_fullPeriod[s_lastToneLatchCh] = (s_fullPeriod[s_lastToneLatchCh] & 0x0F) | ((data & 0x3F) << 4); if (s_lastToneLatchCh == 2) tone2Updated = true; }
            }
            // Hardware write (with channel mute support)
            if (s_connected) {
                // 屏蔽通道：音量 latch 时替换为静音
                if ((data & 0x80) && (data & 0x10)) {
                    int ch = (data >> 5) & 3;
                    if (s_chMuted[ch]) {
                        data = (data & 0xF0) | 0x0F;  // vol=15 (mute)
                    }
                }
                if (s_t6w28Mode == 2 && s_isT6W28) {
                    // Dual Chip: slot0 只接收 tone (ch0-2)，屏蔽噪音 (ch3)
                    if (data & 0x80) {
                        int ch = (data >> 5) & 3;
                        if (ch != 3) {
                            sn76489_write(data); safe_flush();
                        }
                    } else {
                        sn76489_write(data); safe_flush();
                    }
                } else if (tone2Updated && ApplyPeriodicNoiseFix()) {
                    // Fix already wrote corrected Tone2 freq, skip original write
                } else {
                    sn76489_write(data);
                    safe_flush();
                    // After noise ctrl or tone2 vol update, re-apply fix
                    if ((noiseCtrlUpdated || tone2VolUpdated) && ApplyPeriodicNoiseFix()) {}
                }
            }
            return 0;
        }
        case 0x61: { // Wait N samples (libvgm: Cmd_DelaySamples2B, 3 bytes)
            UINT16 wait; if (fread(&wait, 1, 2, s_vgmFile) != 2) return -1;
            return wait;
        }
        case 0x62: return 735;  // Wait 735 samples (libvgm: Cmd_Delay60Hz)
        case 0x63: return 882;  // Wait 882 samples (libvgm: Cmd_Delay50Hz)
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: // Short wait (libvgm: Cmd_DelaySamplesN1)
            return (cmd & 0x0F) + 1;
        case 0x66: { // End of data (libvgm: Cmd_EndOfData)
            if (s_vgmLoopOffset > 0 && (s_vgmMaxLoops == 0 || s_vgmLoopCount < s_vgmMaxLoops)) {
                // 淡出触发：倒数第二次 0x66（进入最后一遍循环时）
                if (s_vgmMaxLoops > 0 && s_vgmLoopCount >= s_vgmMaxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeoutActive) {
                    s_fadeoutActive = true;
                    s_fadeoutLevel = 1.0f;
                    s_fadeoutStartSample = s_vgmCurrentSamples;
                    UINT32 fadeoutSamples = (UINT32)(s_fadeoutDuration * 44100.0);
                    // 淡出长度限制在一遍循环内
                    if (s_vgmLoopSamples > 0 && fadeoutSamples > s_vgmLoopSamples)
                        fadeoutSamples = s_vgmLoopSamples;
                    s_fadeoutEndSample = s_vgmCurrentSamples + fadeoutSamples;
                }
                fseek(s_vgmFile, s_vgmLoopOffset, SEEK_SET);
                s_vgmLoopCount++;
                return 0;
            }
            return -1;
        }
        case 0x67: { // Data block (libvgm: Cmd_DataBlock, variable length)
            UINT8 compat; if (fread(&compat, 1, 1, s_vgmFile) != 1) return -1;
            if (compat != 0x66) return -1;
            UINT8 type; if (fread(&type, 1, 1, s_vgmFile) != 1) return -1;
            UINT32 size; if (fread(&size, 4, 1, s_vgmFile) != 1) return -1;
            fseek(s_vgmFile, size, SEEK_CUR);
            return 0;
        }
    }

    // All other commands: skip using VGM_CMD_LEN table
    UINT8 cmdLen = VGM_CMD_LEN[cmd];
    if (cmdLen <= 1) return 0; // 0=invalid/1=opcode only, nothing to skip
    UINT8 skip = cmdLen - 1;
    if (fseek(s_vgmFile, skip, SEEK_CUR) != 0) return -1;
    return 0;
}

static DWORD WINAPI VGMPlaybackThread(LPVOID) {
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double samplesPerTick = 44100.0 / freq.QuadPart;
    double samplesToProcess = 0.0;

    while (s_vgmThreadRunning && s_vgmPlaying) {
        if (s_vgmPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        samplesToProcess += (now.QuadPart - last.QuadPart) * samplesPerTick;
        last = now;

        int run = (int)samplesToProcess;
        if (run > 0) {
            int processed = 0;
            while (processed < run && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
                int s = VGMProcessCommand();
                if (s < 0) {
                    s_vgmTrackEnded = true;
                    s_vgmPlaying = false;
                    break;
                }
                if (s > 0) processed += s;
            }
            samplesToProcess -= processed;
            s_vgmCurrentSamples += processed;

            // 淡出音量覆盖
            if (s_fadeoutActive && s_fadeoutDuration > 0) {
                UINT32 fadeRange = s_fadeoutEndSample - s_fadeoutStartSample;
                if (fadeRange == 0) fadeRange = 1;
                float progress = (float)(s_vgmCurrentSamples - s_fadeoutStartSample) / (float)fadeRange;
                if (s_vgmCurrentSamples >= s_fadeoutEndSample) {
                    s_fadeoutLevel = 0.0f;
                    s_fadeoutActive = false;
                } else {
                    s_fadeoutLevel = 1.0f - progress;
                }
                // 每 ~10ms 发送一次淡出音量
                static UINT32 lastFadeSample = 0;
                if (s_vgmCurrentSamples - lastFadeSample >= 441 || s_fadeoutLevel <= 0.0f) {
                    lastFadeSample = s_vgmCurrentSamples;
                    if (s_connected) {
                        for (int ch = 0; ch < 3; ch++) {
                            uint8_t vol = (uint8_t)(s_vol[ch] + (15 - s_vol[ch]) * (1.0f - s_fadeoutLevel));
                            sn76489_write(sn76489_vol_latch(ch, vol));
                        }
                        uint8_t nvol = (uint8_t)(s_vol[3] + (15 - s_vol[3]) * (1.0f - s_fadeoutLevel));
                        sn76489_write(sn76489_noise_vol_latch(nvol));
                    }
                    if (s_connected2) {
                        for (int ch = 0; ch < 3; ch++) {
                            uint8_t vol = (uint8_t)(s2_vol[ch] + (15 - s2_vol[ch]) * (1.0f - s_fadeoutLevel));
                            sn76489_write2(sn76489_vol_latch(ch, vol));
                        }
                        uint8_t nvol = (uint8_t)(s2_vol[3] + (15 - s2_vol[3]) * (1.0f - s_fadeoutLevel));
                        sn76489_write2(sn76489_noise_vol_latch(nvol));
                    }
                }
                // 淡出完成后立即结束播放
                if (s_fadeoutLevel <= 0.0f) {
                    s_vgmTrackEnded = true;
                    s_vgmPlaying = false;
                }
            }

            safe_flush();
        }
        Sleep(1);
    }
    safe_flush();
    s_vgmThreadRunning = false;
    return 0;
}

static void StartVGMPlayback(void) {
    if (!s_vgmLoaded) return;
    DcLog("[VGM] StartPlayback: loaded=%d connected=%d dataOff=0x%X\n", s_vgmLoaded, s_connected, s_vgmDataOffset);
    StopTest();
    // Ensure any previous playback thread is fully stopped before starting a new one
    s_vgmPlaying = false;
    s_vgmThreadRunning = false;
    if (s_vgmThread) {
        WaitForSingleObject(s_vgmThread, 2000);
        CloseHandle(s_vgmThread);
        s_vgmThread = nullptr;
    }
    if (s_vgmFile) { fclose(s_vgmFile); s_vgmFile = sn_fopen(s_vgmPath, "rb"); }
    if (!s_vgmFile) { DcLog("[VGM] Reopen failed in StartPlayback\n"); return; }
    fseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
    if (s_connected) InitHardware();
    s_vgmPlaying = true; s_vgmPaused = false; s_vgmTrackEnded = false;
    s_vgmCmdCount = 0;
    s_vgmCurrentSamples = 0;
    s_vgmLoopCount = 0;
    s_vgmThreadRunning = true;
    s_vgmThread = CreateThread(NULL, 0, VGMPlaybackThread, NULL, 0, NULL);
    DcLog("[VGM] Playing\n");
}

static void StopVGMPlayback(void) {
    s_vgmPlaying = false; s_vgmPaused = false;
    s_vgmThreadRunning = false;
    s_fadeoutActive = false; s_fadeoutLevel = 1.0f;
    if (s_vgmThread) {
        WaitForSingleObject(s_vgmThread, 2000);
        CloseHandle(s_vgmThread);
        s_vgmThread = nullptr;
    }
    if (s_connected) InitHardware();
    DcLog("[VGM] Stopped at %.1fs\n", (double)s_vgmCurrentSamples / 44100.0);
}

static void PauseVGMPlayback(void) {
    if (!s_vgmPlaying) return;
    s_vgmPaused = !s_vgmPaused;
}

static void OpenVGMFileDialog(void) {
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "VGM Files\0*.vgm;*.vgz\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) LoadVGMFile(path);
}

static void SeekVGMToStart(void) {
    if (!s_vgmLoaded || !s_vgmFile) return;
    if (s_vgmPlaying) { s_vgmPlaying = false; s_vgmPaused = false; }
    if (s_connected) InitHardware();
    fclose(s_vgmFile);
    s_vgmFile = sn_fopen(s_vgmPath, "rb");
    if (!s_vgmFile) return;
    fseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
    s_vgmCurrentSamples = 0; s_vgmLoopCount = 0;
    s_vgmTrackEnded = false;
    s_fadeoutActive = false; s_fadeoutLevel = 1.0f;
}

static void PlayPlaylistNext(void) {
    if (s_playlist.empty()) return;
    if (s_isSequentialPlayback) {
        int next = s_playlistIndex + 1;
        if (next >= (int)s_playlist.size()) next = 0;
        if (LoadVGMFile(s_playlist[next].c_str())) StartVGMPlayback();
    } else {
        int next = rand() % (int)s_playlist.size();
        if (LoadVGMFile(s_playlist[next].c_str())) StartVGMPlayback();
    }
}

static void PlayPlaylistPrev(void) {
    if (s_playlist.empty()) return;
    int prev = s_playlistIndex - 1;
    if (prev < 0) prev = (int)s_playlist.size() - 1;
    if (LoadVGMFile(s_playlist[prev].c_str())) StartVGMPlayback();
}

// ============ Test Functions ============
static double GetTestElapsedMs(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_testStartTime.QuadPart) / s_perfFreq.QuadPart * 1000.0;
}

static double GetStepDurationMs(int type) {
    switch (type) {
        case 0: return 300.0; case 1: return 150.0;
        case 2: return 800.0; case 3: return 200.0; case 4: return 500.0;
        default: return 300.0;
    }
}

static void TestStep(void) {
    switch (s_testType) {
        case 0: {
            static const uint8_t notes[] = {60, 62, 64, 65, 67, 69, 71, 72};
            if (s_testStep >= 8) { s_testRunning = false; return; }
            uint16_t period = sn76489_note_to_period(SN76489_CLOCK_NTSC, notes[s_testStep]);
            sn76489_set_tone(0, period); s_fullPeriod[0] = period; break;
        }
        case 1: {
            static const uint8_t notes[] = {60, 64, 67, 72, 76, 79, 84, 79, 76, 72, 67, 64};
            if (s_testStep >= 12) { s_testRunning = false; return; }
            uint16_t period = sn76489_note_to_period(SN76489_CLOCK_NTSC, notes[s_testStep]);
            sn76489_set_tone(1, period); s_fullPeriod[1] = period; break;
        }
        case 2: {
            if (s_testStep >= 3) { s_testRunning = false; return; }
            sn76489_mute_all();
            static const struct { uint8_t n0, n1, n2; } chords[] = {{60,64,67},{65,69,72},{67,71,74}};
            sn76489_set_vol(0, 0); sn76489_set_vol(1, 0); sn76489_set_vol(2, 0);
            uint16_t p0 = sn76489_note_to_period(SN76489_CLOCK_NTSC, chords[s_testStep].n0);
            uint16_t p1 = sn76489_note_to_period(SN76489_CLOCK_NTSC, chords[s_testStep].n1);
            uint16_t p2 = sn76489_note_to_period(SN76489_CLOCK_NTSC, chords[s_testStep].n2);
            sn76489_set_tone(0, p0); s_fullPeriod[0] = p0;
            sn76489_set_tone(1, p1); s_fullPeriod[1] = p1;
            sn76489_set_tone(2, p2); s_fullPeriod[2] = p2;
            s_vol[0] = s_vol[1] = s_vol[2] = 0; break;
        }
        case 3: {
            uint16_t period = sn76489_note_to_period(SN76489_CLOCK_NTSC, 69.0);
            sn76489_set_tone(0, period); s_fullPeriod[0] = period;
            s_vol[0] = (s_testStep < 16) ? (uint8_t)s_testStep : (uint8_t)(30 - s_testStep);
            sn76489_set_vol(0, s_vol[0]);
            if (s_testStep >= 30) { s_testRunning = false; sn76489_set_vol(0, 15); return; }
            break;
        }
        case 4: {
            if (s_testStep >= 4) { s_testRunning = false; sn76489_set_vol(3, 15); return; }
            switch (s_testStep) {
                case 0: sn76489_set_noise(0, 0); s_vol[3] = 0; sn76489_set_vol(3, 0); s_noiseType = 0; s_noiseFreq = 0; break;
                case 1: sn76489_set_noise(1, 0); s_noiseType = 1; break;
                case 2: sn76489_set_noise(1, 3); s_noiseFreq = 3; break;
                case 3: for (int v = 0; v <= 15; v++) { s_vol[3] = (uint8_t)v; sn76489_set_vol(3, s_vol[3]); } return;
            }
            break;
        }
        default: s_testRunning = false; return;
    }
    safe_flush(); s_testStep++;
}

static void StopTest(void) {
    if (!s_testRunning) return;
    s_testRunning = false;
    if (s_connected) InitHardware();
    ResetState();
}

static void StartTest(int type) {
    if (!s_connected || s_vgmPlaying) return;
    StopTest();
    s_testType = type; s_testStep = 0; s_testStepMs = 0.0;
    QueryPerformanceCounter(&s_testStartTime);
    s_testRunning = true;
    switch (type) {
        case 0: sn76489_set_vol(0, 0); safe_flush(); break;
        case 1: sn76489_set_vol(1, 0); safe_flush(); break;
        case 4: sn76489_set_noise(0, 0); sn76489_set_vol(3, 0); safe_flush(); break;
    }
}

// ============ Public API ============
void Init() {
    QueryPerformanceFrequency(&s_perfFreq);
    s_scope.Init();
    LoadConfig();
    if (s_currentPath[0] != '\0') {
        RefreshFileList();
    } else {
        GetExeDir(s_currentPath, MAX_PATH);
        RefreshFileList();
    }
}

void Shutdown() {
    s_vgmPlaying = false;
    s_vgmThreadRunning = false;
    if (s_vgmThread) {
        WaitForSingleObject(s_vgmThread, 2000);
        CloseHandle(s_vgmThread);
        s_vgmThread = nullptr;
    }
    SaveConfig();
    DisconnectHardware();
}

void Update() {
    // Detect device removal when connected
    if (s_connected) {
        static int disconnectCheckCounter = 0;
        if (++disconnectCheckCounter >= 120) { // every ~2 seconds
            disconnectCheckCounter = 0;
            DWORD numDevs = 0;
            if (FT_CreateDeviceInfoList(&numDevs) != FT_OK || numDevs == 0) {
                DcLog("[SN] Device removed, disconnecting\n");
                s_vgmPlaying = false; s_vgmPaused = false;
                s_connected = false; s_manualDisconnect = false;
                ::spfm_cleanup();
                YM2163::g_manualDisconnect = false;
            }
        }
    }
    UpdateChannelLevels();
    if (s_vgmTrackEnded && !s_vgmThreadRunning && !s_vgmPlaying) {
        s_vgmTrackEnded = false;
        if (s_autoPlayNext && !s_playlist.empty()) PlayPlaylistNext();
    }

    // Update scope voice channel offsets (check periodically)
    static int scopeCheckCounter = 0;
    if (++scopeCheckCounter >= 60) {
        scopeCheckCounter = 0;
        ScopeChipSlot *slot = scope_find_slot("SN76489", 0);
        if (slot) {
            for (int i = 0; i < 4; i++) s_voiceCh[i] = slot->slot_base + i;
        } else {
            for (int i = 0; i < 4; i++) s_voiceCh[i] = -1;
        }
    }

    // Test
    if (s_testRunning) {
        double elapsed = GetTestElapsedMs();
        if (elapsed >= s_testStepMs) { TestStep(); s_testStepMs += GetStepDurationMs(s_testType); }
    }
}

// ============ Piano Keyboard ============
static ImU32 getChColor(int ch, int noiseType = -1) {
    // ch 0-2 = tone, 3 = noise (slot0), 4-6 = tone (slot1), 7 = noise (slot1)
    // noiseType: 0=periodic, 1=white, -1=use default (periodic)
    int colorIdx;
    int customIdx;
    if (ch <= 2) {
        colorIdx = ch; customIdx = ch;
    } else if (ch == 3) {
        colorIdx = (noiseType >= 0) ? 3 + noiseType : 3; // 3=periodic, 4=white
        customIdx = colorIdx;
    } else if (ch <= 6) {
        colorIdx = ch - 4 + 5; // slot1 tone: 4→5, 5→6, 6→7
        customIdx = colorIdx;
    } else { // ch == 7
        colorIdx = (noiseType >= 0) ? 3 + noiseType : 3; // 3 or 4
        customIdx = colorIdx + 5; // slot1 noise: 8=periodic, 9=white
    }
    if (customIdx >= 0 && customIdx < 10 && kChColorsCustom[customIdx] != 0) return kChColorsCustom[customIdx];
    if (colorIdx < 5) return kChColors[colorIdx];
    return kCh2Colors[colorIdx - 5];
}

static ImU32 blendKey(ImU32 col, float lv, bool isBlack) {
    float blendLv = 0.55f + lv * 0.45f;
    int r = (col >>  0) & 0xFF;
    int g = (col >>  8) & 0xFF;
    int b = (col >> 16) & 0xFF;
    int br = isBlack ? 20 : 255;
    return IM_COL32(
        br + (int)((r - br) * blendLv),
        br + (int)((g - br) * blendLv),
        br + (int)((b - br) * blendLv), 255);
}

static ImU32 getKeyColor(int idx, float level) {
    int ch = s_pianoKeyChannel[idx];
    ImU32 col = (ch >= 0) ? getChColor(ch, s_pianoKeyNoiseType[idx]) : IM_COL32(160, 200, 160, 255);
    return blendKey(col, level, false);
}

static ImU32 getKeyColorBlack(int idx, float level) {
    int ch = s_pianoKeyChannel[idx];
    ImU32 col = (ch >= 0) ? getChColor(ch, s_pianoKeyNoiseType[idx]) : IM_COL32(160, 200, 160, 255);
    return blendKey(col, level, true);
}

static void RenderPianoKeyboard(void) {
    ImGui::BeginChild("SN_Piano", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float whiteKeyH = 100.0f;
    float blackKeyH = 60.0f;

    const int kMinNote = SN_PIANO_LOW;
    const int kMaxNote = SN_PIANO_HIGH;

    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) if (!s_isBlackNote[n % 12]) numWhiteKeys++;

    float whiteKeyW = availW / (float)numWhiteKeys;
    if (whiteKeyW < 6.0f) whiteKeyW = 6.0f;
    float blackKeyW = whiteKeyW * 0.65f;

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (s_isBlackNote[n % 12]) continue;
        int idx = n - SN_PIANO_LOW;
        float x = p.x + wkIdx * whiteKeyW;
        ImU32 fillCol = s_pianoKeyOn[idx] ? getKeyColor(idx, s_pianoKeyLevel[idx]) : IM_COL32(255, 255, 255, 255);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + whiteKeyW - 1, p.y + whiteKeyH), fillCol);
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + whiteKeyW, p.y + whiteKeyH), IM_COL32(0, 0, 0, 255));
        if (n % 12 == 0) {
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "C%d", n / 12 - 1);
            dl->AddText(ImVec2(x + 2, p.y + whiteKeyH - 18), IM_COL32(0, 0, 0, 255), lbl);
        }
        wkIdx++;
    }

    // Pass 2: black keys
    wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!s_isBlackNote[n % 12]) { wkIdx++; continue; }
        int idx = n - SN_PIANO_LOW;
        float x = p.x + (wkIdx - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;
        ImU32 fillCol = (idx >= 0 && idx < SN_PIANO_KEYS && s_pianoKeyOn[idx])
            ? getKeyColorBlack(idx, s_pianoKeyLevel[idx]) : IM_COL32(0, 0, 0, 255);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), fillCol);
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), IM_COL32(128, 128, 128, 255));
    }

    ImGui::EndChild();
}

// ============ Level Meters ============
static void ApplyChannelMute(int i) {
    int chip = i / 4, ch = i % 4;
    if (s_chMuted[i]) {
        if (chip == 0 && s_connected) {
            sn76489_write(ch < 3 ? sn76489_vol_latch(ch, 0x0F) : sn76489_noise_vol_latch(0x0F));
        }
        if (chip == 1 && s_connected2) {
            sn76489_write2(ch < 3 ? sn76489_vol_latch(ch, 0x0F) : sn76489_noise_vol_latch(0x0F));
        }
    } else {
        uint8_t vol = (chip == 0) ? s_vol[ch] : s2_vol[ch];
        if (chip == 0 && s_connected) {
            sn76489_write(ch < 3 ? sn76489_vol_latch(ch, vol) : sn76489_noise_vol_latch(vol));
        }
        if (chip == 1 && s_connected2) {
            sn76489_write2(ch < 3 ? sn76489_vol_latch(ch, vol) : sn76489_noise_vol_latch(vol));
        }
    }
    safe_flush();
}

static void RenderLevelMeters(void) {
    ImGui::BeginChild("SN_LevelMeters", ImVec2(0, 0), true);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    int numCh = 8; // 4 per chip
    float groupW = availW / (float)numCh;
    float meterW = groupW - 8.0f;
    if (meterW > 28.0f) meterW = 28.0f;
    float labelH = 20.0f;
    float volTextH = 18.0f;
    float meterH = availH - labelH - volTextH - 4.0f;
    if (meterH < 10.0f) meterH = 10.0f;

    auto levelToDB = [](float level) -> float {
        if (level <= 0.0f) return 0.0f;
        float db = 20.0f * log10f(level);
        if (db < -24.0f) db = -24.0f;
        return (db + 24.0f) / 24.0f;
    };

    auto getLevelColor = [](float level) -> ImU32 {
        if (level <= 0.0f) return IM_COL32(40, 40, 40, 255);
        if (level < 0.33f) {
            float t = level / 0.33f;
            return IM_COL32(0, (int)(100 + 155 * t), (int)(255 - 155 * t), 255);
        } else if (level < 0.66f) {
            float t = (level - 0.33f) / 0.33f;
            return IM_COL32((int)(255 * t), 255, (int)(100 - 100 * t), 255);
        } else {
            float t = (level - 0.66f) / 0.34f;
            return IM_COL32(255, (int)(255 - 155 * t), 0, 255);
        }
    };

    static const char* kChLabels[8] = {"T0", "T1", "T2", "N", "2T0", "2T1", "2T2", "2N"};

    // 滚轮反转所有通道
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
        for (int j = 0; j < numCh; j++) s_chMuted[j] = !s_chMuted[j];
        s_soloCh = -1;
        for (int j = 0; j < numCh; j++) ApplyChannelMute(j);
    }

    for (int i = 0; i < numCh; i++) {
        int chip = i / 4;  // 0 or 1
        int ch = i % 4;

        float centerX = p.x + i * groupW + groupW * 0.5f;
        float mY = p.y + labelH;
        float meterLeft = centerX - meterW * 0.5f;
        float meterRight = centerX + meterW * 0.5f;
        float meterBottom = mY + meterH;

        // Channel label as ImGui button
        int noiseType = (ch == 3) ? ((chip == 0) ? s_noiseType : s2_noiseType) : -1;
        ImU32 labelCol = getChColor(chip * 4 + ch, noiseType);
        bool isMuted = s_chMuted[i];
        bool isSolo = (s_soloCh == i);

        ImVec2 textSize = ImGui::CalcTextSize(kChLabels[i]);
        float btnW = textSize.x + 6.0f;
        float btnH = textSize.y + 4.0f;
        ImGui::SetCursorScreenPos(ImVec2(centerX - btnW * 0.5f, p.y + 1));

        // Style button per state
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        if (isMuted) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 50, 50, 200));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 70, 70, 220));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 90, 90, 240));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 200, 255));
        } else if (isSolo) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 160, 30, 200));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 200, 50, 220));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 240, 70, 240));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 200, 255));
        } else {
            ImColor col(labelCol);
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(col.Value.x * 0.4f * 255, col.Value.y * 0.4f * 255, col.Value.z * 0.4f * 255, 180));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(col.Value.x * 0.6f * 255, col.Value.y * 0.6f * 255, col.Value.z * 0.6f * 255, 220));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(col.Value.x * 0.8f * 255, col.Value.y * 0.8f * 255, col.Value.z * 0.8f * 255, 240));
            ImGui::PushStyleColor(ImGuiCol_Text, labelCol);
        }

        char btnId[16];
        snprintf(btnId, sizeof(btnId), "##ch%d", i);
        if (ImGui::Button(btnId, ImVec2(btnW, btnH))) {
            // Left-click: toggle mute
            if (s_soloCh >= 0) {
                s_soloCh = -1;
                s_chMuted[i] = !s_chMuted[i];
            } else {
                s_chMuted[i] = !s_chMuted[i];
            }
            ApplyChannelMute(i);
        }
        if (ImGui::IsItemClicked(1)) {
            // Right-click: toggle solo
            if (s_soloCh == i) {
                s_soloCh = -1;
                for (int j = 0; j < numCh; j++) { s_chMuted[j] = false; ApplyChannelMute(j); }
            } else {
                s_soloCh = i;
                for (int j = 0; j < numCh; j++) {
                    s_chMuted[j] = (j != i);
                    ApplyChannelMute(j);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Left-click: Mute/Unmute\nRight-click: Solo\nScroll: Invert all");
            ImGui::EndTooltip();
        }

        // Draw label text on top of button (button itself is small, no visible text)
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        // Overlay label text on the button area
        dl->AddText(ImVec2(centerX - textSize.x * 0.5f, p.y + 3),
                     isMuted ? IM_COL32(255, 200, 200, 255) : labelCol, kChLabels[i]);

        // Background
        dl->AddRectFilled(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(30, 30, 30, 255));
        dl->AddRect(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(100, 100, 100, 255));

        float level = (chip == 0) ? s_channelLevel[ch] : s2_channelLevel[ch];
        float displayLevel = levelToDB(level);
        if (displayLevel > 0.01f) {
            float barH = meterH * displayLevel;
            float barY = mY + meterH - barH;
            int segs = 20;
            for (int s = 0; s < segs; s++) {
                float segH = barH / segs;
                float segY = barY + s * segH;
                float segLvl = (float)(segs - s) / segs * displayLevel;
                dl->AddRectFilled(ImVec2(meterLeft + 1, segY), ImVec2(meterRight - 1, segY + segH), getLevelColor(segLvl));
            }
        }

        // 屏蔽覆盖层 + X 标记
        if (s_chMuted[i]) {
            dl->AddRectFilled(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(0, 0, 0, 120));
            dl->AddLine(ImVec2(meterLeft + 2, mY + 2), ImVec2(meterRight - 2, meterBottom - 2), IM_COL32(255, 80, 80, 200), 2);
            dl->AddLine(ImVec2(meterRight - 2, mY + 2), ImVec2(meterLeft + 2, meterBottom - 2), IM_COL32(255, 80, 80, 200), 2);
        }
        // 独奏高亮
        if (s_soloCh == i) {
            dl->AddRect(ImVec2(meterLeft - 1, mY - 1), ImVec2(meterRight + 1, meterBottom + 1), IM_COL32(255, 220, 50, 200), 0, 0, 2);
        }

        // Volume value
        uint8_t vol = (chip == 0) ? s_vol[ch] : s2_vol[ch];
        char volStr[8];
        snprintf(volStr, sizeof(volStr), "%d", vol);
        ImVec2 volSize = ImGui::CalcTextSize(volStr);
        ImU32 volCol = s_chMuted[i] ? IM_COL32(180, 60, 60, 255) : IM_COL32(180, 180, 180, 255);
        dl->AddText(ImVec2(centerX - volSize.x * 0.5f, mY + meterH + 2), volCol, volStr);
    }

    ImGui::EndChild();
}

// ============ Scope (placeholder) ============
static void RenderScopeArea(void) {
    if (!s_showScope) return;

    if (s_scopeHeight < 10.0f) s_scopeHeight = 10.0f;
    ImGui::BeginChild("SN_Scope", ImVec2(0, s_scopeHeight), true);

    bool hasScope = (s_voiceCh[0] >= 0);

    if (hasScope) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;
        float chW = availW / 4.0f - 4.0f;

        for (int i = 0; i < 4; i++) {
            if (s_voiceCh[i] < 0) continue;
            float x = p.x + i * (chW + 4.0f) + 2.0f;
            bool keyon = (s_vol[i] < 15);
            float level = (15.0f - (float)s_vol[i]) / 15.0f;

            s_scope.DrawChannel(s_voiceCh[i], dl, x, p.y + 16, chW, availH - 16,
                s_scopeAmplitude, getChColor(i, (i == 3) ? s_noiseType : -1), keyon, level,
                s_scopeSamples, 0, 735, true, true, 1, false);
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Scope requires libvgm SN76489 core integration)");
    }

    ImGui::EndChild();
}

// ============ Test & Channel Controls Window ============
static void RenderTestPopup(void) {
    if (!s_showTestPopup) return;
    ImGui::SetNextWindowSize(ImVec2(360, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SN76489 Debug##sntestwin", &s_showTestPopup)) { ImGui::End(); return; }

    // Hardware Tests
    if (ImGui::CollapsingHeader("Hardware Tests##sntest", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Scale##sntest", ImVec2(-1, 0))) StartTest(0);
        if (ImGui::Button("Arpeggio##sntest", ImVec2(-1, 0))) StartTest(1);
        if (ImGui::Button("Chord##sntest", ImVec2(-1, 0))) StartTest(2);
        if (ImGui::Button("Vol Sweep##sntest", ImVec2(-1, 0))) StartTest(3);
        if (ImGui::Button("Noise##sntest", ImVec2(-1, 0))) StartTest(4);
        if (s_testRunning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Stop Test##sntest", ImVec2(-1, 0))) StopTest();
            ImGui::PopStyleColor();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running...");
        }
    }

    // Channel Controls
    if (ImGui::CollapsingHeader("Channel Controls##snch", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        auto colorToVec4 = [](ImU32 c) -> ImVec4 {
            return ImVec4(((c >> 0) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, 1.0f);
        };
        for (int ch = 0; ch < 3; ch++) {
            ImGui::PushID(ch);
            ImGui::TextColored(colorToVec4(getChColor(ch)), "%s", kChNames[ch]);
            ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f);
            int volVal = s_vol[ch];
            if (ImGui::SliderInt("##vol", &volVal, 0, 15)) {
                s_vol[ch] = (uint8_t)volVal;
                if (s_connected) { sn76489_set_vol((uint8_t)ch, s_vol[ch]); safe_flush(); }
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(120.0f);
            int periodVal = (int)s_fullPeriod[ch];
            if (ImGui::SliderInt("##period", &periodVal, 0, 1023)) {
                s_fullPeriod[ch] = (uint16_t)periodVal;
                if (s_connected) { sn76489_set_tone((uint8_t)ch, s_fullPeriod[ch]); safe_flush(); }
            }
            ImGui::PopID();
        }

        // Noise
        ImGui::PushID(100);
        ImGui::TextColored(colorToVec4(getChColor(3, s_noiseType)), "Noise");
        ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f);
        int nVolVal = s_vol[3];
        if (ImGui::SliderInt("##nvol", &nVolVal, 0, 15)) {
            s_vol[3] = (uint8_t)nVolVal;
            if (s_connected) { sn76489_set_vol(3, s_vol[3]); safe_flush(); }
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Periodic##snp", s_noiseType == 0)) { s_noiseType = 0; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); safe_flush(); } }
        ImGui::SameLine();
        if (ImGui::RadioButton("White##snw", s_noiseType == 1)) { s_noiseType = 1; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); safe_flush(); } }
        ImGui::SameLine();
        if (ImGui::RadioButton("Ch2##snfch2", s_noiseUseCh2)) { s_noiseUseCh2 = true; if (s_connected) { sn76489_set_noise(s_noiseType, 3); safe_flush(); } }
        ImGui::SameLine();
        if (ImGui::RadioButton("Shift##snfsh", !s_noiseUseCh2)) { s_noiseUseCh2 = false; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); safe_flush(); } }
        if (!s_noiseUseCh2) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(60.0f);
            int nFreqVal = s_noiseFreq;
            if (ImGui::SliderInt("##nfreq", &nFreqVal, 0, 3)) {
                s_noiseFreq = (uint8_t)nFreqVal;
                if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); safe_flush(); }
            }
        }
        ImGui::PopID();
    }

    ImGui::End();
}

// ============ UI Rendering ============
static void RenderSidebar(void) {
    // SN76489 Hardware
    if (!ImGui::CollapsingHeader("SN76489 Hardware##sn", nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (s_connected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Disconnect##sn", ImVec2(-1, 0))) DisconnectHardware();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        if (ImGui::Button("Connect##sn", ImVec2(-1, 0))) { s_manualDisconnect = false; ConnectHardware(); }
        ImGui::PopStyleColor(2);
    }
    if (s_connected) ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected");

    ImGui::Spacing(); ImGui::Separator();

    // Periodic noise Ch2 frequency fix (MegaGRRL method)
    bool fixEnabled = s_dcsgLfsrWidth != 15;
    if (ImGui::Checkbox("Periodic Noise Fix##pnfix", &fixEnabled)) {
        s_dcsgLfsrWidth = fixEnabled ? 16 : 15;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale Tone2 freq for LFSR width difference\nwhen Periodic+Ch2 noise is active");
    if (fixEnabled) {
        ImGui::Indent();
        int lw = s_dcsgLfsrWidth;
        if (ImGui::RadioButton("15-bit TI (no fix)##lfsr15", lw == 15)) { s_dcsgLfsrWidth = 15; }
        if (ImGui::RadioButton("16-bit Sega (x1.0625)##lfsr16", lw == 16)) { s_dcsgLfsrWidth = 16; }
        if (ImGui::RadioButton("17-bit (x1.1333)##lfsr17", lw == 17)) { s_dcsgLfsrWidth = 17; }
        ImGui::Unindent();
    }

    ImGui::Spacing(); ImGui::Separator();

    // 2nd SN76489 (slot 1)
    if (ImGui::Checkbox("2nd SN76489 (slot 1)##sn2nd", &s_connected2)) {
        if (s_connected && s_connected2) {
            sn76489_mute_all2();
            safe_flush();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable 2nd SN76489 chip on slot 1\nfor T6W28 Dual Chip mode");

    ImGui::Spacing(); ImGui::Separator();

    // T6W28 mode
    if (ImGui::RadioButton("T6W28 Passthrough##t6w28pt", s_t6w28Mode == 0)) s_t6w28Mode = 0;
    if (ImGui::RadioButton("T6W28 Force SF2##t6w28sf2", s_t6w28Mode == 1)) s_t6w28Mode = 1;
    if (ImGui::RadioButton("T6W28 Dual Chip##t6w28dc", s_t6w28Mode == 2)) s_t6w28Mode = 2;
    if (s_t6w28Mode == 2 && !s_connected2) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Enable 2nd SN76489 above!");
    }

    ImGui::Spacing(); ImGui::Separator();

    // Shift noise note mapping
    if (ImGui::CollapsingHeader("Noise Shift Note Map##snshiftmap")) {
        static const char* kSfLabels[3] = {"SF0 (16x)", "SF1 (32x)", "SF2 (64x)"};
        for (int i = 0; i < 3; i++) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(80);
            if (ImGui::DragInt(kSfLabels[i], &s_shiftNoteMap[i], 1.0f, 0, 127)) {
                if (s_shiftNoteMap[i] < 0) s_shiftNoteMap[i] = 0;
                if (s_shiftNoteMap[i] > 127) s_shiftNoteMap[i] = 127;
            }
            ImGui::SameLine();
            static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            int n = s_shiftNoteMap[i];
            ImGui::Text("%s%d", kNoteNames[n % 12], n / 12 - 1);
            ImGui::PopID();
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // Channel Colors
    if (ImGui::CollapsingHeader("Channel Colors##snchcolors")) {
        static const char* kChLabels[10] = {"Tone0", "Tone1", "Tone2", "N-Periodic", "N-White", "2-Tone0", "2-Tone1", "2-Tone2", "2N-Periodic", "2N-White"};
        for (int i = 0; i < 10; i++) {
            ImGui::PushID(i);
            float colF[4];
            ImU32 curCol = kChColorsCustom[i];
            ImU32 defCol = (i < 5) ? kChColors[i] : kCh2Colors[i - 5];
            if (curCol != 0) {
                colF[0] = ((curCol >> 0) & 0xFF) / 255.0f;
                colF[1] = ((curCol >> 8) & 0xFF) / 255.0f;
                colF[2] = ((curCol >> 16) & 0xFF) / 255.0f;
                colF[3] = ((curCol >> 24) & 0xFF) / 255.0f;
            } else {
                colF[0] = ((defCol >> 0) & 0xFF) / 255.0f;
                colF[1] = ((defCol >> 8) & 0xFF) / 255.0f;
                colF[2] = ((defCol >> 16) & 0xFF) / 255.0f;
                colF[3] = 1.0f;
            }
            if (ImGui::ColorEdit4(("##snclredit" + std::to_string(i)).c_str(), colF, ImGuiColorEditFlags_NoInputs)) {
                kChColorsCustom[i] = IM_COL32(
                    (int)(colF[0] * 255 + 0.5f), (int)(colF[1] * 255 + 0.5f),
                    (int)(colF[2] * 255 + 0.5f), (int)(colF[3] * 255 + 0.5f));
                SaveConfig();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(kChLabels[i]);
            if (kChColorsCustom[i] == 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(auto)");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(("Reset##rclr" + std::to_string(i)).c_str())) {
                kChColorsCustom[i] = 0;
                SaveConfig();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Reset All Colors##snrclrall")) {
            memset(kChColorsCustom, 0, sizeof(kChColorsCustom)); // 10 entries
            SaveConfig();
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // Test popup button
    if (s_testRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Test Running...##sntestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Debug Test##sntestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Loop settings
    ImGui::TextDisabled("VGM Loop Count");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max internal loop repetitions\n0 = infinite loop");
    ImGui::SameLine();
    int maxL = s_vgmMaxLoops;
    if (ImGui::InputInt("##maxloops", &maxL, 1, 5)) {
        if (maxL < 0) maxL = 0;
        s_vgmMaxLoops = maxL;
    }

    // Seek mode
    ImGui::TextDisabled("Seek Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fast-forward: replay commands from start\nDirect: skip without HW, reset chip on target");
    ImGui::RadioButton("Fast-forward##snseek", &s_seekMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Direct##snseek", &s_seekMode, 1);

    // Fadeout
    ImGui::TextDisabled("Loop Fadeout");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fade out volume before final loop end\n0 = disabled");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragFloat("##snfadeout", &s_fadeoutDuration, 0.1f, 0.0f, 30.0f, "%.1f sec")) {
        if (s_fadeoutDuration < 0.0f) s_fadeoutDuration = 0.0f;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Scope settings
    if (ImGui::CollapsingHeader("Scope##snscope", nullptr, 0)) {
        ImGui::Checkbox("Show Scope##sn", &s_showScope);
        if (s_showScope) {
            ImGui::SliderFloat("Height##sn", &s_scopeHeight, 20, 300);
            ImGui::SliderFloat("Amplitude##sn", &s_scopeAmplitude, 0.5f, 10.0f);
            ImGui::SliderInt("Samples##sn", &s_scopeSamples, 100, 1000);
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // VGM File Info
    if (ImGui::CollapsingHeader("VGM File Info##sninfo", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (s_vgmLoaded) {
            if (s_vgmVersion) ImGui::TextDisabled("VGM v%X.%02X", (s_vgmVersion >> 8) & 0xFF, s_vgmVersion & 0xFF);
            double durSec = (double)s_vgmTotalSamples / 44100.0;
            int durMin = (int)durSec / 60; int durSecI = (int)durSec % 60;
            ImGui::TextDisabled("Duration:"); ImGui::SameLine();
            ImGui::Text("%02d:%02d", durMin, durSecI);

            if (s_vgmLoopSamples > 0) {
                ImGui::TextDisabled("Loop: Yes");
                ImGui::SameLine();
                ImGui::TextDisabled("(%d / %s)", s_vgmLoopCount, s_vgmMaxLoops == 0 ? "inf" : std::to_string(s_vgmMaxLoops).c_str());
            } else {
                ImGui::TextDisabled("Loop: No");
            }

            ImGui::Spacing();
            if (!s_trackName.empty()) {
                ImGui::TextDisabled("Track:");
                ImGui::Indent();
                DrawScrollingText("##sntrack", s_trackName.c_str(), IM_COL32(255, 255, 102, 255));
                ImGui::Unindent();
            }
            if (!s_gameName.empty()) {
                ImGui::TextDisabled("Game:");
                ImGui::SameLine();
                DrawScrollingText("##sngame", s_gameName.c_str(), IM_COL32(200, 200, 200, 255));
            }
            if (!s_systemName.empty()) {
                ImGui::TextDisabled("System:");
                ImGui::SameLine();
                ImGui::Text("%s", s_systemName.c_str());
            }
            if (!s_artistName.empty()) {
                ImGui::TextDisabled("Artist:");
                ImGui::SameLine();
                DrawScrollingText("##snartist", s_artistName.c_str(), IM_COL32(200, 200, 200, 255));
            }
        } else {
            ImGui::TextDisabled("No VGM file loaded");
        }
    }
}

static void RenderPlayerBar(void) {
    bool hasFile = s_vgmLoaded;
    bool isPlaying = hasFile && s_vgmPlaying && !s_vgmPaused;
    bool isPaused = hasFile && s_vgmPlaying && s_vgmPaused;

    ImGui::SetNextItemAllowOverlap();
    bool playerOpen = ImGui::TreeNodeEx("SN VGM Player##snvgmplayer",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_snPlayerCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));

    if (playerOpen) s_snPlayerCollapsed = false;
    else s_snPlayerCollapsed = true;

    // Semi-transparent progress bar background when collapsed
    if (!playerOpen && hasFile && s_vgmTotalSamples > 0) {
        double posSec = (double)s_vgmCurrentSamples / 44100.0;
        double durSec;
        if (s_vgmLoopSamples > 0 && s_vgmMaxLoops > 0) {
            durSec = (double)(s_vgmTotalSamples - s_vgmLoopSamples) / 44100.0 + (double)s_vgmLoopSamples / 44100.0 * s_vgmMaxLoops;
        } else {
            durSec = (double)s_vgmTotalSamples / 44100.0;
        }
        float progress = (durSec > 0.0) ? (float)(posSec / durSec) : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        ImVec2 rectMin = ImGui::GetItemRectMin();
        ImVec2 rectMax = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(rectMin, rectMax, true);
        float fillW = (rectMax.x - rectMin.x) * progress;
        dl->AddRectFilled(rectMin, ImVec2(rectMin.x + fillW, rectMax.y), IM_COL32(100, 180, 255, 50));
        dl->PopClipRect();
    }

    // Title bar mini controls (always visible)
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("<<##snminiprev")) {
        if (!s_playlist.empty()) PlayPlaylistPrev();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous");
    ImGui::SameLine();
    if (isPlaying) {
        if (ImGui::SmallButton("||##snminipause")) PauseVGMPlayback();
    } else if (isPaused) {
        if (ImGui::SmallButton(">##snminipause")) PauseVGMPlayback();
    } else if (hasFile) {
        if (ImGui::SmallButton(">##snminipause")) StartVGMPlayback();
    } else {
        ImGui::SmallButton(">##snminipause");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play / Pause");
    ImGui::SameLine();
    if (ImGui::SmallButton(">>##snmininext")) {
        if (!s_playlist.empty()) PlayPlaylistNext();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next");
    ImGui::SameLine();

    // Scrolling filename
    if (hasFile) {
        const char* fname = s_vgmPath;
        const char* slash = strrchr(fname, '\\');
        if (!slash) slash = strrchr(fname, '/');
        const char* displayName = slash ? slash + 1 : fname;

        ImU32 nameCol;
        if (isPlaying) nameCol = IM_COL32(100, 255, 100, 255);
        else if (isPaused) nameCol = IM_COL32(255, 255, 100, 255);
        else nameCol = IM_COL32(220, 220, 220, 255);

        float maxNameW = ImGui::CalcTextSize("ABCDEFGHIJKLMNOPQRSTUVWXABCDEFGHIJKLMNOP").x;
        DrawScrollingText("##snminifilename", displayName, nameCol, maxNameW);
    } else {
        ImGui::TextDisabled("(no file)");
    }

    // Time display when collapsed
    if (!playerOpen && hasFile && s_vgmTotalSamples > 0) {
        double posSec = (double)s_vgmCurrentSamples / 44100.0;
        double durSec;
        if (s_vgmLoopSamples > 0 && s_vgmMaxLoops > 0) {
            durSec = (double)(s_vgmTotalSamples - s_vgmLoopSamples) / 44100.0 + (double)s_vgmLoopSamples / 44100.0 * s_vgmMaxLoops;
        } else {
            durSec = (double)s_vgmTotalSamples / 44100.0;
        }
        int curMin = (int)posSec / 60; int curSecI = (int)posSec % 60;
        int totMin = (int)durSec / 60; int totSecI = (int)durSec % 60;
        char timeStr[64];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d", curMin, curSecI, totMin, totSecI);
        float timeW = ImGui::CalcTextSize(timeStr).x;
        float contentRight = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(contentRight - timeW);
        ImGui::Text("%s", timeStr);
    }

    if (playerOpen) {
        // Expanded controls
        float buttonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
        if (ImGui::Button("Play##snplay", ImVec2(buttonWidth, 30))) {
            if (!hasFile) { /* nothing */ }
            else if (isPaused) PauseVGMPlayback();
            else { SeekVGMToStart(); StartVGMPlayback(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause##snpause", ImVec2(buttonWidth, 30))) {
            if (isPlaying) PauseVGMPlayback();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop##snstop", ImVec2(buttonWidth, 30))) {
            s_vgmTrackEnded = false;
            StopVGMPlayback();
        }

        float navWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
        if (ImGui::Button("<< Prev##snprev", ImVec2(navWidth, 25))) {
            if (!s_playlist.empty()) PlayPlaylistPrev();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>##snnext", ImVec2(navWidth, 25))) {
            if (!s_playlist.empty()) PlayPlaylistNext();
        }

        ImGui::Checkbox("Auto-play##sn", &s_autoPlayNext);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-play next track when current finishes");
        ImGui::SameLine();
        const char* modeText = s_isSequentialPlayback ? "Seq" : "Rnd";
        if (ImGui::Button(modeText, ImVec2(35, 0))) s_isSequentialPlayback = !s_isSequentialPlayback;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sequential / Random");
        ImGui::SameLine();
        if (ImGui::Button("Open##snopen")) OpenVGMFileDialog();

        // Progress bar with seek (VGM-style)
        if (hasFile && s_vgmTotalSamples > 0) {
            // 真实播放时间（含循环）
            double posSec = (double)s_vgmCurrentSamples / 44100.0;
            // 有循环时，总时长 = intro + loop × maxLoops
            double durSec;
            if (s_vgmLoopSamples > 0 && s_vgmMaxLoops > 0) {
                double introSec = (double)(s_vgmTotalSamples - s_vgmLoopSamples) / 44100.0;
                double loopSec = (double)s_vgmLoopSamples / 44100.0;
                durSec = introSec + loopSec * s_vgmMaxLoops;
            } else {
                durSec = (double)s_vgmTotalSamples / 44100.0;
            }
            float progress = (float)(posSec / durSec);
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            int curMin = (int)posSec / 60; int curSecI = (int)posSec % 60;
            int totMin = (int)durSec / 60; int totSecI = (int)durSec % 60;
            char posStr[32], durStr[32];
            snprintf(posStr, sizeof(posStr), "%02d:%02d", curMin, curSecI);
            snprintf(durStr, sizeof(durStr), "%02d:%02d", totMin, totSecI);
            ImGui::Text("%s / %s", posStr, durStr);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPos().x - 70);
            if (s_vgmLoopCount > 0)
                ImGui::TextDisabled("Loop #%d", s_vgmLoopCount);
            else
                ImGui::TextDisabled("       ");  // 占位

            // Slider for seeking — only seek on mouse release
            static float seek_progress = 0.0f;
            ImGui::SliderFloat("##snseek", &seek_progress, 0.0f, 1.0f, "");
            if (ImGui::IsItemActive()) {
                // Dragging: keep slider value, don't seek yet
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
                // targetSample 按含循环的时间轴计算
                double totalDurSec;
                if (s_vgmLoopSamples > 0 && s_vgmMaxLoops > 0) {
                    totalDurSec = (double)(s_vgmTotalSamples - s_vgmLoopSamples) / 44100.0
                                + (double)s_vgmLoopSamples / 44100.0 * s_vgmMaxLoops;
                } else {
                    totalDurSec = (double)s_vgmTotalSamples / 44100.0;
                }
                UINT32 targetSample = (UINT32)(seek_progress * totalDurSec * 44100.0);
                // Stop thread temporarily for seek
                s_vgmThreadRunning = false; s_vgmPlaying = false;
                if (s_vgmThread) { WaitForSingleObject(s_vgmThread, 2000); CloseHandle(s_vgmThread); s_vgmThread = nullptr; }
                if (s_vgmFile) { fclose(s_vgmFile); s_vgmFile = sn_fopen(s_vgmPath, "rb"); }
                if (s_vgmFile) {
                    if (s_seekMode == 0) {
                        // 快进模式：先静音，从头解析到目标位置，恢复后 VGM 命令自动设音量
                        if (s_connected) sn76489_mute_all();
                        if (s_connected2) sn76489_mute_all2();
                        safe_flush();
                        fseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
                        UINT32 skipSamples = 0;
                        while (skipSamples < targetSample) {
                            int cmdSamples = VGMProcessCommand();
                            if (cmdSamples < 0) break;
                            if (cmdSamples > 0) {
                                skipSamples += cmdSamples;
                                if (skipSamples > targetSample) skipSamples = targetSample;
                            }
                        }
                        s_vgmCurrentSamples = targetSample;
                        if (s_connected) safe_flush();
                    } else {
                        // 直接跳转模式：快进不发送硬件，然后复位芯片
                        fseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
                        bool wasConn = s_connected, wasConn2 = s_connected2;
                        s_connected = false; s_connected2 = false;
                        UINT32 skipSamples = 0;
                        while (skipSamples < targetSample) {
                            int cmdSamples = VGMProcessCommand();
                            if (cmdSamples < 0) break;
                            if (cmdSamples > 0) {
                                skipSamples += cmdSamples;
                                if (skipSamples > targetSample) skipSamples = targetSample;
                            }
                        }
                        s_connected = wasConn; s_connected2 = wasConn2;
                        s_vgmCurrentSamples = targetSample;
                        // 复位硬件（学习断开时的复位序列）
                        if (s_connected) {
                            sn76489_mute_all();
                            if (s_connected2) sn76489_mute_all2();
                            safe_flush();
                            Sleep(50);
                            InitHardware();
                            safe_flush();
                        }
                    }
                    // Restart playback
                    s_vgmPlaying = true;
                    s_vgmPaused = false;
                    s_vgmThreadRunning = true;
                    s_vgmThread = CreateThread(NULL, 0, VGMPlaybackThread, NULL, 0, NULL);
                }
            } else {
                // Idle: sync slider to current playback position
                seek_progress = progress;
            }
        } else {
            ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
        }

        // Status text
        if (hasFile) {
            const char* fname = s_vgmPath;
            const char* slash = strrchr(fname, '\\');
            if (!slash) slash = strrchr(fname, '/');
            if (isPlaying) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Playing:");
                ImGui::SameLine();
                ImGui::Text("%s", slash ? slash + 1 : fname);
            } else if (isPaused) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused:");
                ImGui::SameLine();
                ImGui::Text("%s", slash ? slash + 1 : fname);
            } else {
                ImGui::TextDisabled("Ready:");
                ImGui::SameLine();
                ImGui::Text("%s", slash ? slash + 1 : fname);
            }
        }
    }
}

static void RenderRegisterTable(void) {
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "SN76489 Registers");
    ImGui::Separator();

    // Tone channels: one row per channel
    if (ImGui::BeginTable("##sntoneregs", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 3; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Tone%d", ch);
            ImGui::TableSetColumnIndex(1);
            if (s_fullPeriod[ch] > 0) {
                int note = period_to_midi_note(s_fullPeriod[ch]);
                static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                if (note >= 0 && note < 128) ImGui::Text("%s%d", kNoteNames[note % 12], note / 12 - 1);
                else ImGui::Text("-");
            } else {
                ImGui::Text("-");
            }
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", s_fullPeriod[ch]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u%s", s_vol[ch], s_vol[ch] == 15 ? " [MUTE]" : "");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    // Noise channel: separate detailed table
    if (ImGui::BeginTable("##snnoiseregs", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Type");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", s_noiseType == 0 ? "Periodic" : "White");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Source");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", s_noiseUseCh2 ? "Tone2 frequency" : "Shift register");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Shift Freq");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", s_noiseFreq);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Volume");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%u%s", s_vol[3], s_vol[3] == 15 ? " [MUTE]" : "");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Register");
        ImGui::TableSetColumnIndex(1); ImGui::Text("0x%02X", sn76489_noise_latch(s_noiseType, s_noiseUseCh2 ? 3 : s_noiseFreq));
        ImGui::EndTable();
    }
}

static void RenderRegisterTable2(void) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "2nd SN76489 Registers (slot 1)");
    ImGui::Separator();

    if (ImGui::BeginTable("##sntoneregs2", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 3; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Tone%d", ch);
            ImGui::TableSetColumnIndex(1);
            if (s2_fullPeriod[ch] > 0) {
                int note = period_to_midi_note(s2_fullPeriod[ch]);
                static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                if (note >= 0 && note < 128) ImGui::Text("%s%d", kNoteNames[note % 12], note / 12 - 1);
                else ImGui::Text("-");
            } else {
                ImGui::Text("-");
            }
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", s2_fullPeriod[ch]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u%s", s2_vol[ch], s2_vol[ch] == 15 ? " [MUTE]" : "");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    if (ImGui::BeginTable("##snnoiseregs2", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Type");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", s2_noiseType == 0 ? "Periodic" : "White");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Source");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", s2_noiseUseCh2 ? "Tone2 frequency" : "Shift register");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Shift Freq");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", s2_noiseFreq);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Volume");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%u%s", s2_vol[3], s2_vol[3] == 15 ? " [MUTE]" : "");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Register");
        ImGui::TableSetColumnIndex(1); ImGui::Text("0x%02X", sn76489_noise_latch(s2_noiseType, s2_noiseUseCh2 ? 3 : s2_noiseFreq));
        ImGui::EndTable();
    }
}

static void RenderFileBrowser(void) {
    // CollapsingHeader with filter on title bar (VGM-style)
    ImGui::SetNextItemAllowOverlap();
    bool browserOpen = ImGui::TreeNodeEx("SN File Browser##snfb",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_DefaultOpen);

    // Filter on title bar
    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##snfbfilter", "Filter...", s_fileBrowserFilter, sizeof(s_fileBrowserFilter));

    if (!browserOpen) return;

    // Navigation buttons
    if (ImGui::Button("<##snfbback", ImVec2(25, 0))) NavBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">##snfbfwd", ImVec2(25, 0))) NavForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^##snfbup", ImVec2(25, 0))) NavToParent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");
    ImGui::SameLine();

    // Breadcrumb path bar (VGM-style: only show InputText when in edit mode)
    if (!s_pathEditMode) {
        float availWidth = ImGui::GetContentRegionAvail().x;
    std::vector<std::string> segments = MidiPlayer::SplitPath(s_currentPath);
    std::vector<float> buttonWidths;
    std::vector<std::string> accumulatedPaths;
    std::string accumulatedPath;
    ImGuiStyle& style = ImGui::GetStyle();
    float framePaddingX = style.FramePadding.x;
    float itemSpacingX = style.ItemSpacing.x;
    float buttonBorderSize = style.FrameBorderSize;
    for (size_t i = 0; i < segments.size(); i++) {
        if (i == 0) accumulatedPath = segments[i];
        else { if (accumulatedPath.back() != '\\') accumulatedPath += "\\"; accumulatedPath += segments[i]; }
        accumulatedPaths.push_back(accumulatedPath);
        ImVec2 textSize = ImGui::CalcTextSize(segments[i].c_str());
        float bw = textSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
        buttonWidths.push_back(bw);
    }
    ImVec2 separatorTextSize = ImGui::CalcTextSize(">");
    float separatorWidth = separatorTextSize.x + itemSpacingX * 2.0f;
    ImVec2 ellipsisTextSize = ImGui::CalcTextSize("...");
    float ellipsisButtonWidth = ellipsisTextSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
    float ellipsisWidth = ellipsisButtonWidth + separatorWidth;
    float safeAvailWidth = availWidth - 10.0f;
    int firstVisibleSegment = (int)segments.size() - 1;
    float usedWidth = (segments.size() > 0) ? buttonWidths.back() : 0.0f;
    for (int i = (int)segments.size() - 2; i >= 0; i--) {
        float segWidth = buttonWidths[i] + separatorWidth;
        float neededEllipsis = (i > 0) ? ellipsisWidth : 0.0f;
        if (usedWidth + segWidth + neededEllipsis > safeAvailWidth) break;
        else { usedWidth += segWidth; firstVisibleSegment = i; }
    }

    ImVec2 barStartPos = ImGui::GetCursorScreenPos();
    float barHeight = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    if (firstVisibleSegment > 0) {
        if (ImGui::Button("...##snellipsis")) {
            s_pathEditMode = true; s_pathEditModeJustActivated = true;
            snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
        }
        ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine();
    }
    for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
        std::string btnId = segments[i] + "##snseg" + std::to_string(i);
        if (ImGui::Button(btnId.c_str())) NavigateTo(accumulatedPaths[i].c_str());
        if (i < (int)segments.size() - 1) { ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine(); }
    }
    ImGui::EndGroup();
    ImVec2 barEndPos = ImGui::GetItemRectMax();
    float emptySpaceWidth = barStartPos.x + availWidth - barEndPos.x;
    if (emptySpaceWidth > 0) {
        ImGui::SameLine();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(barEndPos.x, barStartPos.y), ImVec2(barEndPos.x + emptySpaceWidth, barStartPos.y + barHeight),
            ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::InvisibleButton("##snpathEmpty", ImVec2(emptySpaceWidth, barHeight));
        if (ImGui::IsItemClicked(0)) {
            s_pathEditMode = true; s_pathEditModeJustActivated = true;
            snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
        }
    }
    } // end !s_pathEditMode
    else {
        // Path edit mode
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##snPathInput", s_pathInput, MAX_PATH, ImGuiInputTextFlags_EnterReturnsTrue)) {
            NavigateTo(s_pathInput);
            s_pathEditMode = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_pathEditMode = false; s_pathEditModeJustActivated = false;
            snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
        } else if (!s_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            s_pathEditMode = false;
            snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
        }
        if (s_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            s_pathEditModeJustActivated = false;
        }
    }

    // File list
    float fileAreaHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("SnFileList##snfl", ImVec2(-1, fileAreaHeight), true);

    for (int i = 0; i < (int)s_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = s_fileList[i];
        ImGui::PushID(i);

        // Apply filter
        if (s_fileBrowserFilter[0] != '\0') {
            std::string lowerName = entry.name;
            std::string lowerFilter = s_fileBrowserFilter;
            for (auto& c : lowerName) c = tolower(c);
            for (auto& c : lowerFilter) c = tolower(c);
            if (lowerName.find(lowerFilter) == std::string::npos) {
                ImGui::PopID(); continue;
            }
        }

        char label[512];
        if (entry.isDirectory) {
            if (entry.name == "..") snprintf(label, sizeof(label), "[UP] %s", entry.name.c_str());
            else snprintf(label, sizeof(label), "[DIR] %s", entry.name.c_str());
        } else {
            snprintf(label, sizeof(label), "%s", entry.name.c_str());
        }

        bool isPlaying = (entry.fullPath == s_currentPlayingFilePath && s_vgmPlaying);
        if (isPlaying)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));

        bool selected = (i == s_selectedFileIndex);
        if (ImGui::Selectable(label, selected)) {
            s_selectedFileIndex = i;
            if (entry.isDirectory) {
                if (entry.name == "..") NavToParent();
                else NavigateTo(entry.fullPath.c_str());
            } else {
                s_currentPlayingFilePath = entry.fullPath;
                for (int pi = 0; pi < (int)s_playlist.size(); pi++) {
                    if (s_playlist[pi] == entry.fullPath) { s_playlistIndex = pi; break; }
                }
                if (LoadVGMFile(entry.fullPath.c_str())) StartVGMPlayback();
            }
        }

        if (isPlaying) ImGui::PopStyleColor();

        ImGui::PopID();
    }

    // Scroll to playing file
    if (!s_currentPlayingFilePath.empty()) {
        for (int i = 0; i < (int)s_fileList.size(); i++) {
            if (s_fileList[i].fullPath == s_currentPlayingFilePath) {
                // Auto-scroll only if not user-scrolling
                // Simple approach: scroll to center if visible
                break;
            }
        }
    }

    ImGui::EndChild();
}

static void RenderLogPanel(void) {
    // Debug Log
    if (ImGui::CollapsingHeader("SN Debug Log##snlog", nullptr, 0)) {
        ImGui::Checkbox("Auto-scroll##snlog", &s_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##snlog")) {
            s_log.clear(); s_logDisplay[0] = '\0'; s_logLastSize = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All##snlog")) {
            ImGui::SetClipboardText(s_log.c_str());
        }
        size_t copyLen = s_log.size() < sizeof(s_logDisplay) - 1 ? s_log.size() : sizeof(s_logDisplay) - 1;
        memcpy(s_logDisplay, s_log.c_str(), copyLen);
        s_logDisplay[copyLen] = '\0';
        bool changed = (s_log.size() != s_logLastSize);
        s_logLastSize = s_log.size();
        if (s_logAutoScroll && changed) s_logScrollToBottom = true;

        float logH = 150;
        ImGui::BeginChild("SnDebugLogRegion", ImVec2(0, logH), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 tsz = ImGui::CalcTextSize(s_logDisplay, NULL, false, -1.0f);
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float minH = ImGui::GetContentRegionAvail().y;
        float inH = (tsz.y > minH) ? tsz.y + lineH * 2 : minH;
        ImGui::InputTextMultiline("##SnLogText", s_logDisplay, sizeof(s_logDisplay),
            ImVec2(-1, inH), ImGuiInputTextFlags_ReadOnly);
        if (s_logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            s_logScrollToBottom = false;
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // Folder History
    ImGui::SetNextItemAllowOverlap();
    bool historyOpen = ImGui::TreeNodeEx("SN Folder History##snhist",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_snHistoryCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
    if (historyOpen) s_snHistoryCollapsed = false;
    else s_snHistoryCollapsed = true;

    // Title bar mini controls
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Clear##snHistClear")) {
        s_folderHistory.clear(); SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All");
    ImGui::SameLine();
    if (s_histSortMode == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Time##snhistSortTime")) s_histSortMode = 0;
    if (s_histSortMode == 0) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by time");
    ImGui::SameLine();
    if (s_histSortMode == 1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Freq##snhistSortFreq")) s_histSortMode = 1;
    if (s_histSortMode == 1) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##snHistFilter", "Filter...", s_histFilter, sizeof(s_histFilter));

    if (historyOpen) {
        struct HistEntry { std::string name; int idx; int fileCount; };
        std::vector<HistEntry> entries;
        std::set<std::string> seen;
        for (int i = 0; i < (int)s_folderHistory.size(); i++) {
            size_t lastSlash = s_folderHistory[i].find_last_of("\\/");
            std::string folderName = (lastSlash != std::string::npos) ? s_folderHistory[i].substr(lastSlash + 1) : s_folderHistory[i];
            if (s_histFilter[0] != '\0') {
                std::string lowerName = folderName;
                std::string lowerFilter = s_histFilter;
                for (auto& c : lowerName) c = tolower(c);
                for (auto& c : lowerFilter) c = tolower(c);
                if (lowerName.find(lowerFilter) == std::string::npos) continue;
            }
            if (seen.insert(folderName).second) {
                int fileCount = 0;
                const char* exts[] = { "\\*.vgm", "\\*.vgz", "\\*.VGM", "\\*.VGZ" };
                for (int e = 0; e < 4; e++) {
                    std::string searchPath = s_folderHistory[i] + exts[e];
                    std::wstring wSearchPath = MidiPlayer::UTF8ToWide(searchPath);
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
                    if (h != INVALID_HANDLE_VALUE) {
                        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) fileCount++; } while (FindNextFileW(h, &fd));
                        FindClose(h);
                    }
                }
                entries.push_back({folderName, i, fileCount});
            }
        }
        if (s_histSortMode == 1) {
            std::sort(entries.begin(), entries.end(), [](const HistEntry& a, const HistEntry& b) { return a.fileCount > b.fileCount; });
        }

        ImGui::Separator();
        float historyHeight = ImGui::GetContentRegionAvail().y - 5;
        ImGui::BeginChild("SnHistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

        if (entries.empty()) {
            ImGui::TextDisabled("No matching folders...");
        } else {
            for (const auto& e : entries) {
                const std::string& path = s_folderHistory[e.idx];
                ImGui::PushID(e.idx);
                char label[512];
                if (e.fileCount > 0)
                    snprintf(label, sizeof(label), "[%d] %s", e.fileCount, e.name.c_str());
                else
                    snprintf(label, sizeof(label), "[DIR] %s", e.name.c_str());
                if (ImGui::Selectable(label, false)) NavigateTo(path.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Remove from history")) {
                        s_folderHistory.erase(s_folderHistory.begin() + e.idx);
                        SaveConfig();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
}

static void RenderMain(void) {
    float pianoHeight      = 150;
    float levelMeterHeight = 200;
    float statusAreaWidth  = 460;
    float topSectionHeight = pianoHeight + levelMeterHeight;

    // Top section: Piano + LevelMeters (left) | Registers (right)
    ImGui::BeginGroup();
    ImGui::BeginChild("SN_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoKeyboard();
    ImGui::EndChild();
    ImGui::BeginChild("SN_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
    RenderLevelMeters();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginChild("SN_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderRegisterTable();
    RenderRegisterTable2();
    ImGui::EndChild();

    // Bottom section: Player + FileBrowser | Log+History (matching VGM layout)
    ImGui::BeginChild("SN_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderPlayerBar();
    RenderFileBrowser();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("SN_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();

    // Scope (optional, very bottom)
    RenderScopeArea();
}

void Render() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    bool visible = ImGui::Begin("SN76489(DCSG)");
    ImGui::BeginChild("SN_LeftPane", ImVec2(300, 0), true);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("SN_RightPane", ImVec2(0, 0), false);
    RenderMain();
    ImGui::EndChild();
    ImGui::End();

    // Popups
    RenderTestPopup();
}

bool WantsKeyboardCapture() { return false; }

} // namespace SN76489Window
