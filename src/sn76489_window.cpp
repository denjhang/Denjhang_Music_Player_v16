// sn76489_window.cpp - SN76489 (DCSG) Hardware Control Window
// Controls real SN76489 chip via FTDI/SPFM Light interface
// VGM hardware playback + debug test functions + file browser

#include "sn76489_window.h"
#include "sn76489/spfm.h"
#include "sn76489/sn76489.h"
#include "chip_control.h"
#include "chip_window_ym2163.h"
#include "midi_player.h"
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

static ImU32 kChColors[4] = {
    IM_COL32(80, 220, 80, 255),   // Ch0: green
    IM_COL32(80, 140, 255, 255),  // Ch1: blue
    IM_COL32(255, 80, 80, 255),   // Ch2: red
    IM_COL32(255, 180, 60, 255)   // Ch3: orange (noise)
};
static const char* kChNames[4] = { "Tone0", "Tone1", "Tone2", "Noise" };

// ============ Connection State ============
static bool s_connected = false;
static bool s_manualDisconnect = false;
static int s_connectRetries = 0;
static const int MAX_RETRIES = 10;
static const int CHECK_INTERVAL_MS = 2000;
static LARGE_INTEGER s_lastCheckTime;
static LARGE_INTEGER s_perfFreq;

// ============ Test State ============
static bool s_testRunning = false;
static int s_testType = 0;
static int s_testStep = 0;
static double s_testStepMs = 0.0;
static LARGE_INTEGER s_testStartTime;

// ============ VGM Playback State ============
static FILE* s_vgmFile = nullptr;
static char s_vgmPath[MAX_PATH] = "";
static bool s_vgmLoaded = false;
static bool s_vgmPlaying = false;
static bool s_vgmPaused = false;
static bool s_vgmTrackEnded = false;  // true when track finished naturally
static UINT32 s_vgmSamplesRemaining = 0;
static UINT32 s_vgmTotalSamples = 0;
static UINT32 s_vgmCurrentSamples = 0;
static UINT32 s_vgmDataOffset = 0;
static UINT32 s_vgmLoopOffset = 0;
static UINT32 s_vgmLoopSamples = 0;
static int s_vgmLoopCount = 0;
static LARGE_INTEGER s_vgmWaitStart;
static LARGE_INTEGER s_vgmPauseStart;
static double s_vgmPauseAccum = 0.0;

// GD3 tags
static std::string s_trackName, s_gameName, s_systemName, s_artistName;
static UINT32 s_vgmVersion = 0;

// ============ Channel State ============
static uint8_t s_vol[4] = {15, 15, 15, 15};
static uint8_t s_noiseType = 0;
static uint8_t s_noiseFreq = 0;
static bool s_noiseUseCh2 = false;
static uint16_t s_fullPeriod[3] = {0, 0, 0};

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
static void sn76489_write(uint8_t data) {
    ::spfm_write_data(0, data);
    ::spfm_hw_wait(3);
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
    sn76489_write(0x9F); sn76489_write(0xBF);
    sn76489_write(0xDF); sn76489_write(0xFF);
}

static void sn76489_set_noise(uint8_t ntype, uint8_t shift_freq) {
    sn76489_write(sn76489_noise_latch(ntype, shift_freq));
}

static void InitHardware(void) {
    sn76489_mute_all();
    sn76489_set_noise(0, 0);
    sn76489_set_tone(0, 0); sn76489_set_tone(1, 0); sn76489_set_tone(2, 0);
    spfm_flush();
}

static void ResetState(void) {
    s_testRunning = false; s_testType = 0; s_testStep = 0; s_testStepMs = 0.0;
    s_vol[0] = s_vol[1] = s_vol[2] = s_vol[3] = 15;
    s_noiseType = 0; s_noiseFreq = 0; s_noiseUseCh2 = false;
    s_fullPeriod[0] = s_fullPeriod[1] = s_fullPeriod[2] = 0;
}

// ============ Connection ============
static bool CheckHardwarePresent(void) {
    DWORD numDevs = 0;
    return (FT_CreateDeviceInfoList(&numDevs) == FT_OK && numDevs > 0);
}

static bool CheckHardwareReady(void) {
    DWORD numDevs = 0;
    if (FT_CreateDeviceInfoList(&numDevs) != FT_OK || numDevs == 0) return false;
    FT_DEVICE_LIST_INFO_NODE devInfo;
    if (FT_GetDeviceInfoDetail(0, &devInfo.Flags, &devInfo.Type,
        &devInfo.ID, &devInfo.LocId, devInfo.SerialNumber,
        devInfo.Description, &devInfo.ftHandle) != FT_OK) return false;
    return !(devInfo.Flags & 0x01);
}

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
    if (!s_connected) return;
    if (s_testRunning) s_testRunning = false;
    if (s_vgmPlaying) s_vgmPlaying = false;
    InitHardware(); spfm_cleanup();
    s_connected = false; s_manualDisconnect = true;
    YM2163::g_manualDisconnect = false;
    DcLog("[SN] Hardware disconnected\n");
}

static void CheckAutoConnect(void) {
    if (s_manualDisconnect || s_connected) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - s_lastCheckTime.QuadPart) / s_perfFreq.QuadPart * 1000.0;
    if (elapsed < CHECK_INTERVAL_MS) return;
    s_lastCheckTime = now;
    if (!CheckHardwarePresent()) { s_connectRetries = 0; return; }
    if (!CheckHardwareReady()) {
        s_connectRetries++;
        if (s_connectRetries >= MAX_RETRIES) s_connectRetries = 0;
        return;
    }
    ConnectHardware();
    s_connectRetries = s_connected ? 0 : (s_connectRetries + 1);
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

    FILE* f = fopen(path, "rb");
    if (!f) { DcLog("[VGM] Cannot open: %s\n", path); return false; }

    char sig[4]; fread(sig, 1, 4, f);
    if (memcmp(sig, "Vgm ", 4) != 0) { fclose(f); DcLog("[VGM] Not a VGM file\n"); return false; }

    fseek(f, 0x08, SEEK_SET);
    s_vgmVersion = ReadLE32(f);
    UINT32 snClock = ReadLE32(f);
    fseek(f, 0x14, SEEK_SET);
    UINT32 gd3Off = ReadLE32(f);
    s_vgmTotalSamples = ReadLE32(f);
    s_vgmLoopOffset = ReadLE32(f);
    s_vgmLoopSamples = ReadLE32(f);

    UINT32 dataOff = 0x40;
    fseek(f, 0x34, SEEK_SET);
    UINT32 hdrDataOff = ReadLE32(f);
    if (hdrDataOff >= 0x40) dataOff = hdrDataOff;
    s_vgmDataOffset = dataOff;

    ParseGD3Tags(f, gd3Off);

    fclose(f);
    s_vgmFile = fopen(path, "rb");
    if (!s_vgmFile) { DcLog("[VGM] Reopen failed\n"); return false; }

    snprintf(s_vgmPath, MAX_PATH, "%s", path);
    s_vgmLoaded = true;
    s_vgmCurrentSamples = 0;
    s_vgmLoopCount = 0;
    s_vgmPaused = false;
    s_vgmPauseAccum = 0.0;
    s_vgmSamplesRemaining = 0;

    // Update playlist index
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

static int VGMProcessCommand(void) {
    UINT8 cmd;
    if (fread(&cmd, 1, 1, s_vgmFile) != 1) return -1;

    switch (cmd) {
        case 0x50: { UINT8 data; if (fread(&data, 1, 1, s_vgmFile) != 1) return -1; sn76489_write(data); return 0; }
        case 0x51: { UINT8 buf[2]; fread(buf, 1, 2, s_vgmFile); return 0; }
        case 0x52: case 0x53: { UINT8 buf[2]; fread(buf, 1, 2, s_vgmFile); return 0; }
        case 0x54: { UINT8 buf[2]; fread(buf, 1, 2, s_vgmFile); return 0; }
        case 0x55: { UINT8 buf[2]; fread(buf, 1, 2, s_vgmFile); return 0; }
        case 0x56: case 0x57: { UINT8 buf[2]; fread(buf, 1, 2, s_vgmFile); return 0; }
        case 0x61: { UINT8 buf[2]; fread(buf, 1, 2, s_vgmFile); return buf[0] | (buf[1] << 8); }
        case 0x62: return 735;
        case 0x63: return 882;
        case 0x66: {
            if (s_vgmLoopOffset > 0) { fseek(s_vgmFile, s_vgmLoopOffset, SEEK_SET); s_vgmLoopCount++; return 0; }
            return -1;
        }
        case 0x67: { UINT8 buf[3]; fread(buf, 1, 3, s_vgmFile); UINT32 size = buf[0] | (buf[1] << 8) | (buf[2] << 16); fseek(s_vgmFile, size, SEEK_CUR); return 0; }
        default:
            if (cmd >= 0x70 && cmd <= 0x7F) return (cmd & 0x0F) + 1;
            return 0;
    }
}

static void StartVGMPlayback(void) {
    if (!s_vgmLoaded || !s_connected) return;
    StopTest();
    if (s_vgmFile) { fclose(s_vgmFile); s_vgmFile = fopen(s_vgmPath, "rb"); }
    if (!s_vgmFile) return;
    fseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
    InitHardware();
    s_vgmPlaying = true; s_vgmPaused = false; s_vgmTrackEnded = false;
    s_vgmCurrentSamples = 0; s_vgmSamplesRemaining = 0;
    s_vgmLoopCount = 0; s_vgmPauseAccum = 0.0;
    QueryPerformanceFrequency(&s_perfFreq);
    QueryPerformanceCounter(&s_vgmWaitStart);
    DcLog("[VGM] Playing\n");
}

static void StopVGMPlayback(void) {
    if (!s_vgmPlaying) return;
    s_vgmPlaying = false; s_vgmPaused = false;
    if (s_connected) InitHardware();
    DcLog("[VGM] Stopped at %.1fs\n", (double)s_vgmCurrentSamples / 44100.0);
}

static void PauseVGMPlayback(void) {
    if (!s_vgmPlaying) return;
    s_vgmPaused = !s_vgmPaused;
    if (s_vgmPaused) {
        QueryPerformanceCounter(&s_vgmPauseStart);
    } else {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        s_vgmPauseAccum += (double)(now.QuadPart - s_vgmPauseStart.QuadPart) / s_perfFreq.QuadPart;
        QueryPerformanceCounter(&s_vgmWaitStart);
        s_vgmSamplesRemaining = 0;
    }
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
    s_vgmFile = fopen(s_vgmPath, "rb");
    if (!s_vgmFile) return;
    fseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
    s_vgmCurrentSamples = 0; s_vgmLoopCount = 0;
    s_vgmSamplesRemaining = 0; s_vgmPauseAccum = 0.0;
    s_vgmTrackEnded = false;
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
    spfm_flush(); s_testStep++;
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
        case 0: sn76489_set_vol(0, 0); spfm_flush(); break;
        case 1: sn76489_set_vol(1, 0); spfm_flush(); break;
        case 4: sn76489_set_noise(0, 0); sn76489_set_vol(3, 0); spfm_flush(); break;
    }
}

// ============ Public API ============
void Init() {
    QueryPerformanceFrequency(&s_perfFreq);
    QueryPerformanceCounter(&s_lastCheckTime);
    LoadConfig();
    if (s_currentPath[0] != '\0') {
        RefreshFileList();
    } else {
        GetExeDir(s_currentPath, MAX_PATH);
        RefreshFileList();
    }
}

void Shutdown() {
    SaveConfig();
    DisconnectHardware();
}

void Update() {
    CheckAutoConnect();

    // VGM playback
    if (s_vgmPlaying && !s_vgmPaused) {
        if (s_vgmSamplesRemaining > 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart - s_vgmWaitStart.QuadPart) / s_perfFreq.QuadPart * 1000.0;
            double waitMs = (double)s_vgmSamplesRemaining / 44100.0 * 1000.0;
            if (elapsed >= waitMs) s_vgmSamplesRemaining = 0;
        }
        while (s_vgmPlaying && !s_vgmPaused && s_vgmSamplesRemaining == 0) {
            int samples = VGMProcessCommand();
            if (samples < 0) {
                s_vgmTrackEnded = true;
                StopVGMPlayback();
                // Auto-play next
                if (s_autoPlayNext && !s_playlist.empty()) PlayPlaylistNext();
                break;
            }
            if (samples > 0) {
                s_vgmSamplesRemaining = samples;
                s_vgmCurrentSamples += samples;
                QueryPerformanceCounter(&s_vgmWaitStart);
            }
        }
    }

    // Test
    if (s_testRunning) {
        double elapsed = GetTestElapsedMs();
        if (elapsed >= s_testStepMs) { TestStep(); s_testStepMs += GetStepDurationMs(s_testType); }
    }
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

    if (ImGui::Button("Scale##sn", ImVec2(-1, 0))) StartTest(0);
    if (ImGui::Button("Arpeggio##sn", ImVec2(-1, 0))) StartTest(1);
    if (ImGui::Button("Chord##sn", ImVec2(-1, 0))) StartTest(2);
    if (ImGui::Button("Vol Sweep##sn", ImVec2(-1, 0))) StartTest(3);
    if (ImGui::Button("Noise##sn", ImVec2(-1, 0))) StartTest(4);
    if (s_testRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop Test##sn", ImVec2(-1, 0))) StopTest();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running...");
    } else {
        if (ImGui::Button("Stop Test##sn", ImVec2(-1, 0))) StopTest();
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
                ImGui::TextDisabled("(%d loops)", s_vgmLoopCount);
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
        float progress = (float)s_vgmCurrentSamples / (float)s_vgmTotalSamples;
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
        double durSec = (double)s_vgmTotalSamples / 44100.0;
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

        // Progress bar
        ImGui::Spacing();
        if (hasFile && s_vgmTotalSamples > 0) {
            float progress = (float)s_vgmCurrentSamples / (float)s_vgmTotalSamples;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            double posSec = (double)s_vgmCurrentSamples / 44100.0;
            double durSec = (double)s_vgmTotalSamples / 44100.0;
            int curMin = (int)posSec / 60; int curSecI = (int)posSec % 60;
            int totMin = (int)durSec / 60; int totSecI = (int)durSec % 60;
            char posStr[32], durStr[32];
            snprintf(posStr, sizeof(posStr), "%02d:%02d", curMin, curSecI);
            snprintf(durStr, sizeof(durStr), "%02d:%02d", totMin, totSecI);
            ImGui::Text("%s / %s", posStr, durStr);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
            ImGui::ProgressBar(progress, ImVec2(-1, 0));
            ImGui::PopStyleColor();
            if (s_vgmLoopCount > 0) ImGui::TextDisabled("Loop #%d", s_vgmLoopCount);
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

static void RenderChannelControls(void) {
    if (!ImGui::CollapsingHeader("Channel Controls##snch", nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    for (int ch = 0; ch < 3; ch++) {
        ImGui::PushID(ch);
        ImGui::TextColored(ImVec4(
            ((kChColors[ch] >> 0) & 0xFF) / 255.0f,
            ((kChColors[ch] >> 8) & 0xFF) / 255.0f,
            ((kChColors[ch] >> 16) & 0xFF) / 255.0f, 1.0f), "%s", kChNames[ch]);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60.0f);
        int volVal = s_vol[ch];
        if (ImGui::SliderInt("##vol", &volVal, 0, 15)) {
            s_vol[ch] = (uint8_t)volVal;
            if (s_connected) { sn76489_set_vol((uint8_t)ch, s_vol[ch]); spfm_flush(); }
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(120.0f);
        int periodVal = (int)s_fullPeriod[ch];
        if (ImGui::SliderInt("##period", &periodVal, 0, 1023)) {
            s_fullPeriod[ch] = (uint16_t)periodVal;
            if (s_connected) { sn76489_set_tone((uint8_t)ch, s_fullPeriod[ch]); spfm_flush(); }
        }
        ImGui::PopID();
    }

    // Noise
    ImGui::PushID(100);
    ImGui::TextColored(ImVec4(
        ((kChColors[3] >> 0) & 0xFF) / 255.0f,
        ((kChColors[3] >> 8) & 0xFF) / 255.0f,
        ((kChColors[3] >> 16) & 0xFF) / 255.0f, 1.0f), "Noise");
    ImGui::SameLine(); ImGui::SetNextItemWidth(60.0f);
    int nVolVal = s_vol[3];
    if (ImGui::SliderInt("##nvol", &nVolVal, 0, 15)) {
        s_vol[3] = (uint8_t)nVolVal;
        if (s_connected) { sn76489_set_vol(3, s_vol[3]); spfm_flush(); }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Periodic##snp", s_noiseType == 0)) { s_noiseType = 0; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); } }
    ImGui::SameLine();
    if (ImGui::RadioButton("White##snw", s_noiseType == 1)) { s_noiseType = 1; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); } }
    ImGui::SameLine();
    if (ImGui::RadioButton("Ch2##snfch2", s_noiseUseCh2)) { s_noiseUseCh2 = true; if (s_connected) { sn76489_set_noise(s_noiseType, 3); spfm_flush(); } }
    ImGui::SameLine();
    if (ImGui::RadioButton("Shift##snfsh", !s_noiseUseCh2)) { s_noiseUseCh2 = false; if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); } }
    if (!s_noiseUseCh2) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(60.0f);
        int nFreqVal = s_noiseFreq;
        if (ImGui::SliderInt("##nfreq", &nFreqVal, 0, 3)) {
            s_noiseFreq = (uint8_t)nFreqVal;
            if (s_connected) { sn76489_set_noise(s_noiseType, s_noiseFreq); spfm_flush(); }
        }
    }
    ImGui::PopID();
}

static void RenderRegisterTable(void) {
    if (!ImGui::CollapsingHeader("Registers##snreg", nullptr, 0))
        return;

    if (ImGui::BeginTable("##sn76489regs", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 3; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", ch);
            ImGui::TableSetColumnIndex(1); ImGui::Text("Tone%d", ch);
            ImGui::TableSetColumnIndex(2); ImGui::Text("0x%03X (%u)", s_fullPeriod[ch], s_fullPeriod[ch]);
            ImGui::TableSetColumnIndex(3);
            double freq = (s_fullPeriod[ch] > 0) ? SN76489_CLOCK_NTSC / (32.0 * s_fullPeriod[ch]) : 0.0;
            ImGui::Text("%.1f Hz", freq);
        }
        for (int ch = 0; ch < 4; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", ch);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s Vol", kChNames[ch]);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u (0x%X)", s_vol[ch], s_vol[ch]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", s_vol[ch] == 15 ? "[MUTE]" : "");
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("3");
        ImGui::TableSetColumnIndex(1); ImGui::Text("Noise");
        ImGui::TableSetColumnIndex(2); ImGui::Text("0x%02X", sn76489_noise_latch(s_noiseType, s_noiseUseCh2 ? 3 : s_noiseFreq));
        ImGui::TableSetColumnIndex(3); ImGui::Text("%s %s", s_noiseType == 0 ? "Periodic" : "White", s_noiseUseCh2 ? "(Ch2)" : "");
        ImGui::EndTable();
    }
}

static void RenderFileBrowser(void) {
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

    // Breadcrumb path bar
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

    // Path edit mode
    if (s_pathEditMode) {
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

    // Folder history dropdown
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::BeginCombo("##snHistCombo", "History")) {
        for (int i = 0; i < (int)s_folderHistory.size(); i++) {
            const char* slash = strrchr(s_folderHistory[i].c_str(), '\\');
            const char* folderName = slash ? slash + 1 : s_folderHistory[i].c_str();
            bool selected = false;
            if (ImGui::Selectable(folderName, selected)) {
                NavigateTo(s_folderHistory[i].c_str());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s_folderHistory[i].c_str());
        }
        ImGui::EndCombo();
    }

    // Filter
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("##snFileFilter", "Filter...", s_fileBrowserFilter, sizeof(s_fileBrowserFilter));

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
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            s_selectedFileIndex = i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                if (entry.isDirectory) {
                    if (entry.name == "..") NavToParent();
                    else NavigateTo(entry.fullPath.c_str());
                } else {
                    if (LoadVGMFile(entry.fullPath.c_str()) && s_connected) StartVGMPlayback();
                }
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
    // Top section: Player + Channel + Registers
    float topHeight = ImGui::GetContentRegionAvail().y * 0.55f;
    ImGui::BeginChild("SN_TopSection", ImVec2(0, topHeight), true);
    RenderPlayerBar();
    RenderChannelControls();
    RenderRegisterTable();
    ImGui::EndChild();

    // Bottom section: File Browser | Log+History
    ImGui::BeginChild("SN_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderFileBrowser();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("SN_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();
}

void Render() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    ImGui::Begin("SN76489(DCSG)");
    ImGui::BeginChild("SN_LeftPane", ImVec2(300, 0), true);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("SN_RightPane", ImVec2(0, 0), false);
    RenderMain();
    ImGui::EndChild();
    ImGui::End();
}

bool WantsKeyboardCapture() { return false; }

} // namespace SN76489Window
