// ay8910_window.cpp - AY8910 (PSG) Hardware Control Window
// Controls real AY8910 chip via FTDI/SPFM Light interface
// VGM hardware playback + debug test functions + file browser

#include "ay8910_window.h"
#include "windows/sn76489/spfm.h"
#include "windows/ym2163/chip_control.h"
#include "windows/ym2163/chip_window_ym2163.h"
#include "windows/spfm/spfm_manager.h"
#include "midi/midi_player.h"
#include "core/vgm_sync.h"
#include "core/modizer_viz.h"
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
#include <zlib.h>

namespace AY8910Window {

// ============ Constants ============
static const int AY_SAMPLE_RATE = 44100;
static const double AY8910_CLOCK = 1789773.0;

// 3 tone + 1 noise = 4 channels
static const int AY_NUM_CHANNELS = 4;

// Channel names
static const char* kChNames[4] = { "ChA", "ChB", "ChC", "Noise" };

// Note names for display
static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// Envelope shape names
static const char* kEnvShapeNames[16] = {
    "\\\\_____", "/\\_\\_\\_\\_", "\\\\\\___", "\\/\\/\\\\/",
    "_______", "\\\\\\____", "\\/\\___", "Hold\\\\Alt",
    "Continu", "Attack2", "Alt\\\\Alt", "Hold\\\\Hld",
    "\\\\____/\\_", "/\\_\\\\___", "Hold\\\\Cont", "\\/\\\\___\\\\"
};

// 4 channel colors
static ImU32 kChColors[4] = {
    IM_COL32(160, 200, 160, 255), // ChA: green
    IM_COL32(160, 160, 220, 255), // ChB: blue
    IM_COL32(220, 160, 160, 255), // ChC: red
    IM_COL32(220, 220, 100, 255), // Noise: yellow
};

// Custom channel colors (0 = use default)
static ImU32 kChColorsCustom[4] = {};

// ============ Connection State ============
static bool s_connected = false;

static inline bool IsConnected() { return s_connected; }

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

// VGZ support: memory buffer for decompressed data
static std::vector<UINT8> s_memData;
static size_t s_memPos = 0;

static size_t vgmfread(void* buf, size_t sz, size_t cnt, FILE* f) {
    if (!s_memData.empty()) {
        size_t total = sz * cnt;
        if (s_memPos + total > s_memData.size()) {
            size_t avail = s_memData.size() - s_memPos;
            if (avail < sz) return 0;
            cnt = avail / sz;
            total = cnt * sz;
        }
        memcpy(buf, &s_memData[s_memPos], total);
        s_memPos += total;
        return cnt;
    }
    return fread(buf, sz, cnt, f);
}

static int vgmfseek(FILE* f, long off, int whence) {
    if (!s_memData.empty()) {
        switch (whence) {
            case SEEK_SET: s_memPos = (size_t)off; break;
            case SEEK_CUR: s_memPos += off; break;
            case SEEK_END: s_memPos = s_memData.size() + off; break;
        }
        if (s_memPos > s_memData.size()) s_memPos = s_memData.size();
        return 0;
    }
    return fseek(f, off, whence);
}

static std::vector<UINT8> InflateGzip(const UINT8* data, size_t size) {
    std::vector<UINT8> out;
    z_stream strm = {};
    strm.avail_in = (uInt)size;
    strm.next_in = (Bytef*)data;
    if (inflateInit2(&strm, 15 + 32) != Z_OK) return out;
    UINT8 chunk[16384];
    int ret;
    do {
        strm.avail_out = sizeof(chunk);
        strm.next_out = chunk;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&strm); return std::vector<UINT8>(); }
        out.insert(out.end(), chunk, chunk + sizeof(chunk) - strm.avail_out);
    } while (strm.avail_out == 0);
    inflateEnd(&strm);
    return out;
}
static int s_vgmLoopCount = 0;
static int s_vgmMaxLoops = 2;

// Flush Mode & Timer Mode
static int s_flushMode = 2;   // 1=Register-Level, 2=Command-Level (default)
static int s_timerMode = 0;   // 0=H-Prec, 1=Hybrid, 2=MM-Timer, 3=VGMPlay, 7=OptVGMPlay

// Seek & Fadeout
static int s_seekMode = 0;
static float s_fadeoutDuration = 3.0f;
static bool s_fadeoutActive = false;
static float s_fadeoutLevel = 1.0f;
static UINT32 s_fadeoutStartSample = 0;
static UINT32 s_fadeoutEndSample = 0;

// Channel mute/solo
static bool s_chMuted[AY_NUM_CHANNELS] = {};
static int s_soloCh = -1;

// GD3 tags
static std::string s_trackName, s_gameName, s_systemName, s_artistName;
static UINT32 s_vgmVersion = 0;

// ============ AY8910 Register Shadow ============
static uint8_t s_regShadow[0x10] = {};

// Individual channel state for UI display (extracted from shadow)
static uint8_t s_toneFine[3] = {};     // Reg 0x00, 0x02, 0x04
static uint8_t s_toneCoarse[3] = {};   // Reg 0x01, 0x03, 0x05
static uint8_t s_noisePeriod = 0;     // Reg 0x06
static uint8_t s_mixer = 0;           // Reg 0x07
static uint8_t s_vol[3] = {};         // Reg 0x08, 0x09, 0x0A
static uint8_t s_envFine = 0;         // Reg 0x0B
static uint8_t s_envCoarse = 0;       // Reg 0x0C
static uint8_t s_envShape = 0;        // Reg 0x0D

// Tone/noise active state (from mixer reg, 0=on, 1=off)
static bool s_toneOn[3] = {};
static bool s_noiseOn[3] = {};

// ============ Piano State ============
static const int AY_PIANO_LOW = 24;   // C1
static const int AY_PIANO_HIGH = 107; // B7
static const int AY_PIANO_KEYS = AY_PIANO_HIGH - AY_PIANO_LOW + 1;
static bool s_pianoKeyOn[AY_PIANO_KEYS] = {};
static float s_pianoKeyLevel[AY_PIANO_KEYS] = {};
static int s_pianoKeyChannel[AY_PIANO_KEYS] = {};
static float s_visualNote[3] = {}; // smoothed visual note per tone channel
static const bool s_isBlackNote[12] = {false, true, false, true, false, false, true, false, true, false, true, false};

// ============ Level Meter State ============
static float s_channelLevel[AY_NUM_CHANNELS] = {};

// Tone on/off edge detection (from mixer register)
static uint8_t s_prevMixer = 0;
static float s_chDecay[3] = {};         // per-channel decay envelope (tone)
static bool  s_chKeyOff[3] = {};        // key-off state per tone channel

// Noise on/off edge detection
static float s_noiseDecay = {};         // noise decay

// ============ Scope State ============
static ModizerViz s_scope;
static bool s_showScope = false;
static float s_scopeHeight = 80.0f;
static int s_voiceCh[AY_NUM_CHANNELS] = {};
static int s_scopeSamples = 441;
static float s_scopeAmplitude = 3.0f;

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
static int s_histSortMode = 0;
static std::vector<std::string> s_playlist;
static int s_playlistIndex = -1;
static bool s_autoPlayNext = true;
static bool s_isSequentialPlayback = true;

// ============ UI Collapse State ============
static bool s_ymPlayerCollapsed = false;
static bool s_ymHistoryCollapsed = false;
static bool s_showTestPopup = false;

// ============ Config ============
static char s_configPath[MAX_PATH] = "";

// ============ Forward Declarations ============
static void ay8910_mute_all(void);
void ay8910_write_reg(uint8_t reg, uint8_t data);
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
static void RenderSidebar(void);
static void RenderPlayerBar(void);
static void RenderRegisterTable(void);
static void RenderFileBrowser(void);
static void RenderLogPanel(void);
static void RenderMain(void);

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
static FILE* ym_fopen(const char* path, const char* mode) {
    std::wstring wPath = MidiPlayer::UTF8ToWide(std::string(path));
    std::wstring wMode = MidiPlayer::UTF8ToWide(std::string(mode));
    return _wfopen(wPath.c_str(), wMode.c_str());
}

void ay8910_write_reg(uint8_t reg, uint8_t data) {
    int slot = VGMSync::FindChipSlot(VGMSync::CHIP_AY8910);
    if (slot < 0) return;
    ::spfm_write_reg(slot, 0, reg, data);
    if (s_flushMode == 1) {
        ::spfm_hw_wait(1);
    }
}

void safe_flush(void) {
    ::spfm_flush();
}

// Update AY8910 shadow state and UI state for a register write
// Returns the (possibly modified) data byte after fadeout/mute interception
UINT8 UpdateAY8910State(UINT8 reg, UINT8 data) {
    if (reg < 0x10) s_regShadow[reg] = data;

    if (reg <= 0x05) {
        // Tone period registers
        int ch = reg / 2;
        if (reg % 2 == 0) s_toneFine[ch] = data;
        else s_toneCoarse[ch] = data;
    } else if (reg == 0x06) {
        s_noisePeriod = data;
    } else if (reg == 0x07) {
        s_mixer = data;
        // Extract tone/noise enable state (0=on, 1=off)
        for (int i = 0; i < 3; i++) {
            bool prevTone = s_toneOn[i];
            bool newTone = !(data & (1 << i));
            s_toneOn[i] = newTone;
            if (newTone && !prevTone) { s_chDecay[i] = 1.0f; s_chKeyOff[i] = false; }
            else if (!newTone && prevTone) { s_chKeyOff[i] = true; }

            bool prevNoise = s_noiseOn[i];
            bool newNoise = !(data & (1 << (i + 3)));
            s_noiseOn[i] = newNoise;
            if (newNoise && !prevNoise) s_noiseDecay = 1.0f;
        }
        s_prevMixer = data;
    } else if (reg >= 0x08 && reg <= 0x0A) {
        s_vol[reg - 0x08] = data;
    } else if (reg == 0x0B) {
        s_envFine = data;
    } else if (reg == 0x0C) {
        s_envCoarse = data;
    } else if (reg == 0x0D) {
        s_envShape = data;
    }

    // Fadeout: intercept volume writes
    if (reg >= 0x08 && reg <= 0x0A && s_fadeoutActive && s_fadeoutLevel < 1.0f) {
        uint8_t origVol = data & 0x0F;
        uint8_t fadedVol = (uint8_t)(origVol * s_fadeoutLevel);
        data = (data & 0xF0) | (fadedVol & 0x0F);
        s_regShadow[reg] = data;
        s_vol[reg - 0x08] = data;
    }

    // Channel mute: intercept volume writes
    if (reg >= 0x08 && reg <= 0x0A) {
        int ch = reg - 0x08;
        if (s_chMuted[ch]) data = (data & 0xF0) | 0x0F;
    }

    return data;
}

// ============ Timer Mode Sleep Helpers ============

static void sleep_precise_us(unsigned int usec) {
    if (usec < 100) {
        LARGE_INTEGER start, cur;
        QueryPerformanceCounter(&start);
        LONGLONG target = start.QuadPart + (s_perfFreq.QuadPart * usec) / 1000000;
        do { QueryPerformanceCounter(&cur); } while (cur.QuadPart < target);
    } else {
        HANDLE h = CreateWaitableTimer(NULL, TRUE, NULL);
        if (h) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10);
            SetWaitableTimer(h, &due, 0, NULL, NULL, 0);
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }
    }
}

static void sleep_hybrid_us(unsigned int usec) {
    LARGE_INTEGER start, cur;
    QueryPerformanceCounter(&start);
    LONGLONG target = start.QuadPart + (s_perfFreq.QuadPart * usec) / 1000000;
    if (usec > 1000) Sleep((usec - 1000) / 1000);
    do { QueryPerformanceCounter(&cur); } while (cur.QuadPart < target);
}

static void mm_timer_callback(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)uTimerID; (void)uMsg; (void)dwUser; (void)dw1; (void)dw2;
}

static void sleep_mm_timer_us(unsigned int usec) {
    unsigned int ms = usec / 1000;
    if (ms == 0) ms = 1;
    MMRESULT id = timeSetEvent(ms, 1, (LPTIMECALLBACK)mm_timer_callback, 0,
                               TIME_ONESHOT);
    if (id) {
        Sleep(ms + 5);
        timeKillEvent(id);
    } else {
        sleep_hybrid_us(usec);
    }
}

static void timer_sleep_1ms(void) {
    switch (s_timerMode) {
        case 1: sleep_hybrid_us(1000); break;
        case 2: sleep_mm_timer_us(1000); break;
        default: Sleep(1); break;
    }
}

static void ay8910_mute_all(void) {
    if (!s_connected) return;
    // Disable tone and noise for all 3 channels
    ay8910_write_reg(0x07, 0x3F);
    // Set volume to 0 for all 3 channels
    for (int i = 0; i < 3; i++)
        ay8910_write_reg(0x08 + i, 0x00);
    // Zero noise and envelope
    ay8910_write_reg(0x06, 0x00);
    ay8910_write_reg(0x0B, 0x00);
    ay8910_write_reg(0x0C, 0x00);
    ay8910_write_reg(0x0D, 0x00);
    safe_flush();
}

void MuteAll() {
    if (!SPFMManager::IsConnected()) return;
    ay8910_mute_all();
}

static void InitHardware(void) {
    if (!s_connected) return;
    DcLog("[AY] InitHardware\n");
    ay8910_mute_all();
    // Zero all registers
    for (int i = 0; i < 14; i++) {
        ay8910_write_reg(i, 0x00);
    }
    safe_flush();
    DcLog("[AY] Init done\n");
}

static void ResetState(void) {
    s_testRunning = false; s_testType = 0; s_testStep = 0; s_testStepMs = 0.0;
    memset(s_regShadow, 0, sizeof(s_regShadow));
    memset(s_toneFine, 0, sizeof(s_toneFine));
    memset(s_toneCoarse, 0, sizeof(s_toneCoarse));
    s_noisePeriod = 0;
    s_mixer = 0;
    memset(s_vol, 0, sizeof(s_vol));
    s_envFine = 0;
    s_envCoarse = 0;
    s_envShape = 0;
    memset(s_toneOn, 0, sizeof(s_toneOn));
    memset(s_noiseOn, 0, sizeof(s_noiseOn));
    memset(s_pianoKeyOn, 0, sizeof(s_pianoKeyOn));
    memset(s_pianoKeyLevel, 0, sizeof(s_pianoKeyLevel));
    memset(s_pianoKeyChannel, -1, sizeof(s_pianoKeyChannel));
    memset(s_visualNote, 0, sizeof(s_visualNote));
    memset(s_channelLevel, 0, sizeof(s_channelLevel));
    memset(s_chDecay, 0, sizeof(s_chDecay));
    memset(s_chKeyOff, false, sizeof(s_chKeyOff));
    s_noiseDecay = 0;
    s_prevMixer = 0;
    memset(s_chMuted, false, sizeof(s_chMuted));
    s_soloCh = -1;
    s_fadeoutActive = false;
    s_fadeoutLevel = 1.0f;
    s_scope.ResetOffsets();
}

// ============ Period to MIDI Note Conversion ============
static int period_to_midi_note(int ch) {
    // AY8910 tone period: 12-bit = coarse(4bit) << 8 | fine(8bit)
    int period = (s_toneCoarse[ch] << 8) | s_toneFine[ch];
    if (period == 0) return -1;

    double freq = AY8910_CLOCK / (8.0 * period);
    int midiNote = (int)round(69.0 + 12.0 * log2(freq / 440.0));
    return midiNote;
}

// ============ Connection (managed by SPFMManager) ============
static void SyncConnectionState(void) {
    bool wasConnected = s_connected;
    s_connected = SPFMManager::IsConnected();
    if (s_connected && !wasConnected) {
        ResetState();
        InitHardware();
        DcLog("[AY] Hardware connected via SPFMManager\n");
    }
    if (!s_connected && wasConnected) {
        DcLog("[AY] Hardware disconnected\n");
    }
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
    snprintf(s_configPath, MAX_PATH, "%s\\ay8910_config.ini", exeDir);

    char buf[MAX_PATH] = "";
    GetPrivateProfileStringA("Settings", "CurrentPath", "", buf, MAX_PATH, s_configPath);
    if (buf[0]) snprintf(s_currentPath, MAX_PATH, "%s", buf);

    s_ymPlayerCollapsed = GetPrivateProfileIntA("Settings", "PlayerCollapsed", 0, s_configPath) != 0;
    s_ymHistoryCollapsed = GetPrivateProfileIntA("Settings", "HistoryCollapsed", 0, s_configPath) != 0;
    s_autoPlayNext = GetPrivateProfileIntA("Settings", "AutoPlayNext", 1, s_configPath) != 0;
    s_isSequentialPlayback = GetPrivateProfileIntA("Settings", "SequentialPlayback", 1, s_configPath) != 0;

    // Folder history
    s_folderHistory.clear();
    for (int i = 0; i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        char val[MAX_PATH] = "";
        GetPrivateProfileStringA("YmFolderHistory", key, "", val, MAX_PATH, s_configPath);
        if (val[0] == '\0') break;
        s_folderHistory.push_back(std::string(val));
    }

    // Seek & fadeout
    s_seekMode = GetPrivateProfileIntA("Settings", "SeekMode", 0, s_configPath);
    if (s_seekMode < 0 || s_seekMode > 1) s_seekMode = 0;
    s_flushMode = GetPrivateProfileIntA("Settings", "FlushMode", 2, s_configPath);
    if (s_flushMode != 1 && s_flushMode != 2) s_flushMode = 2;
    s_timerMode = GetPrivateProfileIntA("Settings", "TimerMode", 0, s_configPath);
    if (s_timerMode != 0 && s_timerMode != 1 && s_timerMode != 2
        && s_timerMode != 3 && s_timerMode != 7) s_timerMode = 0;
    {
        char val[32] = "";
        GetPrivateProfileStringA("Settings", "FadeoutDuration", "3.0", val, sizeof(val), s_configPath);
        s_fadeoutDuration = (float)atof(val);
        if (s_fadeoutDuration < 0.0f) s_fadeoutDuration = 0.0f;
    }

    // Channel colors
    for (int i = 0; i < 4; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ChColor%d", i);
        char val[32] = "";
        GetPrivateProfileStringA("Colors", key, "", val, sizeof(val), s_configPath);
        if (val[0]) {
            unsigned int c = (unsigned int)strtoul(val, NULL, 16);
            if (c > 0) kChColorsCustom[i] = (ImU32)c;
        }
    }

    DcLog("[AY] Config loaded\n");
}

static void SaveConfig(void) {
    WritePrivateProfileStringA("Settings", "CurrentPath", s_currentPath, s_configPath);
    WritePrivateProfileStringA("Settings", "PlayerCollapsed", s_ymPlayerCollapsed ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "HistoryCollapsed", s_ymHistoryCollapsed ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "AutoPlayNext", s_autoPlayNext ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "SequentialPlayback", s_isSequentialPlayback ? "1" : "0", s_configPath);

    WritePrivateProfileStringA("YmFolderHistory", NULL, NULL, s_configPath);
    for (int i = 0; i < (int)s_folderHistory.size() && i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        WritePrivateProfileStringA("YmFolderHistory", key, s_folderHistory[i].c_str(), s_configPath);
    }

    // Seek & fadeout
    WritePrivateProfileStringA("Settings", "SeekMode", std::to_string(s_seekMode).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "FlushMode", std::to_string(s_flushMode).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "TimerMode", std::to_string(s_timerMode).c_str(), s_configPath);
    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f", s_fadeoutDuration);
        WritePrivateProfileStringA("Settings", "FadeoutDuration", val, s_configPath);
    }

    // Channel colors
    WritePrivateProfileStringA("Colors", NULL, NULL, s_configPath);
    for (int i = 0; i < 4; i++) {
        char key[64], val[32];
        snprintf(key, sizeof(key), "ChColor%d", i);
        if (kChColorsCustom[i] != 0) {
            snprintf(val, sizeof(val), "%08X", kChColorsCustom[i]);
            WritePrivateProfileStringA("Colors", key, val, s_configPath);
        }
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

    // Preview: auto-assign slots from first VGM in folder
    if (!s_playlist.empty()) {
        VGMSync::PreviewAssignSlots(s_playlist[0].c_str());
    }
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
static void UpdateChannelLevels(void) {
    // Clear all piano keys first
    for (int i = 0; i < AY_PIANO_KEYS; i++) {
        s_pianoKeyOn[i] = false;
        s_pianoKeyLevel[i] = 0.0f;
        s_pianoKeyChannel[i] = -1;
    }

    // Tone channels (0-2)
    for (int ch = 0; ch < 3; ch++) {
        bool useEnv = s_vol[ch] & 0x10; // bit 4: use envelope
        uint8_t vol = s_vol[ch] & 0x0F;
        bool keyoff = s_chKeyOff[ch];
        bool toneOn = s_toneOn[ch];

        // Decay logic
        if (keyoff || !toneOn) {
            s_chDecay[ch] *= 0.85f;
            if (s_chDecay[ch] < 0.01f) s_chDecay[ch] = 0.0f;
        } else {
            s_chDecay[ch] *= 0.98f;
            if (s_chDecay[ch] < 0.01f) s_chDecay[ch] = 0.0f;
        }

        float lv;
        if (useEnv) {
            // Envelope mode: level based on decay only (envelope shapes the sound)
            lv = s_chDecay[ch];
        } else {
            lv = (vol >= 15) ? 0.0f : (1.0f - (float)vol / 15.0f) * s_chDecay[ch];
        }
        s_channelLevel[ch] += (lv - s_channelLevel[ch]) * 0.3f;
        if (s_channelLevel[ch] < 0.001f) s_channelLevel[ch] = 0.0f;

        if (s_chMuted[ch]) continue;

        bool kon = s_chDecay[ch] > 0.01f;
        if (kon && s_channelLevel[ch] > 0.01f) {
            int midi = period_to_midi_note(ch);
            if (midi >= AY_PIANO_LOW && midi <= AY_PIANO_HIGH) {
                int idx = midi - AY_PIANO_LOW;
                if (!s_pianoKeyOn[idx] || s_channelLevel[ch] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = s_channelLevel[ch];
                    s_pianoKeyChannel[idx] = ch;
                }
            }
        }
        if (!kon) {
            s_visualNote[ch] = 0.0f;
        }
    }

    // Noise channel (index 3)
    s_noiseDecay *= 0.80f;
    if (s_noiseDecay < 0.01f) s_noiseDecay = 0.0f;

    bool anyNoiseOn = false;
    for (int i = 0; i < 3; i++) {
        if (s_noiseOn[i]) { anyNoiseOn = true; break; }
    }
    float noiseLv = anyNoiseOn ? s_noiseDecay : 0.0f;
    s_channelLevel[3] += (noiseLv - s_channelLevel[3]) * 0.3f;
    if (s_channelLevel[3] < 0.001f) s_channelLevel[3] = 0.0f;
}

// ============ VGM Player ============
static UINT32 ReadLE32(FILE* f) {
    UINT8 b[4]; vgmfread(b, 1, 4, f);
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
    vgmfseek(f, offset, SEEK_SET);
    char sig[4]; vgmfread(sig, 1, 4, f);
    if (memcmp(sig, "gd3", 3) != 0) return;
    vgmfseek(f, offset + 4, SEEK_SET);
    UINT32 strLen = ReadLE32(f);
    UINT32 dataOff = offset + 12;
    vgmfseek(f, dataOff, SEEK_SET);
    std::vector<UINT8> buf(strLen);
    vgmfread(buf.data(), 1, strLen, f);
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
    s_memData.clear();
    s_memData.shrink_to_fit();
    s_memPos = 0;
    s_vgmLoaded = false;
    s_trackName.clear(); s_gameName.clear(); s_systemName.clear(); s_artistName.clear();
    s_vgmTotalSamples = 0; s_vgmCurrentSamples = 0;
    s_vgmTrackEnded = false;

    FILE* f = ym_fopen(path, "rb");
    if (!f) { DcLog("[VGM] Cannot open: %s\n", path); return false; }

    // Detect gzip header (VGZ)
    UINT8 hdr[2];
    if (fread(hdr, 1, 2, f) != 2) { fclose(f); return false; }
    if (hdr[0] == 0x1F && hdr[1] == 0x8B) {
        // VGZ: read entire file, decompress to memory
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<UINT8> compressed(fsize);
        if ((long)fread(compressed.data(), 1, fsize, f) != fsize) { fclose(f); DcLog("[VGM] VGZ read failed\n"); return false; }
        fclose(f); f = nullptr;
        s_memData = InflateGzip(compressed.data(), compressed.size());
        if (s_memData.empty()) { DcLog("[VGM] VGZ decompress failed\n"); return false; }
        DcLog("[VGM] VGZ decompressed: %ld -> %u bytes\n", fsize, (unsigned)s_memData.size());
    } else {
        // Plain VGM: use FILE* directly
        fseek(f, 0, SEEK_SET);
    }

    char sig[4]; vgmfread(sig, 1, 4, f);
    if (memcmp(sig, "Vgm ", 4) != 0) {
        if (f) fclose(f);
        s_memData.clear(); DcLog("[VGM] Not a VGM file\n"); return false;
    }

    vgmfseek(f, 0x08, SEEK_SET);
    s_vgmVersion = ReadLE32(f);

    // AY8910 clock at offset 0x74
    vgmfseek(f, 0x74, SEEK_SET);
    UINT32 ay8910Clock = ReadLE32(f);

    // GD3 tags at 0x14
    vgmfseek(f, 0x14, SEEK_SET);
    UINT32 gd3RelOff = ReadLE32(f);
    UINT32 gd3Off = gd3RelOff ? (gd3RelOff + 0x14) : 0;
    s_vgmTotalSamples = ReadLE32(f);
    UINT32 loopRelOff = ReadLE32(f);
    s_vgmLoopOffset = loopRelOff ? (loopRelOff + 0x1C) : 0;
    s_vgmLoopSamples = ReadLE32(f);

    UINT32 dataOff = 0x40;
    if (s_vgmVersion >= 0x150) {
        vgmfseek(f, 0x34, SEEK_SET);
        UINT32 hdrDataOff = ReadLE32(f);
        if (hdrDataOff > 0) dataOff = hdrDataOff + 0x34;
    }
    s_vgmDataOffset = dataOff;

    DcLog("[VGM] hdr: ver=0x%X dataAbs=0x%X loopAbs=0x%X gd3Abs=0x%X AY8910Clk=%u\n",
        s_vgmVersion, s_vgmDataOffset, s_vgmLoopOffset, gd3Off, ay8910Clock);

    ParseGD3Tags(f, gd3Off);

    if (s_memData.empty()) {
        fclose(f);
        s_vgmFile = ym_fopen(path, "rb");
        if (!s_vgmFile) { DcLog("[VGM] Reopen failed\n"); return false; }
    } else {
        // VGZ: data already in memory, reset position for playback
        s_memPos = 0;
    }

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
    DcLog("[VGM] Clock=%u Total=%.1fs\n", ay8910Clock, (double)s_vgmTotalSamples / 44100.0);
    if (!s_trackName.empty()) DcLog("[VGM] Track: %s\n", s_trackName.c_str());
    if (!s_gameName.empty()) DcLog("[VGM] Game: %s\n", s_gameName.c_str());
    return true;
}

static int s_vgmCmdCount = 0;

// VGM Command Length Table for AY8910 player
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
    // 0x50-0x5F: 50=SN76489(2), 51=YM2413(3), A0=AY8910(3), 52-5F=YM chips(3)
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

// Table-driven VGM command processor for AY8910
// Returns: wait samples (>0), 0 = no wait, -1 = EOF/error
static int VGMProcessCommand(void) {
    UINT8 cmd;
    if (vgmfread(&cmd, 1, 1, s_vgmFile) != 1) return -1;

    if (s_vgmCmdCount < 50) {
        DcLog("[VGM] cmd=0x%02X at %ld\n", cmd, ftell(s_vgmFile) - 1);
    }

    // Special commands with unique behavior
    switch (cmd) {
        case 0xA0: { // AY8910 register write: [0xA0, register, data]  (3 bytes)
            UINT8 reg, data;
            if (vgmfread(&reg, 1, 1, s_vgmFile) != 1) return -1;
            if (vgmfread(&data, 1, 1, s_vgmFile) != 1) return -1;
            s_vgmCmdCount++;

            data = UpdateAY8910State(reg, data);

            // Hardware write
            if (s_connected) {
                ay8910_write_reg(reg, data);
                if (s_flushMode == 2) {
                    safe_flush();
                }
            }
            return 0;
        }
        case 0x61: { // Wait N samples (3 bytes)
            UINT16 wait; if (vgmfread(&wait, 1, 2, s_vgmFile) != 2) return -1;
            return wait;
        }
        case 0x62: return 735;  // Wait 735 samples
        case 0x63: return 882;  // Wait 882 samples
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return (cmd & 0x0F) + 1;
        case 0x66: { // End of data
            if (s_vgmLoopOffset > 0 && (s_vgmMaxLoops == 0 || s_vgmLoopCount < s_vgmMaxLoops)) {
                // Fadeout trigger: on the penultimate 0x66
                if (s_vgmMaxLoops > 0 && s_vgmLoopCount >= s_vgmMaxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeoutActive) {
                    s_fadeoutActive = true;
                    s_fadeoutLevel = 1.0f;
                    s_fadeoutStartSample = s_vgmCurrentSamples;
                    UINT32 fadeoutSamples = (UINT32)(s_fadeoutDuration * 44100.0);
                    if (s_vgmLoopSamples > 0 && fadeoutSamples > s_vgmLoopSamples)
                        fadeoutSamples = s_vgmLoopSamples;
                    s_fadeoutEndSample = s_vgmCurrentSamples + fadeoutSamples;
                }
                vgmfseek(s_vgmFile, s_vgmLoopOffset, SEEK_SET);
                s_vgmLoopCount++;
                return 0;
            }
            return -1;
        }
        case 0x67: { // Data block (variable length)
            UINT8 compat; if (vgmfread(&compat, 1, 1, s_vgmFile) != 1) return -1;
            if (compat != 0x66) return -1;
            UINT8 type; if (vgmfread(&type, 1, 1, s_vgmFile) != 1) return -1;
            UINT32 size; if (vgmfread(&size, 4, 1, s_vgmFile) != 1) return -1;
            vgmfseek(s_vgmFile, size, SEEK_CUR);
            return 0;
        }
    }

    // All other commands: skip using VGM_CMD_LEN table
    UINT8 cmdLen = VGM_CMD_LEN[cmd];
    if (cmdLen <= 1) return 0;
    UINT8 skip = cmdLen - 1;
    if (vgmfseek(s_vgmFile, skip, SEEK_CUR) != 0) return -1;
    return 0;
}

// Optimized VGMPlay: lookahead batch — reads consecutive 0xA0 writes,
// updates shadow/UI state, sends via ay8910_write_reg.
// Returns number of register writes processed.
// *outWait: wait samples (>0), -1 = EOF, -2 = need fallback to VGMProcessCommand.
static int VGMProcessBatch(int* outWait) {
    *outWait = 0;
    const int MAX_BATCH = 64;
    int count = 0;

    while (count < MAX_BATCH && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
        UINT8 cmd;
        if (vgmfread(&cmd, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }

        if (cmd == 0xA0) {
            UINT8 reg, data;
            if (vgmfread(&reg, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            if (vgmfread(&data, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            s_vgmCmdCount++;
            data = UpdateAY8910State(reg, data);
            if (s_connected) {
                ay8910_write_reg(reg, data);
            }
            count++;
        } else if (cmd == 0x62) {
            *outWait = 735; break;
        } else if (cmd == 0x63) {
            *outWait = 882; break;
        } else if (cmd >= 0x70 && cmd <= 0x7F) {
            *outWait = (cmd & 0x0F) + 1; break;
        } else if (cmd == 0x61) {
            UINT16 wait;
            if (vgmfread(&wait, 1, 2, s_vgmFile) != 2) { *outWait = -1; break; }
            *outWait = wait; break;
        } else if (cmd == 0x66) {
            if (s_vgmLoopOffset > 0 && (s_vgmMaxLoops == 0 || s_vgmLoopCount < s_vgmMaxLoops)) {
                if (s_vgmMaxLoops > 0 && s_vgmLoopCount >= s_vgmMaxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeoutActive) {
                    s_fadeoutActive = true;
                    s_fadeoutLevel = 1.0f;
                    s_fadeoutStartSample = s_vgmCurrentSamples;
                    UINT32 fadeoutSamples = (UINT32)(s_fadeoutDuration * 44100.0);
                    if (s_vgmLoopSamples > 0 && fadeoutSamples > s_vgmLoopSamples)
                        fadeoutSamples = s_vgmLoopSamples;
                    s_fadeoutEndSample = s_vgmCurrentSamples + fadeoutSamples;
                }
                vgmfseek(s_vgmFile, s_vgmLoopOffset, SEEK_SET);
                s_vgmLoopCount++;
                // Loop jump produces no wait, continue reading
                continue;
            } else {
                *outWait = -1;
            }
            break;
        } else if (cmd == 0x67) {
            UINT8 compat; if (vgmfread(&compat, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            if (compat != 0x66) { *outWait = -1; break; }
            UINT8 type; if (vgmfread(&type, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            UINT32 size; if (vgmfread(&size, 4, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            vgmfseek(s_vgmFile, size, SEEK_CUR);
            // Data block produces no wait, continue reading
            continue;
        } else {
            // Unknown or non-batchable command: put back, signal fallback
            vgmfseek(s_vgmFile, -1, SEEK_CUR);
            *outWait = -2;
            break;
        }
    }

    // Flush after batch
    if (count > 0 && s_connected) {
        safe_flush();
    }
    return count;
}

// Fadeout helper: shared logic for volume ramp
static bool DoFadeoutUpdate(void) {
    if (!s_fadeoutActive || s_fadeoutDuration <= 0) return false;
    UINT32 fadeRange = s_fadeoutEndSample - s_fadeoutStartSample;
    if (fadeRange == 0) fadeRange = 1;
    float progress = (float)(s_vgmCurrentSamples - s_fadeoutStartSample) / (float)fadeRange;
    if (s_vgmCurrentSamples >= s_fadeoutEndSample) {
        s_fadeoutLevel = 0.0f;
        s_fadeoutActive = false;
    } else {
        s_fadeoutLevel = 1.0f - progress;
    }
    static UINT32 lastFadeSample = 0;
    if (s_vgmCurrentSamples - lastFadeSample >= 441 || s_fadeoutLevel <= 0.0f) {
        lastFadeSample = s_vgmCurrentSamples;
        if (s_connected) {
            for (int ch = 0; ch < 3; ch++) {
                uint8_t origVol = s_vol[ch] & 0x0F;
                uint8_t fadedVol = (uint8_t)(origVol * s_fadeoutLevel);
                uint8_t regData = (s_vol[ch] & 0xF0) | (fadedVol & 0x0F);
                ay8910_write_reg(0x08 + ch, regData);
            }
            safe_flush();
        }
    }
    if (s_fadeoutLevel <= 0.0f) {
        s_vgmTrackEnded = true;
        s_vgmPlaying = false;
        return true;
    }
    return false;
}

static DWORD WINAPI VGMPlaybackThread(LPVOID) {
    QueryPerformanceFrequency(&s_perfFreq);
    double samplesPerTick = 44100.0 / s_perfFreq.QuadPart;

    if (s_timerMode == 3 || s_timerMode == 7) {
        // VGMPlay / OptVGMPlay: 1ms periodic multimedia timer + QPC
        HANDLE mmEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        MMRESULT timerId = timeSetEvent(1, 1, (LPTIMECALLBACK)mmEvent, 0,
                                        TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
        if (!timerId) { CloseHandle(mmEvent); s_vgmThreadRunning = false; return 0; }

        LARGE_INTEGER last;
        QueryPerformanceCounter(&last);
        double samplesToProcess = 0.0;

        while (s_vgmThreadRunning && s_vgmPlaying) {
            if (s_vgmPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }
            WaitForSingleObject(mmEvent, INFINITE);

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            samplesToProcess += (now.QuadPart - last.QuadPart) * samplesPerTick;
            last = now;

            int run = (int)samplesToProcess;
            if (run > 0) {
                int processed = 0;
                while (processed < run && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
                    int s;
                    if (s_timerMode == 7) {
                        // Optimized: lookahead batch
                        int batchWait;
                        VGMProcessBatch(&batchWait);
                        if (batchWait == -2) {
                            // Non-batchable command encountered, fallback
                            s = VGMProcessCommand();
                        } else if (batchWait < 0) {
                            s_vgmTrackEnded = true;
                            s_vgmPlaying = false;
                            break;
                        } else {
                            s = batchWait;
                        }
                    } else {
                        s = VGMProcessCommand();
                        if (s < 0) {
                            s_vgmTrackEnded = true;
                            s_vgmPlaying = false;
                            break;
                        }
                    }
                    if (s > 0) processed += s;
                }
                samplesToProcess -= processed;
                s_vgmCurrentSamples += processed;

                if (DoFadeoutUpdate()) break;
                safe_flush();
            }
        }
        timeKillEvent(timerId);
        CloseHandle(mmEvent);
    } else {
        // Modes 0/1/2: QPC + periodic sleep
        LARGE_INTEGER last;
        QueryPerformanceCounter(&last);
        double samplesToProcess = 0.0;

        while (s_vgmThreadRunning && s_vgmPlaying) {
            if (s_vgmPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
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

                if (DoFadeoutUpdate()) break;
                safe_flush();
            }
            timer_sleep_1ms();
        }
    }
    safe_flush();
    s_vgmThreadRunning = false;
    return 0;
}

static void StartVGMPlayback(void) {
    if (!s_vgmLoaded) return;
    StopTest();
    if (s_connected) InitHardware();
    VGMSync::SetTimerMode(s_timerMode);
    VGMSync::SetFadeout(s_fadeoutDuration);
    VGMSync::SetMaxLoops(s_vgmMaxLoops);
    VGMSync::SetTotalSamples(s_vgmTotalSamples);
    VGMSync::StartUnifiedPlayback(s_vgmPath, s_timerMode);
    s_vgmPlaying = true;
    s_vgmPaused = false;
}

static void StopVGMPlayback(void) {
    VGMSync::StopUnifiedPlayback();
    s_vgmPlaying = false; s_vgmPaused = false;
    if (s_connected) InitHardware();
}

static void PauseVGMPlayback(void) {
    VGMSync::PauseUnifiedPlayback();
    s_vgmPaused = VGMSync::IsUnifiedPaused();
}

static void OpenVGMFileDialog(void) {
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "VGM Files\0*.vgm;*.vgz\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        LoadVGMFile(path);
        VGMSync::AutoAssignSlots(path);
        VGMSync::NotifyFileOpened(path);
    }
}

static void SeekVGMToStart(void) {
    if (!s_vgmLoaded) return;
    if (s_vgmPlaying) { s_vgmPlaying = false; s_vgmPaused = false; }
    if (s_connected) InitHardware();
    if (s_memData.empty()) {
        if (s_vgmFile) fclose(s_vgmFile);
        s_vgmFile = ym_fopen(s_vgmPath, "rb");
        if (!s_vgmFile) return;
        vgmfseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
    } else {
        s_memPos = s_vgmDataOffset;
    }
    s_vgmCurrentSamples = 0; s_vgmLoopCount = 0;
    s_vgmTrackEnded = false;
    s_fadeoutActive = false; s_fadeoutLevel = 1.0f;
}

static void PlayPlaylistNext(void) {
    if (s_playlist.empty()) return;
    const char* nextPath;
    if (s_isSequentialPlayback) {
        int next = s_playlistIndex + 1;
        if (next >= (int)s_playlist.size()) next = 0;
        nextPath = s_playlist[next].c_str();
    } else {
        int next = rand() % (int)s_playlist.size();
        nextPath = s_playlist[next].c_str();
    }
    if (LoadVGMFile(nextPath)) {
        VGMSync::AutoAssignSlots(nextPath);
        VGMSync::NotifyFileOpened(nextPath);
        StartVGMPlayback();
    }
}

static void PlayPlaylistPrev(void) {
    if (s_playlist.empty()) return;
    int prev = s_playlistIndex - 1;
    if (prev < 0) prev = (int)s_playlist.size() - 1;
    const char* prevPath = s_playlist[prev].c_str();
    if (LoadVGMFile(prevPath)) {
        VGMSync::AutoAssignSlots(prevPath);
        VGMSync::NotifyFileOpened(prevPath);
        StartVGMPlayback();
    }
}

// ============ Test Functions ============
static double GetTestElapsedMs(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_testStartTime.QuadPart) / s_perfFreq.QuadPart * 1000.0;
}

static double GetStepDurationMs(int type) {
    switch (type) {
        case 0: return 500.0;  // Instrument demo
        case 1: return 300.0;  // Scale
        default: return 300.0;
    }
}

// Helper: convert MIDI note to AY8910 tone period
static void midi_to_period(int midiNote, int& fine, int& coarse) {
    double freq = 440.0 * pow(2.0, (midiNote - 69.0) / 12.0);
    int period = (int)round(AY8910_CLOCK / (8.0 * freq));
    if (period < 1) period = 1;
    if (period > 4095) period = 4095;
    fine = period & 0xFF;
    coarse = (period >> 8) & 0x0F;
}

// Key off all tone channels
static void key_off_all(void) {
    // Mute all tone channels via mixer
    ay8910_write_reg(0x07, 0x3F);
    safe_flush();
}

static void TestStep(void) {
    switch (s_testType) {
        case 0: {
            // Tone channel demo: play C major scale across 3 channels
            static const uint8_t notes[] = {60, 62, 64, 65, 67, 69, 71, 72};
            if (s_testStep >= 8) { s_testRunning = false; key_off_all(); return; }

            int fine, coarse;
            midi_to_period(notes[s_testStep], fine, coarse);

            // Key off all first
            key_off_all();

            // Set volume on channel 0
            ay8910_write_reg(0x08, 0x0F); // max volume
            // Set period
            ay8910_write_reg(0x00, fine);
            ay8910_write_reg(0x01, coarse);
            // Key-on: enable tone for channel A
            ay8910_write_reg(0x07, ~(1 << 0)); // enable tone ch A
            safe_flush();

            // Update shadow state
            s_vol[0] = 0x0F;
            s_toneFine[0] = fine;
            s_toneCoarse[0] = coarse;
            s_mixer = ~(1 << 0);
            s_toneOn[0] = true;
            s_chDecay[0] = 1.0f;
            s_chKeyOff[0] = false;
            break;
        }
        case 1: {
            // Noise channel test
            if (s_testStep >= 8) { s_testRunning = false; key_off_all(); return; }

            key_off_all();

            // Enable noise on channel A
            ay8910_write_reg(0x08, 0x0F); // max volume
            ay8910_write_reg(0x06, s_testStep * 2 + 1); // varying noise period
            ay8910_write_reg(0x07, ~(1 << 3)); // enable noise ch A
            safe_flush();

            s_vol[0] = 0x0F;
            s_noisePeriod = s_testStep * 2 + 1;
            s_mixer = ~(1 << 3);
            s_noiseOn[0] = true;
            s_noiseDecay = 1.0f;
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
}

// ============ Public API ============
void Init() {
    QueryPerformanceFrequency(&s_perfFreq);
    s_scope.Init();
    LoadConfig();

    // Register unified playback callbacks
    VGMSync::RegisterChipWriter(VGMSync::CHIP_AY8910,
        (VGMSync::ChipStateUpdateFn)UpdateAY8910State,
        (VGMSync::ChipHwWriteFn)ay8910_write_reg,
        (VGMSync::ChipFlushFn)safe_flush);

    if (s_currentPath[0] != '\0') {
        RefreshFileList();
    } else {
        GetExeDir(s_currentPath, MAX_PATH);
        RefreshFileList();
    }
}

void Shutdown() {
    s_vgmPlaying = false;
    SaveConfig();
    s_connected = false;
}

void Update() {
    SyncConnectionState();
    // Check shared VGM file from other windows
    {
        const char* shared = VGMSync::GetSharedFilePath();
        if (shared[0] && strcmp(shared, s_vgmPath) != 0) {
            DcLog("[AY] SyncLoad: %s (loaded=%d)\n", shared, s_vgmLoaded);
            bool ok = LoadVGMFile(shared);
            DcLog("[AY] SyncLoad result: ok=%d loaded=%d\n", ok, s_vgmLoaded);
        }
    }
    // Sync playback state from unified playback
    if (VGMSync::IsUnifiedPlaying() && !s_vgmPlaying) {
        s_vgmPlaying = true;
        s_vgmPaused = false;
        s_vgmTrackEnded = false;
    }
    if (!VGMSync::IsUnifiedPlaying() && s_vgmPlaying) {
        s_vgmPlaying = false;
        s_vgmPaused = false;
    }
    // Sync current progress from unified playback
    if (s_vgmPlaying) {
        s_vgmCurrentSamples = VGMSync::GetCurrentSamples();
    }
    UpdateChannelLevels();
    if (s_vgmTrackEnded && !s_vgmPlaying) {
        s_vgmTrackEnded = false;
        if (s_autoPlayNext && !s_playlist.empty()) PlayPlaylistNext();
    }

    // Update scope voice channel offsets
    static int scopeCheckCounter = 0;
    if (++scopeCheckCounter >= 60) {
        scopeCheckCounter = 0;
        ScopeChipSlot *slot = scope_find_slot("AY8910", 0);
        if (slot) {
            for (int i = 0; i < AY_NUM_CHANNELS; i++) s_voiceCh[i] = slot->slot_base + i;
        } else {
            for (int i = 0; i < AY_NUM_CHANNELS; i++) s_voiceCh[i] = -1;
        }
    }

    // Test
    if (s_testRunning) {
        double elapsed = GetTestElapsedMs();
        if (elapsed >= s_testStepMs) { TestStep(); s_testStepMs += GetStepDurationMs(s_testType); }
    }
}

// ============ Piano Keyboard ============
static ImU32 getChColor(int ch) {
    if (ch >= 0 && ch < AY_NUM_CHANNELS) {
        if (kChColorsCustom[ch] != 0) return kChColorsCustom[ch];
        return kChColors[ch];
    }
    return IM_COL32(160, 200, 160, 255);
}

static ImU32 blendKey(ImU32 col, float lv, bool isBlack) {
    float blendLv = 0.3f + 0.7f * powf(lv, 0.5f);
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
    ImU32 col = (ch >= 0) ? getChColor(ch) : IM_COL32(160, 200, 160, 255);
    return blendKey(col, level, false);
}

static ImU32 getKeyColorBlack(int idx, float level) {
    int ch = s_pianoKeyChannel[idx];
    ImU32 col = (ch >= 0) ? getChColor(ch) : IM_COL32(160, 200, 160, 255);
    return blendKey(col, level, true);
}

static void RenderPianoKeyboard(void) {
    ImGui::BeginChild("AY_Piano", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float whiteKeyH = 100.0f;
    float blackKeyH = 60.0f;

    const int kMinNote = AY_PIANO_LOW;
    const int kMaxNote = AY_PIANO_HIGH;

    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) if (!s_isBlackNote[n % 12]) numWhiteKeys++;

    float whiteKeyW = availW / (float)numWhiteKeys;
    if (whiteKeyW < 6.0f) whiteKeyW = 6.0f;
    float blackKeyW = whiteKeyW * 0.65f;

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (s_isBlackNote[n % 12]) continue;
        int idx = n - AY_PIANO_LOW;
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
        int idx = n - AY_PIANO_LOW;
        float x = p.x + (wkIdx - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;
        ImU32 fillCol = (idx >= 0 && idx < AY_PIANO_KEYS && s_pianoKeyOn[idx])
            ? getKeyColorBlack(idx, s_pianoKeyLevel[idx]) : IM_COL32(0, 0, 0, 255);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), fillCol);
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), IM_COL32(128, 128, 128, 255));
    }

    // (AY8910 has no vibrato/tremolo/portamento, so Pass 3 indicators are omitted)

    ImGui::EndChild();
}

// ============ Level Meters ============
static void ApplyChannelMute(int i) {
    if (i < 3) {
        // Tone channel mute: set volume to 0xF (max attenuation)
        uint8_t regData;
        if (s_chMuted[i]) {
            regData = (s_vol[i] & 0xF0) | 0x0F;
        } else {
            regData = s_vol[i];
        }
        if (s_connected) {
            ay8910_write_reg(0x08 + i, regData);
            safe_flush();
        }
    }
    // Noise channel (3): can mute by setting volume to 0xF on all channels using noise
}

static void RenderLevelMeters(void) {
    ImGui::BeginChild("AY_LevelMeters", ImVec2(0, 0), true);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    float groupW = availW / (float)AY_NUM_CHANNELS;
    float meterW = groupW - 4.0f;
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

    // Scroll wheel invert all
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
        for (int j = 0; j < AY_NUM_CHANNELS; j++) s_chMuted[j] = !s_chMuted[j];
        s_soloCh = -1;
        for (int j = 0; j < AY_NUM_CHANNELS; j++) ApplyChannelMute(j);
    }

    for (int i = 0; i < AY_NUM_CHANNELS; i++) {
        float centerX = p.x + i * groupW + groupW * 0.5f;
        float mY = p.y + labelH;
        float meterLeft = centerX - meterW * 0.5f;
        float meterRight = centerX + meterW * 0.5f;
        float meterBottom = mY + meterH;

        // Channel label
        const char* label = kChNames[i];
        ImU32 labelCol = kChColors[i];
        bool isMuted = s_chMuted[i];
        bool isSolo = (s_soloCh == i);

        ImVec2 textSize = ImGui::CalcTextSize(label);
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
        snprintf(btnId, sizeof(btnId), "##aych%d", i);
        if (ImGui::Button(btnId, ImVec2(btnW, btnH))) {
            if (s_soloCh >= 0) {
                s_soloCh = -1;
                s_chMuted[i] = !s_chMuted[i];
            } else {
                s_chMuted[i] = !s_chMuted[i];
            }
            ApplyChannelMute(i);
        }
        if (ImGui::IsItemClicked(1)) {
            if (s_soloCh == i) {
                s_soloCh = -1;
                for (int j = 0; j < AY_NUM_CHANNELS; j++) { s_chMuted[j] = false; ApplyChannelMute(j); }
            } else {
                s_soloCh = i;
                for (int j = 0; j < AY_NUM_CHANNELS; j++) {
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

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        // Overlay label text
        dl->AddText(ImVec2(centerX - textSize.x * 0.5f, p.y + 3),
                     isMuted ? IM_COL32(255, 200, 200, 255) : labelCol, label);

        // Background
        dl->AddRectFilled(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(30, 30, 30, 255));
        dl->AddRect(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(100, 100, 100, 255));

        float level = s_channelLevel[i];
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

        // Mute overlay + X
        if (s_chMuted[i]) {
            dl->AddRectFilled(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(0, 0, 0, 120));
            dl->AddLine(ImVec2(meterLeft + 2, mY + 2), ImVec2(meterRight - 2, meterBottom - 2), IM_COL32(255, 80, 80, 200), 2);
            dl->AddLine(ImVec2(meterRight - 2, mY + 2), ImVec2(meterLeft + 2, meterBottom - 2), IM_COL32(255, 80, 80, 200), 2);
        }
        // Solo highlight
        if (s_soloCh == i) {
            dl->AddRect(ImVec2(meterLeft - 1, mY - 1), ImVec2(meterRight + 1, meterBottom + 1), IM_COL32(255, 220, 50, 200), 0, 0, 2);
        }

        // Volume / status text
        char volStr[8];
        if (i < 3) {
            uint8_t vol = s_vol[i] & 0x0F;
            snprintf(volStr, sizeof(volStr), "%d", vol);
        } else {
            // Noise channel: show noise period
            snprintf(volStr, sizeof(volStr), "N%d", s_noisePeriod & 0x1F);
        }
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
    ImGui::BeginChild("AY_Scope", ImVec2(0, s_scopeHeight), true);

    bool hasScope = false;
    for (int i = 0; i < AY_NUM_CHANNELS; i++) {
        if (s_voiceCh[i] >= 0) { hasScope = true; break; }
    }

    if (hasScope) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;
        float chW = availW / (float)AY_NUM_CHANNELS - 4.0f;

        for (int i = 0; i < AY_NUM_CHANNELS; i++) {
            if (s_voiceCh[i] < 0) continue;
            float x = p.x + i * (chW + 4.0f) + 2.0f;
            bool keyOn = false;
            float level = 0.0f;
            if (i < 3) {
                keyOn = s_chDecay[i] > 0.01f;
                level = s_channelLevel[i];
            } else {
                keyOn = s_noiseDecay > 0.01f;
                level = s_channelLevel[i];
            }

            s_scope.DrawChannel(s_voiceCh[i], dl, x, p.y + 16, chW, availH - 16,
                s_scopeAmplitude, kChColors[i], keyOn, level,
                s_scopeSamples, 0, 735, true, true, 1, false);
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Scope requires libvgm AY8910 core integration)");
    }

    ImGui::EndChild();
}

// ============ Test & Channel Controls Window ============
static void RenderTestPopup(void) {
    if (!s_showTestPopup) return;
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AY8910 Debug##aytestwin", &s_showTestPopup)) { ImGui::End(); return; }

    // Hardware Tests
    if (ImGui::CollapsingHeader("Hardware Tests##aytest", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Tone Scale##aytest0", ImVec2(-1, 0))) StartTest(0);
        if (ImGui::Button("Noise Test##aytest1", ImVec2(-1, 0))) StartTest(1);
        if (s_testRunning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Stop Test##ayteststop", ImVec2(-1, 0))) StopTest();
            ImGui::PopStyleColor();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running...");
            if (s_testType == 0) {
                ImGui::Text("Note %d/8", s_testStep);
            } else if (s_testType == 1) {
                ImGui::Text("Noise period %d/8", s_testStep * 2 + 1);
            }
        }
    }

    // Quick note test
    if (ImGui::CollapsingHeader("Quick Note##ayqnote", nullptr, 0)) {
        static int qNote = 60;
        static int qVol = 15;
        static int qCh = 0;
        ImGui::SliderInt("Note##ayqn", &qNote, 24, 96);
        ImGui::SliderInt("Volume##ayqv", &qVol, 0, 15);
        ImGui::SliderInt("Channel##ayqc", &qCh, 0, 2);
        ImGui::SameLine(); ImGui::Text("%s", kChNames[qCh]);
        if (ImGui::Button("Play Note##ayqplay", ImVec2(-1, 0))) {
            if (s_connected && !s_vgmPlaying) {
                int fine, coarse;
                midi_to_period(qNote, fine, coarse);
                key_off_all();
                ay8910_write_reg(0x08 + qCh, (uint8_t)(qVol & 0x0F));
                ay8910_write_reg(qCh * 2, (uint8_t)fine);
                ay8910_write_reg(qCh * 2 + 1, (uint8_t)coarse);
                // Enable tone for selected channel
                uint8_t mixer = 0x3F;
                mixer &= ~(1 << qCh); // enable tone
                ay8910_write_reg(0x07, mixer);
                safe_flush();

                // Update shadow state
                s_vol[qCh] = (uint8_t)(qVol & 0x0F);
                s_toneFine[qCh] = (uint8_t)fine;
                s_toneCoarse[qCh] = (uint8_t)coarse;
                s_mixer = mixer;
                s_toneOn[qCh] = true;
                s_chDecay[qCh] = 1.0f;
                s_chKeyOff[qCh] = false;
            }
        }
        if (ImGui::Button("Key Off##ayqoff", ImVec2(-1, 0))) {
            if (s_connected) { key_off_all(); safe_flush(); }
        }
    }

    ImGui::End();
}

// ============ UI Rendering ============
static void RenderSidebar(void) {
    if (!ImGui::CollapsingHeader("AY8910 Hardware##ay", nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Connection status
    if (SPFMManager::IsConnected()) {
        const char* desc = (SPFMManager::GetActiveChipType() == SPFMManager::CHIP_AY8910)
            ? "AY8910" : SPFMManager::GetActiveChipType() == SPFMManager::CHIP_YM2163 ? "YM2163"
            : SPFMManager::GetActiveChipType() == SPFMManager::CHIP_SN76489 ? "SN76489" : "None";
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected (%s)", desc);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Disconnected");
    }

    // Connect / Disconnect buttons
    {
        bool isAY = (SPFMManager::GetActiveChipType() == SPFMManager::CHIP_AY8910);
        if (isAY) {
            if (ImGui::Button("Disconnect##ay", ImVec2(-1, 0))) {
                SPFMManager::SwitchToChipType(SPFMManager::CHIP_NONE);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set chip type to None, send SPFM reset");
        } else {
            if (ImGui::Button("Connect##ay", ImVec2(-1, 0))) {
                SPFMManager::SwitchToChipType(SPFMManager::CHIP_AY8910);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch to AY8910 (PSG) mode");
        }
    }

    // SPFM Slot assignment
    {
        ImGui::Spacing();
        static const char* chipLabels[] = {"(none)","YM2413","AY8910","SN76489"};
        for (int s = 0; s < 4; s++) {
            int cur = VGMSync::GetSlotChip(s);
            ImGui::Text("Slot %d:", s); ImGui::SameLine();
            if (ImGui::Combo(("##slot_ay_"+std::to_string(s)).c_str(), &cur, chipLabels, 4)) {
                VGMSync::SetSlotChip(s, cur);
                VGMSync::SaveSlotPreset(VGMSync::GetLastComboKey());
            }
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // Test popup button
    if (s_testRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Test Running...##aytestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Debug Test##aytestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Loop settings
    ImGui::TextDisabled("VGM Loop Count");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max internal loop repetitions\n0 = infinite loop");
    ImGui::SameLine();
    int maxL = s_vgmMaxLoops;
    if (ImGui::InputInt("##aymaxloops", &maxL, 1, 5)) {
        if (maxL < 0) maxL = 0;
        s_vgmMaxLoops = maxL;
    }

    // Seek mode
    ImGui::TextDisabled("Seek Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fast-forward: replay commands from start\nDirect: skip without HW, reset chip on target");
    ImGui::RadioButton("Fast-forward##ayseek", &s_seekMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Direct##ayseek", &s_seekMode, 1);

    // Fadeout
    ImGui::TextDisabled("Loop Fadeout");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fade out volume before final loop end\n0 = disabled");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragFloat("##ayfadeout", &s_fadeoutDuration, 0.1f, 0.0f, 30.0f, "%.1f sec")) {
        if (s_fadeoutDuration < 0.0f) s_fadeoutDuration = 0.0f;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Flush Mode
    ImGui::TextDisabled("Flush Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Register: flush after each reg write\nCommand: flush after each VGM command");
    if (ImGui::RadioButton("Register##ayflush", &s_flushMode, 1)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Command##ayflush", &s_flushMode, 2)) SaveConfig();

    // Timer Mode
    ImGui::TextDisabled("Timer Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("H-Prec: waitable timer+spin\nHybrid: Sleep+spin\nMM-Timer: timeSetEvent\nVGMPlay: 1ms periodic timer\nOptVGMPlay: periodic+batch lookahead");
    if (ImGui::RadioButton("H-Prec##aytimer", &s_timerMode, 0)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Hybrid##aytimer", &s_timerMode, 1)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("MM-Timer##aytimer", &s_timerMode, 2)) SaveConfig();
    if (ImGui::RadioButton("VGMPlay##aytimer", &s_timerMode, 3)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("OptVGMPlay##aytimer", &s_timerMode, 7)) SaveConfig();

    ImGui::Spacing(); ImGui::Separator();

    // Scope settings
    if (ImGui::CollapsingHeader("Scope##ayscope", nullptr, 0)) {
        ImGui::Checkbox("Show Scope##ay", &s_showScope);
        if (s_showScope) {
            ImGui::SliderFloat("Height##ay", &s_scopeHeight, 20, 300);
            ImGui::SliderFloat("Amplitude##ay", &s_scopeAmplitude, 0.5f, 10.0f);
            ImGui::SliderInt("Samples##ay", &s_scopeSamples, 100, 1000);
        }
    }

    // Channel Colors
    if (ImGui::CollapsingHeader("Channel Colors##aychcolors")) {
        for (int i = 0; i < 4; i++) {
            ImGui::PushID(i);
            float colF[4];
            ImU32 curCol = kChColorsCustom[i];
            ImU32 defCol = kChColors[i];
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
            if (ImGui::ColorEdit4(("##ayclredit" + std::to_string(i)).c_str(), colF, ImGuiColorEditFlags_NoInputs)) {
                kChColorsCustom[i] = IM_COL32(
                    (int)(colF[0] * 255 + 0.5f), (int)(colF[1] * 255 + 0.5f),
                    (int)(colF[2] * 255 + 0.5f), (int)(colF[3] * 255 + 0.5f));
                SaveConfig();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(kChNames[i]);
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
        if (ImGui::SmallButton("Reset All Colors##ayrclrall")) {
            memset(kChColorsCustom, 0, sizeof(kChColorsCustom));
            SaveConfig();
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // VGM File Info
    if (ImGui::CollapsingHeader("VGM File Info##ayinfo", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
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

            // Mixer status
            ImGui::TextDisabled("Mixer:"); ImGui::SameLine();
            ImGui::Text("0x%02X", s_mixer);

            // Envelope shape
            ImGui::TextDisabled("EnvShape:"); ImGui::SameLine();
            ImGui::Text("%s", (s_envShape < 16) ? kEnvShapeNames[s_envShape] : "---");

            ImGui::Spacing();
            if (!s_trackName.empty()) {
                ImGui::TextDisabled("Track:");
                ImGui::Indent();
                DrawScrollingText("##aytrack", s_trackName.c_str(), IM_COL32(255, 255, 102, 255));
                ImGui::Unindent();
            }
            if (!s_gameName.empty()) {
                ImGui::TextDisabled("Game:");
                ImGui::SameLine();
                DrawScrollingText("##aygame", s_gameName.c_str(), IM_COL32(200, 200, 200, 255));
            }
            if (!s_systemName.empty()) {
                ImGui::TextDisabled("System:");
                ImGui::SameLine();
                ImGui::Text("%s", s_systemName.c_str());
            }
            if (!s_artistName.empty()) {
                ImGui::TextDisabled("Artist:");
                ImGui::SameLine();
                DrawScrollingText("##ayartist", s_artistName.c_str(), IM_COL32(200, 200, 200, 255));
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
    bool playerOpen = ImGui::TreeNodeEx("AY8910 VGM Player##ayvgmplayer",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_ymPlayerCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));

    if (playerOpen) s_ymPlayerCollapsed = false;
    else s_ymPlayerCollapsed = true;

    // Progress bar when collapsed
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

    // Title bar mini controls
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("<<##ayminiprev")) {
        if (!s_playlist.empty()) PlayPlaylistPrev();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous");
    ImGui::SameLine();
    if (isPlaying) {
        if (ImGui::SmallButton("||##ayminipause")) PauseVGMPlayback();
    } else if (isPaused) {
        if (ImGui::SmallButton(">##ayminipause")) PauseVGMPlayback();
    } else if (hasFile) {
        if (ImGui::SmallButton(">##ayminipause")) StartVGMPlayback();
    } else {
        ImGui::SmallButton(">##ayminipause");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play / Pause");
    ImGui::SameLine();
    if (ImGui::SmallButton(">>##aymininext")) {
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
        DrawScrollingText("##ayminifilename", displayName, nameCol, maxNameW);
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
        if (ImGui::Button("Play##ayplay", ImVec2(buttonWidth, 30))) {
            if (!hasFile) { /* nothing */ }
            else if (isPaused) PauseVGMPlayback();
            else { SeekVGMToStart(); StartVGMPlayback(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause##aypause", ImVec2(buttonWidth, 30))) {
            if (isPlaying) PauseVGMPlayback();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop##aystop", ImVec2(buttonWidth, 30))) {
            s_vgmTrackEnded = false;
            StopVGMPlayback();
        }

        float navWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
        if (ImGui::Button("<< Prev##ayprev", ImVec2(navWidth, 25))) {
            if (!s_playlist.empty()) PlayPlaylistPrev();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>##aynext", ImVec2(navWidth, 25))) {
            if (!s_playlist.empty()) PlayPlaylistNext();
        }

        ImGui::Checkbox("Auto-play##ay", &s_autoPlayNext);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-play next track when current finishes");
        ImGui::SameLine();
        const char* modeText = s_isSequentialPlayback ? "Seq" : "Rnd";
        if (ImGui::Button(modeText, ImVec2(35, 0))) s_isSequentialPlayback = !s_isSequentialPlayback;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sequential / Random");
        ImGui::SameLine();
        if (ImGui::Button("Open##ayopen")) OpenVGMFileDialog();

        // Progress bar with seek
        if (hasFile && s_vgmTotalSamples > 0) {
            double posSec = (double)s_vgmCurrentSamples / 44100.0;
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
                ImGui::TextDisabled("       ");

            static float seek_progress = 0.0f;
            ImGui::SliderFloat("##ayseek", &seek_progress, 0.0f, 1.0f, "");
            if (ImGui::IsItemActive()) {
                // Dragging
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
                double totalDurSec;
                if (s_vgmLoopSamples > 0 && s_vgmMaxLoops > 0) {
                    totalDurSec = (double)(s_vgmTotalSamples - s_vgmLoopSamples) / 44100.0
                                + (double)s_vgmLoopSamples / 44100.0 * s_vgmMaxLoops;
                } else {
                    totalDurSec = (double)s_vgmTotalSamples / 44100.0;
                }
                UINT32 targetSample = (UINT32)(seek_progress * totalDurSec * 44100.0);
                // Stop thread
                s_vgmThreadRunning = false; s_vgmPlaying = false;
                if (s_vgmThread) { WaitForSingleObject(s_vgmThread, 2000); CloseHandle(s_vgmThread); s_vgmThread = nullptr; }
                if (s_memData.empty()) {
                    if (s_vgmFile) { fclose(s_vgmFile); s_vgmFile = ym_fopen(s_vgmPath, "rb"); }
                }
                if (!s_memData.empty() || s_vgmFile) {
                    // Parse commands to target without HW writes
                    if (s_memData.empty()) {
                        vgmfseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
                    } else {
                        s_memPos = s_vgmDataOffset;
                    }
                    bool wasConn = s_connected;
                    s_connected = false;
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
                    s_connected = wasConn;
                    if (s_connected) {
                        InitHardware();
                    }
                    // Restart playback
                    s_vgmPlaying = true;
                    s_vgmPaused = false;
                    s_vgmThreadRunning = true;
                    s_vgmThread = CreateThread(NULL, 0, VGMPlaybackThread, NULL, 0, NULL);
                }
            } else {
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
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "AY8910 Registers");
    ImGui::Separator();

    // Tone period registers
    ImGui::TextDisabled("-- Tone Period --");
    static const char* toneChNames[3] = { "ChA", "ChB", "ChC" };
    if (ImGui::BeginTable("##ay8910tone", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 32.f);
        ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 3; ch++) {
            int period = (s_toneCoarse[ch] << 8) | s_toneFine[ch];
            double freq = (period > 0) ? AY8910_CLOCK / (8.0 * period) : 0.0;
            int midi = (period > 0) ? period_to_midi_note(ch) : -1;
            bool active = s_toneOn[ch] && period > 0;
            ImVec4 col = active ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "%s", toneChNames[ch]);
            ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%d", period);
            ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%.1f Hz", freq);
            char noteBuf[8] = "---";
            if (midi >= 0 && midi < 128)
                snprintf(noteBuf, sizeof(noteBuf), "%s%d", kNoteNames[midi % 12], midi / 12 - 1);
            ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s", noteBuf);
        }
        ImGui::EndTable();
    }

    // Noise & Mixer
    ImGui::Spacing();
    ImGui::TextDisabled("-- Noise & Mixer --");
    if (ImGui::BeginTable("##ay8910mixer", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Noise");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_noisePeriod);
        ImGui::TableSetColumnIndex(2); ImGui::Text("Period %d", s_noisePeriod & 0x1F);
        bool anyNoise = false;
        for (int i = 0; i < 3; i++) if (s_noiseOn[i]) { anyNoise = true; break; }
        ImGui::TableSetColumnIndex(3); ImGui::TextColored(anyNoise ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1),
            anyNoise ? "ON" : "OFF");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Mixer");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_mixer);
        ImGui::TableSetColumnIndex(2); ImGui::Text("Tone/Nrs mask");
        ImGui::TableSetColumnIndex(3);
        char mixerStr[16];
        snprintf(mixerStr, sizeof(mixerStr), "%c%c%c/%c%c%c",
            s_toneOn[0] ? 'A' : '-', s_toneOn[1] ? 'B' : '-', s_toneOn[2] ? 'C' : '-',
            s_noiseOn[0] ? 'A' : '-', s_noiseOn[1] ? 'B' : '-', s_noiseOn[2] ? 'C' : '-');
        ImGui::Text("%s", mixerStr);
        ImGui::EndTable();
    }

    // Volume registers
    ImGui::Spacing();
    ImGui::TextDisabled("-- Volume --");
    if (ImGui::BeginTable("##ay8910vol", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 32.f);
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 3; ch++) {
            bool useEnv = s_vol[ch] & 0x10;
            uint8_t vol = s_vol[ch] & 0x0F;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", toneChNames[ch]);
            ImGui::TableSetColumnIndex(1); ImGui::Text("0x%02X", 0x08 + ch);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%02X", s_vol[ch]);
            ImGui::TableSetColumnIndex(3); ImGui::Text(useEnv ? "ENV" : "Vol %d", vol);
        }
        ImGui::EndTable();
    }

    // Envelope registers
    ImGui::Spacing();
    ImGui::TextDisabled("-- Envelope --");
    if (ImGui::BeginTable("##ay8910env", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthFixed, 30.f);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Fine");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_envFine);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%d", s_envFine);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Coarse");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_envCoarse);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%d", s_envCoarse);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("Shape");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_envShape);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%s", (s_envShape < 16) ? kEnvShapeNames[s_envShape] : "---");
        ImGui::EndTable();
    }

    // All Regs hex dump
    ImGui::Spacing();
    if (ImGui::TreeNode("All Regs##ay8910")) {
        if (ImGui::BeginTable("##ay8910allregs", 16,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (int col = 0; col < 16; col++) {
                ImGui::TableSetupColumn(col == 0 ? "Reg" : "Val", ImGuiTableColumnFlags_WidthFixed, 26.f);
            }
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            for (int col = 0; col < 16; col++) {
                ImGui::TableSetColumnIndex(col);
                if (col == 0) ImGui::Text("");
                else ImGui::Text("%X", col);
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("00");
            for (int col = 1; col < 16; col++) {
                ImGui::TableSetColumnIndex(col);
                ImGui::Text("%02X", s_regShadow[col - 1]);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

static void RenderFileBrowser(void) {
    ImGui::SetNextItemAllowOverlap();
    bool browserOpen = ImGui::TreeNodeEx("AY8910 File Browser##ayfb",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_DefaultOpen);

    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##ayfbfilter", "Filter...", s_fileBrowserFilter, sizeof(s_fileBrowserFilter));

    if (!browserOpen) return;

    // Navigation buttons
    if (ImGui::Button("<##ayfbback", ImVec2(25, 0))) NavBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">##ayfbfwd", ImVec2(25, 0))) NavForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^##ayfbup", ImVec2(25, 0))) NavToParent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");
    ImGui::SameLine();

    // Breadcrumb path bar
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
            if (ImGui::Button("...##ayellipsis")) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
            }
            ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine();
        }
        for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
            std::string btnId = segments[i] + "##ayseg" + std::to_string(i);
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
            ImGui::InvisibleButton("##aypathEmpty", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ayPathInput", s_pathInput, MAX_PATH, ImGuiInputTextFlags_EnterReturnsTrue)) {
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
    ImGui::BeginChild("AYFileList##ayfl", ImVec2(-1, fileAreaHeight), true);

    for (int i = 0; i < (int)s_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = s_fileList[i];
        ImGui::PushID(i);

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
                if (LoadVGMFile(entry.fullPath.c_str())) {
                    VGMSync::AutoAssignSlots(entry.fullPath.c_str());
                    VGMSync::NotifyFileOpened(entry.fullPath.c_str());
                    StartVGMPlayback();
                }
            }
        }

        if (isPlaying) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::EndChild();
}

static void RenderLogPanel(void) {
    // Debug Log
    if (ImGui::CollapsingHeader("AY8910 Debug Log##aylog", nullptr, 0)) {
        ImGui::Checkbox("Auto-scroll##aylog", &s_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##aylog")) {
            s_log.clear(); s_logDisplay[0] = '\0'; s_logLastSize = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All##aylog")) {
            ImGui::SetClipboardText(s_log.c_str());
        }
        size_t copyLen = s_log.size() < sizeof(s_logDisplay) - 1 ? s_log.size() : sizeof(s_logDisplay) - 1;
        memcpy(s_logDisplay, s_log.c_str(), copyLen);
        s_logDisplay[copyLen] = '\0';
        bool changed = (s_log.size() != s_logLastSize);
        s_logLastSize = s_log.size();
        if (s_logAutoScroll && changed) s_logScrollToBottom = true;

        float logH = 150;
        ImGui::BeginChild("AYDebugLogRegion", ImVec2(0, logH), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 tsz = ImGui::CalcTextSize(s_logDisplay, NULL, false, -1.0f);
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float minH = ImGui::GetContentRegionAvail().y;
        float inH = (tsz.y > minH) ? tsz.y + lineH * 2 : minH;
        ImGui::InputTextMultiline("##YmLogText", s_logDisplay, sizeof(s_logDisplay),
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
    bool historyOpen = ImGui::TreeNodeEx("AY8910 Folder History##ayhist",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_ymHistoryCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
    if (historyOpen) s_ymHistoryCollapsed = false;
    else s_ymHistoryCollapsed = true;

    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Clear##ayHistClear")) {
        s_folderHistory.clear(); SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All");
    ImGui::SameLine();
    if (s_histSortMode == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Time##ayhistSortTime")) s_histSortMode = 0;
    if (s_histSortMode == 0) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by time");
    ImGui::SameLine();
    if (s_histSortMode == 1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Freq##ayhistSortFreq")) s_histSortMode = 1;
    if (s_histSortMode == 1) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##ayHistFilter", "Filter...", s_histFilter, sizeof(s_histFilter));

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
        ImGui::BeginChild("AYHistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

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
    ImGui::BeginChild("AY_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoKeyboard();
    ImGui::EndChild();
    ImGui::BeginChild("AY_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
    RenderLevelMeters();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginChild("AY_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderRegisterTable();
    ImGui::EndChild();

    // Bottom section: Player + FileBrowser | Log+History
    ImGui::BeginChild("AY_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderPlayerBar();
    RenderFileBrowser();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("AY_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();

    // Scope (optional, very bottom)
    RenderScopeArea();
}

void Render() {
    ImGui::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_FirstUseEver);
    bool visible = ImGui::Begin("AY8910(PSG)");
    ImGui::BeginChild("AY_LeftPane", ImVec2(300, 0), true);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("AY_RightPane", ImVec2(0, 0), false);
    RenderMain();
    ImGui::EndChild();
    ImGui::End();

    // Popups
    RenderTestPopup();
}

bool WantsKeyboardCapture() { return false; }

} // namespace AY8910Window
