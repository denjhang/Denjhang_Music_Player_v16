// ym2413_window.cpp - YM2413 (OPLL) Hardware Control Window
// Controls real YM2413 chip via FTDI/SPFM Light interface
// VGM hardware playback + debug test functions + file browser

#include "ym2413_window.h"
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

namespace YM2413Window {

// ============ Constants ============
static const int YM_SAMPLE_RATE = 44100;
static const double YM2413_CLOCK = 3579545.0;  // NTSC
static const double YM2413_FM_CLOCK = YM2413_CLOCK / 72.0;  // ~49716 Hz

// 9 melodic + 5 rhythm channels = 14 total
static const int YM_NUM_MELODIC = 9;
static const int YM_NUM_RHYTHM = 5;
static const int YM_NUM_CHANNELS = YM_NUM_MELODIC + YM_NUM_RHYTHM;  // 14

// Channel names for display
static const char* kMelodicNames[9] = {
    "Ch0", "Ch1", "Ch2", "Ch3", "Ch4",
    "Ch5", "Ch6", "Ch7", "Ch8"
};
static const char* kRhythmNames[5] = { "BD", "SD", "TOM", "HH", "CYM" };

// 16 built-in instrument names (matching libvgm kOPLLPatch)
static const char* kInstNames[16] = {
    "Custom", "Bell", "Guitar", "Piano", "Flute",
    "Clarinet", "Oboe", "Trumpet", "Organ", "Horn",
    "Synth", "Harp", "Vibraphone", "SynBass", "AcBass", "ElecBass"
};

// Note names for register display
static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// OPLL built-in patch ROM: [patch][op][TL,FB,AR,DR,SL,RR,KL,ML,AM,VIB]
// patch 0 = custom (from regs), patches 1-15 from YM2413 datasheet ROM
static const uint8_t kOPLLRom[16][2][10] = {
    // Custom (placeholder, filled from regs 0x00-0x07)
    {{0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0}},
    // 1 Bell
    {{0,5,15,7, 4,0,0,13,0,0},{0,0,15,8, 5,6,0,1,0,0}},
    // 2 Guitar
    {{3,5,12,8, 4,4,0, 1,0,0},{0,0,15,8, 4,4,0,1,0,0}},
    // 3 Piano
    {{0,5,15,9, 6,7,0, 1,0,0},{0,0,15,8, 5,6,0,1,0,0}},
    // 4 Flute
    {{1,5,15,8, 4,0,0, 9,1,1},{0,0,13,6, 3,0,0,2,0,0}},
    // 5 Clarinet
    {{3,5, 9,8, 6,4,0, 1,0,0},{0,0,15,8, 4,4,0,1,0,0}},
    // 6 Oboe
    {{0,5,15,7, 5,6,0, 1,0,0},{0,0,14,8, 5,6,0,1,0,0}},
    // 7 Trumpet
    {{0,5,15,8, 5,6,0, 1,0,0},{0,0,15,8, 5,6,0,1,0,0}},
    // 8 Organ
    {{0,5,15,9, 5,0,0, 1,0,0},{0,0,15,8, 5,0,0,1,0,0}},
    // 9 Horn
    {{0,3,12,6, 7,7,0, 1,0,0},{0,0,15,8, 5,6,0,1,0,0}},
    // 10 Synth
    {{0,5,15,7, 5,6,0,13,0,0},{0,0,15,7, 4,7,1,1,0,0}},
    // 11 Harp
    {{0,0,15,9, 8,7,0, 9,0,0},{0,0,12,6, 6,6,0,1,0,0}},
    // 12 Vibraphone
    {{0,0,15,4, 4,0,0, 9,1,0},{0,0,15,8, 5,0,0,2,0,1}},
    // 13 SynBass
    {{0,0,12,6, 7,5,0, 9,0,0},{0,0,15,8, 4,6,0,1,0,0}},
    // 14 AcBass
    {{0,0,15,8, 8,6,0, 9,0,0},{0,0,15,9, 7,6,0,1,0,0}},
    // 15 ElecBass
    {{0,0,15,7, 6,5,0, 9,0,0},{0,0,15,8, 4,5,0,1,0,0}},
};

// 14 distinct channel colors (9 melodic + 5 rhythm)
static ImU32 kChColors[14] = {
    IM_COL32(160, 200, 160, 255), // Ch0: green
    IM_COL32(160, 160, 220, 255), // Ch1: blue
    IM_COL32(220, 160, 160, 255), // Ch2: red
    IM_COL32(200, 200, 140, 255), // Ch3: yellow-green
    IM_COL32(180, 140, 220, 255), // Ch4: purple
    IM_COL32(140, 200, 200, 255), // Ch5: cyan
    IM_COL32(220, 180, 140, 255), // Ch6: orange
    IM_COL32(200, 160, 200, 255), // Ch7: pink
    IM_COL32(160, 220, 200, 255), // Ch8: teal
    // Rhythm channels:
    IM_COL32(220, 220, 100, 255), // BD: bright yellow
    IM_COL32(100, 180, 220, 255), // SD: sky blue
    IM_COL32(180, 220, 100, 255), // TOM: lime
    IM_COL32(220, 140, 100, 255), // HH: salmon
    IM_COL32(140, 140, 220, 255), // CYM: indigo
};

// Custom channel colors (0 = use default from kChColors)
static ImU32 kChColorsCustom[14] = {};

// Rhythm channel VGM register mapping
// BD=0x10(bd), SD=0x10(sd), TOM=0x10(tom), HH=0x10(hh), CYM=0x10(cym)
// The rhythm bits share register 0x0E (rhythm enable) and freq/vol use ch6-ch10 of 0x10-0x18/0x30-0x38
static const int kRhythmInstReg[5] = { 6, 7, 8, 8, 8 }; // which melodic reg maps (ch index for freq)
static const int kRhythmFreqReg[5] = { 6, 7, 8, 8, 8 }; // block/fnum register index

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
static bool s_chMuted[YM_NUM_CHANNELS] = {};
static int s_soloCh = -1;

// GD3 tags
static std::string s_trackName, s_gameName, s_systemName, s_artistName;
static UINT32 s_vgmVersion = 0;

// ============ YM2413 Register Shadow ============
static uint8_t s_regShadow[0x40] = {};

// Individual channel state for UI display
static uint8_t s_freqLo[9] = {};       // Register 0x10-0x18
static uint8_t s_blockFnHi[9] = {};    // Register 0x20-0x28
static uint8_t s_instVol[9] = {};      // Register 0x30-0x38
static bool   s_rhythmMode = false;    // Register 0x0E bit 5

// Rhythm channel on/off state (extracted from shadow)
static bool s_rhythmOn[5] = {};

// ============ Piano State ============
static const int YM_PIANO_LOW = 24;   // C1
static const int YM_PIANO_HIGH = 107; // B7
static const int YM_PIANO_KEYS = YM_PIANO_HIGH - YM_PIANO_LOW + 1;
static bool s_pianoKeyOn[YM_PIANO_KEYS] = {};
static float s_pianoKeyLevel[YM_PIANO_KEYS] = {};
static int s_pianoKeyChannel[YM_PIANO_KEYS] = {};
static float s_pianoVibrato[YM_PIANO_KEYS] = {}; // VIB pitch offset in semitones
static float s_pianoTremolo[YM_PIANO_KEYS] = {}; // AM tremolo [-1, 1]
static float s_pianoPortamento[YM_PIANO_KEYS] = {}; // portamento offset in semitones
static float s_visualNote[9] = {}; // smoothed visual note per channel
static float s_startNote[9] = {}; // note at key-on (portamento anchor)
static bool  s_chKeyOnEdge[9] = {}; // key-on rising edge flag
static const bool s_isBlackNote[12] = {false, true, false, true, false, false, true, false, true, false, true, false};

// ============ Level Meter State ============
static float s_channelLevel[YM_NUM_CHANNELS] = {};

// Key-on rising edge detection and decay system (melodic channels 0-8)
static uint8_t s_prevKeyOn[9] = {};       // previous key-on state per channel
static float   s_chDecay[9] = {};         // per-channel decay envelope
static bool    s_chKeyOff[9] = {};        // key-off state per channel

// Rhythm key-on edge detection
static uint8_t s_rhyPrevKon = 0;          // previous register 0x0E rhythm bits
static float   s_rhyDecay[5] = {};        // per-rhythm decay envelope

// ============ Scope State ============
static ModizerViz s_scope;
static bool s_showScope = false;
static float s_scopeHeight = 80.0f;
static int s_voiceCh[YM_NUM_CHANNELS] = {};
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
static void ym2413_mute_all(void);
static void ym2413_write_reg(uint8_t reg, uint8_t data);
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
static void ApplyShadowState(void);

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

void ym2413_write_reg(uint8_t reg, uint8_t data) {
    int slot = VGMSync::FindChipSlot(VGMSync::CHIP_YM2413);
    if (slot < 0) return;
    ::spfm_write_reg(slot, 0, reg, data);
    if (s_flushMode == 1) {
        ::spfm_hw_wait(1);
    }
}

void safe_flush(void) {
    ::spfm_flush();
}

// Update YM2413 shadow state and UI state for a register write
// Returns the (possibly modified) data byte after fadeout/mute interception
UINT8 UpdateYM2413State(UINT8 reg, UINT8 data) {
    if (reg < 0x40) s_regShadow[reg] = data;

    if (reg >= 0x10 && reg <= 0x18) {
        s_freqLo[reg - 0x10] = data;
    } else if (reg >= 0x20 && reg <= 0x28) {
        int ch = reg - 0x20;
        bool prevKon = (s_prevKeyOn[ch] != 0);
        bool newKon  = (data >> 4) & 1;
        if (newKon && !prevKon) {
            s_chDecay[ch] = 1.0f;
            s_chKeyOff[ch] = false;
            s_chKeyOnEdge[ch] = true;
        } else if (!newKon && prevKon) {
            s_chKeyOff[ch] = true;
        }
        s_prevKeyOn[ch] = newKon ? 1 : 0;
        s_blockFnHi[ch] = data;
    } else if (reg >= 0x30 && reg <= 0x38) {
        s_instVol[reg - 0x30] = data;
    } else if (reg == 0x0E) {
        s_rhythmMode = (data & 0x20) != 0;
        s_rhythmOn[0] = s_rhythmMode && (data & 0x10);
        s_rhythmOn[1] = s_rhythmMode && (data & 0x08);
        s_rhythmOn[2] = s_rhythmMode && (data & 0x04);
        s_rhythmOn[3] = s_rhythmMode && (data & 0x01);
        s_rhythmOn[4] = s_rhythmMode && (data & 0x02);
        static const int kRhBit[5] = {4, 3, 2, 0, 1};
        for (int i = 0; i < 5; i++) {
            bool prev = (s_rhyPrevKon >> kRhBit[i]) & 1;
            bool cur  = (data >> kRhBit[i]) & 1;
            if (cur && !prev) s_rhyDecay[i] = 1.0f;
        }
        s_rhyPrevKon = data & 0x1F;
    }

    // Fadeout: intercept 0x30-0x38 volume writes
    if (reg >= 0x30 && reg <= 0x38 && s_fadeoutActive && s_fadeoutLevel < 1.0f) {
        int ch = reg - 0x30;
        uint8_t origVol = data & 0x0F;
        uint8_t fadedVol = (uint8_t)(origVol + (15 - origVol) * (1.0f - s_fadeoutLevel));
        data = (data & 0xF0) | (fadedVol & 0x0F);
        s_regShadow[reg] = data;
        s_instVol[ch] = data;
    }

    // Channel mute: intercept 0x30-0x38
    if (reg >= 0x30 && reg <= 0x38) {
        int ch = reg - 0x30;
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

static void ym2413_mute_all(void) {
    // Key off all melodic channels first
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x20 + i, 0x00);
        s_regShadow[0x20 + i] = 0x00;
    }
    // Rhythm off
    ym2413_write_reg(0x0E, 0x00);
    s_regShadow[0x0E] = 0x00;
    // TL=max attenuation for melodic channels (low nibble = TL)
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x30 + i, 0x0F);
        s_regShadow[0x30 + i] = 0x0F;
    }
    // Rhythm channel silence (per MDPlayer softResetYM2413)
    ym2413_write_reg(0x36, 0x0F);
    ym2413_write_reg(0x37, 0xFF);
    ym2413_write_reg(0x38, 0xFF);
    safe_flush();
}

// Mute hardware without touching shadow state (used before ApplyShadowState)
static void MuteHardwareOnly(void) {
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x20 + i, 0x00);
    }
    ym2413_write_reg(0x0E, 0x00);
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x30 + i, 0x0F);
    }
    ym2413_write_reg(0x36, 0x0F);
    ym2413_write_reg(0x37, 0xFF);
    ym2413_write_reg(0x38, 0xFF);
    safe_flush();
}

// Write all shadow state registers to hardware (used after seek)
static void ApplyShadowState(void) {
    MuteHardwareOnly();

    // User custom instruments (0x00-0x07)
    for (int i = 0; i < 8; i++) {
        ym2413_write_reg(i, s_regShadow[i]);
    }
    // Rhythm control
    ym2413_write_reg(0x0E, s_regShadow[0x0E]);
    // Frequency low (0x10-0x18)
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x10 + i, s_regShadow[0x10 + i]);
    }
    // Instrument + Volume (0x30-0x38)
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x30 + i, s_regShadow[0x30 + i]);
    }
    // Block + F-number high + Key on (0x20-0x28) - last, to trigger keys
    for (int i = 0; i < 9; i++) {
        ym2413_write_reg(0x20 + i, s_regShadow[0x20 + i]);
    }
    safe_flush();
}

void MuteAll() {
    if (!SPFMManager::IsConnected()) return;
    ym2413_mute_all();
}

static void InitHardware(void) {
    ym2413_mute_all();
    Sleep(50);
}

static void ResetState(void) {
    s_testRunning = false; s_testType = 0; s_testStep = 0; s_testStepMs = 0.0;
    memset(s_regShadow, 0, sizeof(s_regShadow));
    memset(s_freqLo, 0, sizeof(s_freqLo));
    memset(s_blockFnHi, 0, sizeof(s_blockFnHi));
    memset(s_instVol, 0, sizeof(s_instVol));
    s_rhythmMode = false;
    memset(s_rhythmOn, 0, sizeof(s_rhythmOn));
}

// ============ F-Number to MIDI Note Conversion ============
static int fn_to_midi_note(int ch) {
    // YM2413 register 0x20-0x28 format:
    // bit 7-5: unused, bit 4: KEY ON, bit 3-1: BLOCK, bit 0: F-number bit 8
    int block = (s_blockFnHi[ch] >> 1) & 0x07;
    int fn_hi = s_blockFnHi[ch] & 0x01;
    int fullFn = (fn_hi << 8) | s_freqLo[ch];
    if (fullFn == 0 && block == 0) return -1;

    double freq = (double)fullFn * YM2413_FM_CLOCK / (double)(1 << (19 - block));
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
        DcLog("[YM2413] Hardware connected via SPFMManager\n");
    }
    if (!s_connected && wasConnected) {
        DcLog("[YM2413] Hardware disconnected\n");
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
    snprintf(s_configPath, MAX_PATH, "%s\\ym2413_config.ini", exeDir);

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
    for (int i = 0; i < 14; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ChColor%d", i);
        char val[32] = "";
        GetPrivateProfileStringA("Colors", key, "", val, sizeof(val), s_configPath);
        if (val[0]) {
            unsigned int c = (unsigned int)strtoul(val, NULL, 16);
            if (c > 0) kChColorsCustom[i] = (ImU32)c;
        }
    }

    DcLog("[YM2413] Config loaded\n");
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
    for (int i = 0; i < 14; i++) {
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
    for (int i = 0; i < YM_PIANO_KEYS; i++) {
        s_pianoKeyOn[i] = false;
        s_pianoKeyLevel[i] = 0.0f;
        s_pianoKeyChannel[i] = -1;
        s_pianoVibrato[i] = 0.0f;
        s_pianoTremolo[i] = 0.0f;
        s_pianoPortamento[i] = 0.0f;
    }

    // YM2413 LFO: fixed ~3.98Hz, used for AM (tremolo) and VIB (vibrato)
    const float kOPLLLfoHz = 3.98f;
    float lfo_time = (float)ImGui::GetTime();
    float lfo_sin = sinf(lfo_time * 2.0f * 3.14159265f * kOPLLLfoHz);
    float lfo_tri = 2.0f * fabsf(2.0f * fmodf(lfo_time * kOPLLLfoHz, 1.0f) - 1.0f) - 1.0f;

    // Melodic channels (0-8)
    for (int ch = 0; ch < YM_NUM_MELODIC; ch++) {
        uint8_t vol = s_instVol[ch] & 0x0F;
        bool keyoff = s_chKeyOff[ch];
        bool sus = (s_blockFnHi[ch] >> 5) & 1;

        if (keyoff && !sus) {
            s_chDecay[ch] = 0.0f;
        } else if (keyoff) {
            s_chDecay[ch] *= 0.85f;
            if (s_chDecay[ch] < 0.01f) s_chDecay[ch] = 0.0f;
        } else {
            s_chDecay[ch] *= 0.98f;
            if (s_chDecay[ch] < 0.01f) s_chDecay[ch] = 0.0f;
        }

        float lv = (vol >= 15) ? 0.0f : (1.0f - (float)vol / 15.0f) * s_chDecay[ch];
        s_channelLevel[ch] += (lv - s_channelLevel[ch]) * 0.3f;
        if (s_channelLevel[ch] < 0.001f) s_channelLevel[ch] = 0.0f;

        if (s_chMuted[ch]) continue;

        bool kon = s_chDecay[ch] > 0.01f;
        if (kon && s_channelLevel[ch] > 0.01f) {
            int midi = fn_to_midi_note(ch);
            if (midi >= YM_PIANO_LOW && midi <= YM_PIANO_HIGH) {
                int idx = midi - YM_PIANO_LOW;
                if (!s_pianoKeyOn[idx] || s_channelLevel[ch] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = s_channelLevel[ch];
                    s_pianoKeyChannel[idx] = ch;
                }
                // Read patch AM/VIB from ROM or custom regs
                int patch = (s_instVol[ch] >> 4) & 0x0F;
                bool has_vib = false, has_am = false;
                if (patch == 0) {
                    // Custom: read from regs 0x00-0x07
                    // mod1: reg0[7]=AM, reg0[5]=VIB; mod2: reg1[7]=AM, reg1[5]=VIB
                    has_vib = ((s_regShadow[0x00] >> 5) & 1) || ((s_regShadow[0x01] >> 5) & 1);
                    has_am = ((s_regShadow[0x00] >> 7) & 1) || ((s_regShadow[0x01] >> 7) & 1);
                } else {
                    has_vib = kOPLLRom[patch][0][9] || kOPLLRom[patch][1][9];
                    has_am  = kOPLLRom[patch][0][8] || kOPLLRom[patch][1][8];
                }
                // VIB: small pitch wobble (YM2413 depth is ~14 cent, visual ±0.14 semitones)
                float vib = has_vib ? lfo_sin * 0.14f : 0.0f;
                // AM: small tremolo (YM2413 depth is ~4.8dB, visual scaled down)
                float trem = has_am ? lfo_tri * 0.3f : 0.0f;
                if (fabsf(vib) > fabsf(s_pianoVibrato[idx]))
                    s_pianoVibrato[idx] = vib;
                if (fabsf(trem) > fabsf(s_pianoTremolo[idx]))
                    s_pianoTremolo[idx] = trem;
                // Portamento: track frequency changes within a single key-on
                float fnt = (float)midi;
                // key-on rising edge: snap start/visual to current note (no false slide)
                if (s_chKeyOnEdge[ch]) {
                    s_startNote[ch] = fnt;
                    s_visualNote[ch] = fnt;
                    s_chKeyOnEdge[ch] = false;
                } else if (s_visualNote[ch] > 0.0f && fabsf(fnt - s_visualNote[ch]) > 1.0f) {
                    // Frequency jumped >1 semitone: not portamento, reset anchor
                    s_startNote[ch] = fnt;
                    s_visualNote[ch] = fnt;
                } else if (s_visualNote[ch] > 0.0f) {
                    // Smooth track within same key-on, poff reveals slide
                    s_visualNote[ch] += (fnt - s_visualNote[ch]) * 0.5f;
                } else {
                    s_startNote[ch] = fnt;
                    s_visualNote[ch] = fnt;
                }
                float poff = s_visualNote[ch] - s_startNote[ch];
                if (poff > 1.0f) poff = 1.0f;
                if (poff < -1.0f) poff = -1.0f;
                s_pianoPortamento[idx] = poff;
            }
        }
        // Clear portamento state when channel off
        if (!kon) {
            s_visualNote[ch] = 0.0f;
            s_startNote[ch] = 0.0f;
        }
    }

    // Rhythm channels (9-13): BD, SD, TOM, HH, CYM
    // Register 0x0E: bit 4=BD, bit 3=SD, bit 2=TOM, bit 1=CYM, bit 0=HH
    // Rhythm volumes from registers 0x36-0x38:
    //   BD vol = (reg_0x36 >> 4) & 0x0F
    //   HH vol = (reg_0x37 >> 4) & 0x0F
    // Rhythm volumes from registers 0x36-0x38:
    //   BD vol = (reg_0x36 >> 4) & 0x0F
    //   HH vol = (reg_0x37 >> 4) & 0x0F
    //   SD vol = reg_0x37 & 0x0F
    //   TOM vol = (reg_0x38 >> 4) & 0x0F
    //   CYM vol = reg_0x38 & 0x0F
    // Display order: BD(0), SD(1), TOM(2), HH(3), CYM(4)
    // Volume sources per MDPlayer: BD=0x36&0x0F, SD=0x37&0x0F, TOM=0x38>>4, HH=0x37>>4, CYM=0x38&0x0F
    static const int kRhythmVolReg[5] = { 0x36, 0x37, 0x38, 0x37, 0x38 };
    static const int kRhythmVolShift[5] = { 0, 0, 4, 4, 0 }; // low or high nibble

    for (int r = 0; r < YM_NUM_RHYTHM; r++) {
        int chIdx = YM_NUM_MELODIC + r;

        // Rhythm decay (edge detection done in VGMProcessCommand)
        s_rhyDecay[r] *= 0.80f;
        if (s_rhyDecay[r] < 0.01f) s_rhyDecay[r] = 0.0f;

        // Get rhythm volume
        uint8_t rhyVol = (s_regShadow[kRhythmVolReg[r]] >> kRhythmVolShift[r]) & 0x0F;
        float baseLevel = (rhyVol >= 15) ? 0.0f : (1.0f - (float)rhyVol / 15.0f);
        float target = baseLevel * s_rhyDecay[r];
        s_channelLevel[chIdx] += (target - s_channelLevel[chIdx]) * 0.3f;
        if (s_channelLevel[chIdx] < 0.001f) s_channelLevel[chIdx] = 0.0f;
    }

    // Rhythm channels don't show on piano keyboard (no pitch)
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

    // YM2413 clock at offset 0x10
    vgmfseek(f, 0x10, SEEK_SET);
    UINT32 ym2413Clock = ReadLE32(f);

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

    DcLog("[VGM] hdr: ver=0x%X dataAbs=0x%X loopAbs=0x%X gd3Abs=0x%X YM2413Clk=%u\n",
        s_vgmVersion, s_vgmDataOffset, s_vgmLoopOffset, gd3Off, ym2413Clock);

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
    DcLog("[VGM] Clock=%u Total=%.1fs\n", ym2413Clock, (double)s_vgmTotalSamples / 44100.0);
    if (!s_trackName.empty()) DcLog("[VGM] Track: %s\n", s_trackName.c_str());
    if (!s_gameName.empty()) DcLog("[VGM] Game: %s\n", s_gameName.c_str());
    return true;
}

static int s_vgmCmdCount = 0;

// VGM Command Length Table for YM2413 player
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
    // 0x50-0x5F: 50=SN76489(2), 51=YM2413(3), 52-5F=YM chips(3)
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

// Table-driven VGM command processor for YM2413
// Returns: wait samples (>0), 0 = no wait, -1 = EOF/error
static int VGMProcessCommand(void) {
    UINT8 cmd;
    if (vgmfread(&cmd, 1, 1, s_vgmFile) != 1) return -1;

    if (s_vgmCmdCount < 50) {
        DcLog("[VGM] cmd=0x%02X at %ld\n", cmd, ftell(s_vgmFile) - 1);
    }

    // Special commands with unique behavior
    switch (cmd) {
        case 0x51: { // YM2413 register write: [0x51, register, data]  (3 bytes)
            UINT8 reg, data;
            if (vgmfread(&reg, 1, 1, s_vgmFile) != 1) return -1;
            if (vgmfread(&data, 1, 1, s_vgmFile) != 1) return -1;
            s_vgmCmdCount++;

            data = UpdateYM2413State(reg, data);

            // Hardware write
            if (s_connected) {
                ym2413_write_reg(reg, data);
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

// Optimized VGMPlay: lookahead batch — reads consecutive 0x51 writes,
// updates shadow/UI state, sends in one batch via spfm_write_regs.
// Returns number of register writes processed.
// *outWait: wait samples (>0), -1 = EOF, -2 = need fallback to VGMProcessCommand.
static int VGMProcessBatch(int* outWait) {
    *outWait = 0;
    const int MAX_BATCH = 64;
    spfm_reg_t batch[MAX_BATCH];
    int count = 0;

    while (count < MAX_BATCH && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
        UINT8 cmd;
        if (vgmfread(&cmd, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }

        if (cmd == 0x51) {
            UINT8 reg, data;
            if (vgmfread(&reg, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            if (vgmfread(&data, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            s_vgmCmdCount++;
            data = UpdateYM2413State(reg, data);
            batch[count].port = 0;
            batch[count].addr = reg;
            batch[count].data = data;
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

    // Batch write to hardware
    if (count > 0 && s_connected) {
        ::spfm_write_regs(0, batch, (uint32_t)count, 0);
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
            for (int ch = 0; ch < 9; ch++) {
                uint8_t origVol = s_instVol[ch] & 0x0F;
                uint8_t fadedVol = (uint8_t)(origVol + (15 - origVol) * (1.0f - s_fadeoutLevel));
                uint8_t regData = (s_instVol[ch] & 0xF0) | (fadedVol & 0x0F);
                ym2413_write_reg(0x30 + ch, regData);
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

// Helper: convert MIDI note to YM2413 block/fnumber
static void midi_to_fn(int midiNote, int& block, int& fnumber) {
    double freq = 440.0 * pow(2.0, (midiNote - 69.0) / 12.0);
    // fnumber = freq * 2^(20-block) / FM_clock
    // Try block 0-7, find best fit
    for (int b = 0; b <= 7; b++) {
        double fn = freq * (double)(1 << (20 - b)) / YM2413_FM_CLOCK;
        if (fn >= 0.0 && fn < 1024.0) {
            block = b;
            fnumber = (int)round(fn);
            return;
        }
    }
    block = 0;
    fnumber = 0;
}

// Key off all melodic channels
static void key_off_all(void) {
    for (int ch = 0; ch < 9; ch++) {
        ym2413_write_reg(0x20 + ch, s_blockFnHi[ch] & ~0x10); // clear bit 4 (KEY ON)
        safe_flush();
    }
}

static void TestStep(void) {
    switch (s_testType) {
        case 0: {
            // Instrument demo: play C4 (MIDI 60) with each of 16 instruments
            if (s_testStep >= 16) { s_testRunning = false; key_off_all(); return; }
            int inst = s_testStep;
            int block, fn;
            midi_to_fn(60, block, fn);  // C4

            // Key off all channels first
            key_off_all();

            // Set instrument and volume on channel 0
            uint8_t volData = (inst << 4) | 0x00; // inst in high nibble, volume=0 (max)
            ym2413_write_reg(0x30, volData);
            // Set frequency low
            ym2413_write_reg(0x10, fn & 0xFF);
            // Key-off first: bit4=0, block in bits 3-1, fnhi in bit 0
            ym2413_write_reg(0x20, (block << 1) | ((fn >> 8) & 1));
            ::spfm_hw_wait(1);
            // Key-on: bit4=1
            uint8_t blockData = 0x10 | (block << 1) | ((fn >> 8) & 1);
            ym2413_write_reg(0x20, blockData);
            safe_flush();

            // Update shadow state
            s_instVol[0] = volData;
            s_freqLo[0] = fn & 0xFF;
            s_blockFnHi[0] = blockData;
            s_regShadow[0x30] = volData;
            s_regShadow[0x10] = fn & 0xFF;
            s_regShadow[0x20] = blockData;
            break;
        }
        case 1: {
            // Scale: C major scale on channel 0 with Piano (inst 3)
            static const uint8_t notes[] = {60, 62, 64, 65, 67, 69, 71, 72};
            if (s_testStep >= 8) { s_testRunning = false; key_off_all(); return; }

            int block, fn;
            midi_to_fn(notes[s_testStep], block, fn);

            // Key off all first
            key_off_all();

            // Set instrument=Piano(3), volume=0 on ch0
            uint8_t volData = (3 << 4) | 0x00;
            ym2413_write_reg(0x30, volData);
            // Set frequency low
            ym2413_write_reg(0x10, fn & 0xFF);
            // Key-off first
            ym2413_write_reg(0x20, (block << 1) | ((fn >> 8) & 1));
            ::spfm_hw_wait(1);
            // Key-on
            uint8_t blockData = 0x10 | (block << 1) | ((fn >> 8) & 1);
            ym2413_write_reg(0x20, blockData);
            safe_flush();

            s_instVol[0] = volData;
            s_freqLo[0] = fn & 0xFF;
            s_blockFnHi[0] = blockData;
            s_regShadow[0x30] = volData;
            s_regShadow[0x10] = fn & 0xFF;
            s_regShadow[0x20] = blockData;
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
    VGMSync::Init();
    QueryPerformanceFrequency(&s_perfFreq);
    s_scope.Init();
    LoadConfig();

    // Register unified playback callbacks
    VGMSync::RegisterChipWriter(VGMSync::CHIP_YM2413,
        (VGMSync::ChipStateUpdateFn)UpdateYM2413State,
        (VGMSync::ChipHwWriteFn)ym2413_write_reg,
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
            LoadVGMFile(shared);
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
        ScopeChipSlot *slot = scope_find_slot("YM2413", 0);
        if (slot) {
            for (int i = 0; i < YM_NUM_CHANNELS; i++) s_voiceCh[i] = slot->slot_base + i;
        } else {
            for (int i = 0; i < YM_NUM_CHANNELS; i++) s_voiceCh[i] = -1;
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
    if (ch >= 0 && ch < YM_NUM_CHANNELS) {
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
    ImGui::BeginChild("YM_Piano", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float whiteKeyH = 100.0f;
    float blackKeyH = 60.0f;

    const int kMinNote = YM_PIANO_LOW;
    const int kMaxNote = YM_PIANO_HIGH;

    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) if (!s_isBlackNote[n % 12]) numWhiteKeys++;

    float whiteKeyW = availW / (float)numWhiteKeys;
    if (whiteKeyW < 6.0f) whiteKeyW = 6.0f;
    float blackKeyW = whiteKeyW * 0.65f;

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (s_isBlackNote[n % 12]) continue;
        int idx = n - YM_PIANO_LOW;
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
        int idx = n - YM_PIANO_LOW;
        float x = p.x + (wkIdx - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;
        ImU32 fillCol = (idx >= 0 && idx < YM_PIANO_KEYS && s_pianoKeyOn[idx])
            ? getKeyColorBlack(idx, s_pianoKeyLevel[idx]) : IM_COL32(0, 0, 0, 255);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), fillCol);
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), IM_COL32(128, 128, 128, 255));
    }

    // Pass 3: portamento / vibrato / tremolo indicators
    // Build key center-x table for smooth interpolation (like libvgm)
    float keyCenterX[128] = {};
    {
        int wi = 0;
        for (int n = kMinNote; n <= kMaxNote; n++) {
            if (!s_isBlackNote[n % 12]) {
                keyCenterX[n] = p.x + wi * whiteKeyW + whiteKeyW * 0.5f;
                wi++;
            } else {
                float bkLeft = p.x + (wi - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;
                keyCenterX[n] = bkLeft + blackKeyW * 0.5f;
            }
        }
    }
    auto noteToX = [&](float fnote) -> float {
        int n0 = (int)floorf(fnote);
        int n1 = n0 + 1;
        float frac = fnote - (float)n0;
        float x0 = (n0 >= kMinNote && n0 <= kMaxNote) ? keyCenterX[n0] : keyCenterX[kMinNote];
        float x1 = (n1 >= kMinNote && n1 <= kMaxNote) ? keyCenterX[n1] : keyCenterX[kMaxNote];
        return x0 + (x1 - x0) * frac;
    };

    float hl_w = whiteKeyW * 0.3f;
    wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        int idx = n - YM_PIANO_LOW;
        float poff = s_pianoPortamento[idx];
        float vib = s_pianoVibrato[idx];
        float trem = s_pianoTremolo[idx];
        bool hasPoff = fabsf(poff) >= 0.02f;
        bool hasVib = fabsf(vib) >= 0.02f;
        bool hasTrem = fabsf(trem) >= 0.02f;
        if (!hasPoff && !hasVib && !hasTrem) {
            if (!s_isBlackNote[n % 12]) wkIdx++;
            continue;
        }

        int ch = s_pianoKeyChannel[idx];
        ImU32 rawCol = (ch >= 0) ? getChColor(ch) : IM_COL32(160, 200, 160, 255);
        // Darken channel color significantly for indicator visibility
        int cr = (rawCol >> 0) & 0xFF;
        int cg = (rawCol >> 8) & 0xFF;
        int cb = (rawCol >> 16) & 0xFF;
        int dr = (int)(cr * 0.55f);
        int dg = (int)(cg * 0.55f);
        int db = (int)(cb * 0.55f);
        ImVec4 cv = ImGui::ColorConvertU32ToFloat4(IM_COL32(dr, dg, db, 255));
        cv.w = fmaxf(0.6f, s_pianoKeyLevel[idx]);
        ImU32 col = ImGui::ColorConvertFloat4ToU32(cv);

        bool isBlack = s_isBlackNote[n % 12];
        float keyX, kh;
        if (isBlack) {
            // Match black key drawing center exactly
            float bkLeft = p.x + (wkIdx - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;
            keyX = bkLeft + blackKeyW * 0.5f;
            kh = blackKeyH;
            hl_w = blackKeyW * 0.5f;
        } else {
            keyX = p.x + wkIdx * whiteKeyW + whiteKeyW * 0.5f;
            kh = whiteKeyH;
        }

        // Portamento: full-height bar at current pitch position (smooth interpolation)
        if (hasPoff) {
            float fnote = (float)n + poff;
            float hlX = noteToX(fnote);
            dl->AddRectFilled(
                ImVec2(hlX - hl_w * 0.5f, p.y),
                ImVec2(hlX + hl_w * 0.5f, p.y + kh), col);
        }
        // VIB: full-height bar shifting left/right from note center (smooth interpolation)
        if (hasVib) {
            float hlX = noteToX((float)n + vib);
            dl->AddRectFilled(
                ImVec2(hlX - hl_w * 0.5f, p.y),
                ImVec2(hlX + hl_w * 0.5f, p.y + kh), col);
        }
        // AM/Tremolo: vertical pulse at top of key
        if (hasTrem) {
            float pulseH = kh * 0.15f * fabsf(trem);
            dl->AddRectFilled(
                ImVec2(keyX - hl_w * 0.5f, p.y),
                ImVec2(keyX + hl_w * 0.5f, p.y + pulseH + 2.0f), col);
        }

        if (!isBlack) wkIdx++;
    }

    ImGui::EndChild();
}

// ============ Level Meters ============
static void ApplyChannelMute(int i) {
    if (i < YM_NUM_MELODIC) {
        // Melodic channel mute: set volume to 0xF (max attenuation)
        uint8_t regData;
        if (s_chMuted[i]) {
            regData = (s_instVol[i] & 0xF0) | 0x0F;
        } else {
            regData = s_instVol[i];
        }
        if (s_connected) {
            ym2413_write_reg(0x30 + i, regData);
            safe_flush();
        }
    }
    // Rhythm channels (9-13) can't be individually muted easily via register,
    // but we can disable them by clearing their bits in 0x0E
}

static void RenderLevelMeters(void) {
    ImGui::BeginChild("YM_LevelMeters", ImVec2(0, 0), true);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    float groupW = availW / (float)YM_NUM_CHANNELS;
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
        for (int j = 0; j < YM_NUM_CHANNELS; j++) s_chMuted[j] = !s_chMuted[j];
        s_soloCh = -1;
        for (int j = 0; j < YM_NUM_CHANNELS; j++) ApplyChannelMute(j);
    }

    for (int i = 0; i < YM_NUM_CHANNELS; i++) {
        float centerX = p.x + i * groupW + groupW * 0.5f;
        float mY = p.y + labelH;
        float meterLeft = centerX - meterW * 0.5f;
        float meterRight = centerX + meterW * 0.5f;
        float meterBottom = mY + meterH;

        // Channel label
        const char* label = (i < YM_NUM_MELODIC) ? kMelodicNames[i] : kRhythmNames[i - YM_NUM_MELODIC];
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
        snprintf(btnId, sizeof(btnId), "##ymch%d", i);
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
                for (int j = 0; j < YM_NUM_CHANNELS; j++) { s_chMuted[j] = false; ApplyChannelMute(j); }
            } else {
                s_soloCh = i;
                for (int j = 0; j < YM_NUM_CHANNELS; j++) {
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
        if (i < YM_NUM_MELODIC) {
            uint8_t vol = s_instVol[i] & 0x0F;
            snprintf(volStr, sizeof(volStr), "%d", vol);
        } else {
            // Rhythm: show volume from correct registers
            // BD: reg 0x36 low nibble, SD: reg 0x37 low nibble, TOM: reg 0x38 high nibble,
            // HH: reg 0x37 high nibble, CYM: reg 0x38 low nibble
            static const int kRVolReg[5] = { 0x36, 0x37, 0x38, 0x37, 0x38 };
            static const int kRVolShift[5] = { 0, 0, 4, 4, 0 };
            int ri = i - YM_NUM_MELODIC;
            uint8_t rhyVol = (s_regShadow[kRVolReg[ri]] >> kRVolShift[ri]) & 0x0F;
            if (s_rhyDecay[ri] > 0.01f) {
                snprintf(volStr, sizeof(volStr), "%d", rhyVol);
            } else {
                snprintf(volStr, sizeof(volStr), "--");
            }
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
    ImGui::BeginChild("YM_Scope", ImVec2(0, s_scopeHeight), true);

    bool hasScope = false;
    for (int i = 0; i < YM_NUM_CHANNELS; i++) {
        if (s_voiceCh[i] >= 0) { hasScope = true; break; }
    }

    if (hasScope) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;
        float chW = availW / (float)YM_NUM_CHANNELS - 4.0f;

        for (int i = 0; i < YM_NUM_CHANNELS; i++) {
            if (s_voiceCh[i] < 0) continue;
            float x = p.x + i * (chW + 4.0f) + 2.0f;
            bool keyOn = false;
            float level = 0.0f;
            if (i < YM_NUM_MELODIC) {
                keyOn = s_chDecay[i] > 0.01f;
                level = s_channelLevel[i];
            } else {
                keyOn = s_rhyDecay[i - YM_NUM_MELODIC] > 0.01f;
                level = s_channelLevel[i];
            }

            s_scope.DrawChannel(s_voiceCh[i], dl, x, p.y + 16, chW, availH - 16,
                s_scopeAmplitude, kChColors[i], keyOn, level,
                s_scopeSamples, 0, 735, true, true, 1, false);
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Scope requires libvgm YM2413 core integration)");
    }

    ImGui::EndChild();
}

// ============ Test & Channel Controls Window ============
static void RenderTestPopup(void) {
    if (!s_showTestPopup) return;
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("YM2413 Debug##ymtestwin", &s_showTestPopup)) { ImGui::End(); return; }

    // Hardware Tests
    if (ImGui::CollapsingHeader("Hardware Tests##ymtest", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Instrument Demo (16 inst)##ymtest0", ImVec2(-1, 0))) StartTest(0);
        if (ImGui::Button("C Major Scale##ymtest1", ImVec2(-1, 0))) StartTest(1);
        if (s_testRunning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Stop Test##ymteststop", ImVec2(-1, 0))) StopTest();
            ImGui::PopStyleColor();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running...");
            if (s_testType == 0 && s_testStep < 16) {
                ImGui::Text("Instrument %d/16: %s", s_testStep, kInstNames[s_testStep]);
            }
        }
    }

    // Quick note test
    if (ImGui::CollapsingHeader("Quick Note##ymqnote", nullptr, 0)) {
        static int qNote = 60;
        static int qInst = 3; // Piano
        static int qVol = 0;
        ImGui::SliderInt("Note##ymqn", &qNote, 24, 96);
        ImGui::SliderInt("Instrument##ymqi", &qInst, 0, 15);
        ImGui::SameLine(); ImGui::Text("%s", kInstNames[qInst]);
        ImGui::SliderInt("Volume##ymqv", &qVol, 0, 15);
        if (ImGui::Button("Play Note##ymqplay", ImVec2(-1, 0))) {
            if (s_connected && !s_vgmPlaying) {
                int block, fn;
                midi_to_fn(qNote, block, fn);
                key_off_all();
                uint8_t volData = (qInst << 4) | (qVol & 0x0F);
                ym2413_write_reg(0x30, volData);
                ym2413_write_reg(0x10, fn & 0xFF);
                // Key-off first
                ym2413_write_reg(0x20, (block << 1) | ((fn >> 8) & 1));
                ::spfm_hw_wait(1);
                // Key-on: bit4=1
                uint8_t blockData = 0x10 | (block << 1) | ((fn >> 8) & 1);
                ym2413_write_reg(0x20, blockData);
                safe_flush();
                s_instVol[0] = volData;
                s_freqLo[0] = fn & 0xFF;
                s_blockFnHi[0] = blockData;
            }
        }
        if (ImGui::Button("Key Off##ymqoff", ImVec2(-1, 0))) {
            if (s_connected) { key_off_all(); safe_flush(); }
        }
    }

    ImGui::End();
}

// ============ UI Rendering ============
static void RenderSidebar(void) {
    if (!ImGui::CollapsingHeader("YM2413 Hardware##ym", nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Connection status
    if (SPFMManager::IsConnected()) {
        const char* desc = (SPFMManager::GetActiveChipType() == SPFMManager::CHIP_YM2413)
            ? "YM2413" : SPFMManager::GetActiveChipType() == SPFMManager::CHIP_YM2163 ? "YM2163"
            : SPFMManager::GetActiveChipType() == SPFMManager::CHIP_SN76489 ? "SN76489" : "None";
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected (%s)", desc);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Disconnected");
    }

    // Connect / Disconnect buttons
    {
        bool isYM = (SPFMManager::GetActiveChipType() == SPFMManager::CHIP_YM2413);
        if (isYM) {
            if (ImGui::Button("Disconnect##ym", ImVec2(-1, 0))) {
                SPFMManager::SwitchToChipType(SPFMManager::CHIP_NONE);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set chip type to None, send SPFM reset");
        } else {
            if (ImGui::Button("Connect##ym", ImVec2(-1, 0))) {
                SPFMManager::SwitchToChipType(SPFMManager::CHIP_YM2413);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch to YM2413 (OPLL) mode");
        }
    }

    // SPFM Slot assignment
    {
        ImGui::Spacing();
        static const char* chipLabels[] = {"(none)","YM2413","AY8910","SN76489"};
        for (int s = 0; s < 4; s++) {
            int cur = VGMSync::GetSlotChip(s);
            ImGui::Text("Slot %d:", s); ImGui::SameLine();
            if (ImGui::Combo(("##slot_ym_"+std::to_string(s)).c_str(), &cur, chipLabels, 4)) {
                VGMSync::SetSlotChip(s, cur);
                VGMSync::SaveSlotPreset(VGMSync::GetLastComboKey());
            }
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // Rhythm mode checkbox
    if (ImGui::Checkbox("Rhythm Mode##ymrhythm", &s_rhythmMode)) {
        uint8_t rhythmVal = s_rhythmMode ? 0x20 : 0x00;
        s_regShadow[0x0E] = rhythmVal;
        if (s_connected) {
            ym2413_write_reg(0x0E, rhythmVal);
            safe_flush();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable/disable rhythm mode (register 0x0E)");

    ImGui::Spacing(); ImGui::Separator();

    // Test popup button
    if (s_testRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Test Running...##ymtestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Debug Test##ymtestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Loop settings
    ImGui::TextDisabled("VGM Loop Count");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max internal loop repetitions\n0 = infinite loop");
    ImGui::SameLine();
    int maxL = s_vgmMaxLoops;
    if (ImGui::InputInt("##ymmaxloops", &maxL, 1, 5)) {
        if (maxL < 0) maxL = 0;
        s_vgmMaxLoops = maxL;
    }

    // Seek mode
    ImGui::TextDisabled("Seek Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fast-forward: replay commands from start\nDirect: skip without HW, reset chip on target");
    ImGui::RadioButton("Fast-forward##ymseek", &s_seekMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Direct##ymseek", &s_seekMode, 1);

    // Fadeout
    ImGui::TextDisabled("Loop Fadeout");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fade out volume before final loop end\n0 = disabled");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragFloat("##ymfadeout", &s_fadeoutDuration, 0.1f, 0.0f, 30.0f, "%.1f sec")) {
        if (s_fadeoutDuration < 0.0f) s_fadeoutDuration = 0.0f;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Flush Mode
    ImGui::TextDisabled("Flush Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Register: flush after each reg write\nCommand: flush after each VGM command");
    if (ImGui::RadioButton("Register##ymflush", &s_flushMode, 1)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Command##ymflush", &s_flushMode, 2)) SaveConfig();

    // Timer Mode
    ImGui::TextDisabled("Timer Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("H-Prec: waitable timer+spin\nHybrid: Sleep+spin\nMM-Timer: timeSetEvent\nVGMPlay: 1ms periodic timer\nOptVGMPlay: periodic+batch lookahead");
    if (ImGui::RadioButton("H-Prec##ymtimer", &s_timerMode, 0)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Hybrid##ymtimer", &s_timerMode, 1)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("MM-Timer##ymtimer", &s_timerMode, 2)) SaveConfig();
    if (ImGui::RadioButton("VGMPlay##ymtimer", &s_timerMode, 3)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("OptVGMPlay##ymtimer", &s_timerMode, 7)) SaveConfig();

    ImGui::Spacing(); ImGui::Separator();

    // Scope settings
    if (ImGui::CollapsingHeader("Scope##ymscope", nullptr, 0)) {
        ImGui::Checkbox("Show Scope##ym", &s_showScope);
        if (s_showScope) {
            ImGui::SliderFloat("Height##ym", &s_scopeHeight, 20, 300);
            ImGui::SliderFloat("Amplitude##ym", &s_scopeAmplitude, 0.5f, 10.0f);
            ImGui::SliderInt("Samples##ym", &s_scopeSamples, 100, 1000);
        }
    }

    // Channel Colors
    if (ImGui::CollapsingHeader("Channel Colors##ymchcolors")) {
        static const char* kChLabels[14] = {
            "Ch0", "Ch1", "Ch2", "Ch3", "Ch4", "Ch5", "Ch6", "Ch7", "Ch8",
            "BD", "SD", "TOM", "HH", "CYM"
        };
        for (int i = 0; i < 14; i++) {
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
            if (ImGui::ColorEdit4(("##ymclredit" + std::to_string(i)).c_str(), colF, ImGuiColorEditFlags_NoInputs)) {
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
        if (ImGui::SmallButton("Reset All Colors##ymrclrall")) {
            memset(kChColorsCustom, 0, sizeof(kChColorsCustom));
            SaveConfig();
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // VGM File Info
    if (ImGui::CollapsingHeader("VGM File Info##yminfo", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
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

            // Rhythm mode status
            ImGui::TextDisabled("Rhythm:"); ImGui::SameLine();
            ImGui::Text("%s", s_rhythmMode ? "ON" : "OFF");

            ImGui::Spacing();
            if (!s_trackName.empty()) {
                ImGui::TextDisabled("Track:");
                ImGui::Indent();
                DrawScrollingText("##ymtrack", s_trackName.c_str(), IM_COL32(255, 255, 102, 255));
                ImGui::Unindent();
            }
            if (!s_gameName.empty()) {
                ImGui::TextDisabled("Game:");
                ImGui::SameLine();
                DrawScrollingText("##ymgame", s_gameName.c_str(), IM_COL32(200, 200, 200, 255));
            }
            if (!s_systemName.empty()) {
                ImGui::TextDisabled("System:");
                ImGui::SameLine();
                ImGui::Text("%s", s_systemName.c_str());
            }
            if (!s_artistName.empty()) {
                ImGui::TextDisabled("Artist:");
                ImGui::SameLine();
                DrawScrollingText("##ymartist", s_artistName.c_str(), IM_COL32(200, 200, 200, 255));
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
    bool playerOpen = ImGui::TreeNodeEx("YM2413 VGM Player##ymvgmplayer",
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
    if (ImGui::SmallButton("<<##ymminiprev")) {
        if (!s_playlist.empty()) PlayPlaylistPrev();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous");
    ImGui::SameLine();
    if (isPlaying) {
        if (ImGui::SmallButton("||##ymminipause")) PauseVGMPlayback();
    } else if (isPaused) {
        if (ImGui::SmallButton(">##ymminipause")) PauseVGMPlayback();
    } else if (hasFile) {
        if (ImGui::SmallButton(">##ymminipause")) StartVGMPlayback();
    } else {
        ImGui::SmallButton(">##ymminipause");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play / Pause");
    ImGui::SameLine();
    if (ImGui::SmallButton(">>##ymmininext")) {
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
        DrawScrollingText("##ymminifilename", displayName, nameCol, maxNameW);
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
        if (ImGui::Button("Play##ymplay", ImVec2(buttonWidth, 30))) {
            if (!hasFile) { /* nothing */ }
            else if (isPaused) PauseVGMPlayback();
            else { SeekVGMToStart(); StartVGMPlayback(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause##ympause", ImVec2(buttonWidth, 30))) {
            if (isPlaying) PauseVGMPlayback();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop##ymstop", ImVec2(buttonWidth, 30))) {
            s_vgmTrackEnded = false;
            StopVGMPlayback();
        }

        float navWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
        if (ImGui::Button("<< Prev##ymprev", ImVec2(navWidth, 25))) {
            if (!s_playlist.empty()) PlayPlaylistPrev();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>##ymnext", ImVec2(navWidth, 25))) {
            if (!s_playlist.empty()) PlayPlaylistNext();
        }

        ImGui::Checkbox("Auto-play##ym", &s_autoPlayNext);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-play next track when current finishes");
        ImGui::SameLine();
        const char* modeText = s_isSequentialPlayback ? "Seq" : "Rnd";
        if (ImGui::Button(modeText, ImVec2(35, 0))) s_isSequentialPlayback = !s_isSequentialPlayback;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sequential / Random");
        ImGui::SameLine();
        if (ImGui::Button("Open##ymopen")) OpenVGMFileDialog();

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
            ImGui::SliderFloat("##ymseek", &seek_progress, 0.0f, 1.0f, "");
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
                        ApplyShadowState();
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
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "YM2413 Registers");
    ImGui::Separator();

    UINT8 rhythmReg = s_regShadow[0x0E];
    bool  rhythmMode = (rhythmReg >> 5) & 1;

    ImGui::TextDisabled("-- FM --");
    for (int ch = 0; ch < 9; ch++) {
        UINT8 lo   = s_regShadow[0x10 + ch];
        UINT8 hi   = s_regShadow[0x20 + ch];
        UINT8 inst = s_regShadow[0x30 + ch];
        int fnum   = ((hi & 0x01) << 8) | lo;
        int block  = (hi >> 1) & 0x07;
        int patch  = (inst >> 4) & 0x0F;
        int vol    = inst & 0x0F;
        int midi = (fnum > 0) ? fn_to_midi_note(ch) : -1;
        // More useful keyon: bit4 set AND volume audible
        bool activeKeyon = ((hi >> 4) & 1) || (fnum > 0 && vol < 15);
        bool isRhCh = rhythmMode && ch >= 6;

        char chLabel[32];
        if (isRhCh) {
            static const char* kRhChName[3] = {"BD","HH+SD","TOM+CYM"};
            snprintf(chLabel, sizeof(chLabel), "CH%d [%s]##ym2413ch%d",
                ch+1, kRhChName[ch-6], ch);
        } else {
            snprintf(chLabel, sizeof(chLabel), "CH%d##ym2413ch%d", ch+1, ch);
        }

        ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_DefaultOpen;
        bool nodeOpen = ImGui::TreeNodeEx(chLabel, nodeFlags);
        if (nodeOpen) {
            // Channel info: Note / Patch / Vol (for non-rhythm channels)
            if (!isRhCh) {
                char noteBuf[8];
                if (activeKeyon && midi >= 0)
                    snprintf(noteBuf, sizeof(noteBuf), "%s%d", kNoteNames[midi % 12], midi / 12 - 1);
                else if (activeKeyon)
                    snprintf(noteBuf, sizeof(noteBuf), "F%03X/%d", fnum, block);
                else
                    snprintf(noteBuf, sizeof(noteBuf), "---");
                ImVec4 infoCol = activeKeyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                if (ImGui::BeginTable("##ym2413info", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthFixed, 68.f);
                    ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableHeadersRow();
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextColored(infoCol, "%s", noteBuf);
                    ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol, "%s", kInstNames[patch]);
                    ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol, "%d", vol);
                    ImGui::EndTable();
                }
                ImGui::Spacing();
            }

            // Resolve patch params: custom from regs 0x00-0x07, built-in from ROM
            UINT8 pTL[2], pFB[2], pAR[2], pDR[2], pSL[2], pRR[2], pKL[2], pML[2], pAM[2], pVIB[2];
            if (patch == 0) {
                UINT8 r0=s_regShadow[0x00],r1=s_regShadow[0x01];
                UINT8 r2=s_regShadow[0x02],r3=s_regShadow[0x03];
                UINT8 r4=s_regShadow[0x04],r5=s_regShadow[0x05];
                UINT8 r6=s_regShadow[0x06],r7=s_regShadow[0x07];
                pAM[0]=(r0>>7)&1; pVIB[0]=(r0>>6)&1; pML[0]=r0&0xF; pKL[0]=(r2>>6)&3;
                pAM[1]=(r1>>7)&1; pVIB[1]=(r1>>6)&1; pML[1]=r1&0xF; pKL[1]=(r3>>6)&3;
                pTL[0]=r2&0x3F; pFB[0]=(r3>>1)&7;
                pTL[1]=0;       pFB[1]=0;
                pAR[0]=r4>>4; pDR[0]=r4&0xF; pAR[1]=r5>>4; pDR[1]=r5&0xF;
                pSL[0]=r6>>4; pRR[0]=r6&0xF; pSL[1]=r7>>4; pRR[1]=r7&0xF;
            } else {
                for (int op=0;op<2;op++) {
                    pTL[op]=kOPLLRom[patch][op][0]; pFB[op]=kOPLLRom[patch][op][1];
                    pAR[op]=kOPLLRom[patch][op][2]; pDR[op]=kOPLLRom[patch][op][3];
                    pSL[op]=kOPLLRom[patch][op][4]; pRR[op]=kOPLLRom[patch][op][5];
                    pKL[op]=kOPLLRom[patch][op][6]; pML[op]=kOPLLRom[patch][op][7];
                    pAM[op]=kOPLLRom[patch][op][8]; pVIB[op]=kOPLLRom[patch][op][9];
                }
            }

            // 10-column operator table: OP/TL/FB/AR/DR/SL/RR/KL/ML/AM_VIB
            if (ImGui::BeginTable("##oplltbl", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("OP",     ImGuiTableColumnFlags_WidthFixed, 28.f);
                ImGui::TableSetupColumn("TL",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("FB",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("AR",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("DR",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("SL",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("RR",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("KL",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("ML",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                ImGui::TableSetupColumn("AM/VIB", ImGuiTableColumnFlags_WidthFixed, 46.f);
                ImGui::TableHeadersRow();
                static const char* opName[2] = {"MOD","CAR"};
                for (int op = 0; op < 2; op++) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", opName[op]);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%d", pTL[op]);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", pFB[op]);
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", pAR[op]);
                    ImGui::TableSetColumnIndex(4); ImGui::Text("%d", pDR[op]);
                    ImGui::TableSetColumnIndex(5); ImGui::Text("%d", pSL[op]);
                    ImGui::TableSetColumnIndex(6); ImGui::Text("%d", pRR[op]);
                    ImGui::TableSetColumnIndex(7); ImGui::Text("%d", pKL[op]);
                    ImGui::TableSetColumnIndex(8); ImGui::Text("%d", pML[op]);
                    ImGui::TableSetColumnIndex(9); ImGui::Text("%d/%d", pAM[op], pVIB[op]);
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    }

    // Rhythm channels table
    if (rhythmMode) {
        // Table display order: BD, HH, SD, TM, CY
        // Volume per MDPlayer: BD=0x36&0x0F, HH=0x37>>4, SD=0x37&0x0F, TOM=0x38>>4, CYM=0x38&0x0F
        static const char* kRhName[5] = {"BD","HH","SD","TM","CY"};
        static const int kRhBit[5] = {4, 0, 3, 2, 1};
        ImGui::Spacing();
        ImGui::TextDisabled("-- Rhythm --");
        UINT8 bdVol  =  s_regShadow[0x36] & 0x0F;
        UINT8 hhVol  = (s_regShadow[0x37] >> 4) & 0x0F;
        UINT8 sdVol  =  s_regShadow[0x37] & 0x0F;
        UINT8 tomVol = (s_regShadow[0x38] >> 4) & 0x0F;
        UINT8 cymVol =  s_regShadow[0x38] & 0x0F;
        UINT8 rhVols[5] = {bdVol, hhVol, sdVol, tomVol, cymVol};
        // Map table order to decay array order (BD,SD,TOM,HH,CYM)
        static const int kRhDecayIdx[5] = {0, 3, 1, 2, 4};
        if (ImGui::BeginTable("##ym2413rhythm", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Inst", ImGuiTableColumnFlags_WidthFixed, 32.f);
            ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
            ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 30.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < 5; i++) {
                bool kon = (rhythmReg >> kRhBit[i]) & 1;
                bool decay = s_rhyDecay[kRhDecayIdx[i]] > 0.01f;
                ImVec4 col = decay ? ImVec4(1.0f,0.8f,0.4f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "%s", kRhName[i]);
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", decay ? "[ON]" : "[--]");
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%d", rhVols[i]);
            }
            ImGui::EndTable();
        }
    }

    // All Regs hex dump
    ImGui::Spacing();
    if (ImGui::TreeNode("All Regs##ym2413")) {
        if (ImGui::BeginTable("##ym2413allregs", 8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (int col = 0; col < 8; col++) {
                ImGui::TableSetupColumn(col == 0 ? "Reg" : "Val", ImGuiTableColumnFlags_WidthFixed, 36.f);
            }
            ImGui::TableHeadersRow();
            for (int row = 0; row < 8; row++) {
                ImGui::TableNextRow();
                for (int col = 0; col < 8; col++) {
                    ImGui::TableSetColumnIndex(col);
                    int reg = row * 8 + col;
                    if (reg < 0x40) {
                        ImGui::Text("%02X", s_regShadow[reg]);
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

static void RenderFileBrowser(void) {
    ImGui::SetNextItemAllowOverlap();
    bool browserOpen = ImGui::TreeNodeEx("YM2413 File Browser##ymfb",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_DefaultOpen);

    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##ymfbfilter", "Filter...", s_fileBrowserFilter, sizeof(s_fileBrowserFilter));

    if (!browserOpen) return;

    // Navigation buttons
    if (ImGui::Button("<##ymfbback", ImVec2(25, 0))) NavBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">##ymfbfwd", ImVec2(25, 0))) NavForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^##ymfbup", ImVec2(25, 0))) NavToParent();
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
            if (ImGui::Button("...##ymellipsis")) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
            }
            ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine();
        }
        for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
            std::string btnId = segments[i] + "##ymseg" + std::to_string(i);
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
            ImGui::InvisibleButton("##ympathEmpty", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ymPathInput", s_pathInput, MAX_PATH, ImGuiInputTextFlags_EnterReturnsTrue)) {
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
    ImGui::BeginChild("YmFileList##ymfl", ImVec2(-1, fileAreaHeight), true);

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
    if (ImGui::CollapsingHeader("YM2413 Debug Log##ymlog", nullptr, 0)) {
        ImGui::Checkbox("Auto-scroll##ymlog", &s_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##ymlog")) {
            s_log.clear(); s_logDisplay[0] = '\0'; s_logLastSize = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All##ymlog")) {
            ImGui::SetClipboardText(s_log.c_str());
        }
        size_t copyLen = s_log.size() < sizeof(s_logDisplay) - 1 ? s_log.size() : sizeof(s_logDisplay) - 1;
        memcpy(s_logDisplay, s_log.c_str(), copyLen);
        s_logDisplay[copyLen] = '\0';
        bool changed = (s_log.size() != s_logLastSize);
        s_logLastSize = s_log.size();
        if (s_logAutoScroll && changed) s_logScrollToBottom = true;

        float logH = 150;
        ImGui::BeginChild("YmDebugLogRegion", ImVec2(0, logH), true, ImGuiWindowFlags_HorizontalScrollbar);
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
    bool historyOpen = ImGui::TreeNodeEx("YM2413 Folder History##ymhist",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_ymHistoryCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
    if (historyOpen) s_ymHistoryCollapsed = false;
    else s_ymHistoryCollapsed = true;

    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Clear##ymHistClear")) {
        s_folderHistory.clear(); SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All");
    ImGui::SameLine();
    if (s_histSortMode == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Time##ymhistSortTime")) s_histSortMode = 0;
    if (s_histSortMode == 0) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by time");
    ImGui::SameLine();
    if (s_histSortMode == 1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Freq##ymhistSortFreq")) s_histSortMode = 1;
    if (s_histSortMode == 1) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##ymHistFilter", "Filter...", s_histFilter, sizeof(s_histFilter));

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
        ImGui::BeginChild("YmHistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

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
    ImGui::BeginChild("YM_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoKeyboard();
    ImGui::EndChild();
    ImGui::BeginChild("YM_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
    RenderLevelMeters();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginChild("YM_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderRegisterTable();
    ImGui::EndChild();

    // Bottom section: Player + FileBrowser | Log+History
    ImGui::BeginChild("YM_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderPlayerBar();
    RenderFileBrowser();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("YM_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();

    // Scope (optional, very bottom)
    RenderScopeArea();
}

void Render() {
    ImGui::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_FirstUseEver);
    bool visible = ImGui::Begin("YM2413(OPLL)");
    ImGui::BeginChild("YM_LeftPane", ImVec2(300, 0), true);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("YM_RightPane", ImVec2(0, 0), false);
    RenderMain();
    ImGui::EndChild();
    ImGui::End();

    // Popups
    RenderTestPopup();
}

bool WantsKeyboardCapture() { return false; }

} // namespace YM2413Window
