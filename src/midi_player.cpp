// midi_player.cpp - MIDI Parsing and Playback Module Implementation

#include "midi_player.h"
#include "chip_control.h"
#include "config_manager.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <random>
#include <windows.h>

namespace MidiPlayer {

// ===== Global Variable Definitions =====

MidiPlayerState g_midiPlayer;
std::vector<FileEntry> g_fileList;
char g_currentPath[MAX_PATH] = {0};
char g_pathInput[MAX_PATH] = {0};
std::vector<std::string> g_pathHistory;
int g_pathHistoryIndex = -1;
int g_selectedFileIndex = -1;
bool g_pathEditMode = false;
bool g_pathEditModeJustActivated = false;
std::map<std::string, float> g_pathScrollPositions;
std::string g_lastExitedFolder;
std::string g_currentPlayingFilePath;
std::map<int, TextScrollState> g_textScrollStates;
int g_hoveredFileIndex = -1;
int g_currentPlayingIndex = -1;
bool g_isSequentialPlayback = true;
bool g_autoPlayNext = true;
std::vector<std::string> g_midiFolderHistory;
bool g_isWindowDragging = false;
bool g_showTuningWindow = false;
bool g_isInputActive = false;
bool g_enableGlobalMediaKeys = true;
HWND g_mainWindow = NULL;
bool g_enableAutoSkipSilence = true;

static const char* g_midiFolderHistoryFile = "ym2163_config.ini";

// ===== MidiPlayerState Constructor =====

MidiPlayerState::MidiPlayerState()
    : isPlaying(false), isPaused(false), currentTick(0),
      pausedDuration(0), tempo(500000.0),
      ticksPerQuarterNote(120), accumulatedTime(0.0) {
    QueryPerformanceFrequency(&perfCounterFreq);
    QueryPerformanceCounter(&lastPerfCounter);
}

// ===== UTF-8 / Wide String Utilities =====

std::string WideToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring UTF8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

int UTF8CharCount(const std::string& str) {
    int count = 0;
    for (size_t i = 0; i < str.size(); ) {
        unsigned char c = str[i];
        if      (c < 0x80) i += 1;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else               i += 4;
        count++;
    }
    return count;
}

size_t UTF8CharOffset(const std::string& str, int charIndex) {
    size_t offset = 0;
    for (int i = 0; i < charIndex && offset < str.size(); i++) {
        unsigned char c = str[offset];
        if      (c < 0x80) offset += 1;
        else if (c < 0xE0) offset += 2;
        else if (c < 0xF0) offset += 3;
        else               offset += 4;
    }
    return offset;
}

std::string TruncateFolderName(const std::string& name, int maxLength) {
    int charCount = UTF8CharCount(name);
    if (charCount <= maxLength) return name;
    int sideLength = (maxLength - 3) / 2;
    size_t prefixEnd   = UTF8CharOffset(name, sideLength);
    size_t suffixStart = UTF8CharOffset(name, charCount - sideLength);
    return name.substr(0, prefixEnd) + "..." + name.substr(suffixStart);
}

// ===== File Browser =====

void RefreshFileList() {
    g_fileList.clear();
    g_selectedFileIndex = -1;

    if (strlen(g_currentPath) > 3) {
        FileEntry parent;
        parent.name = "..";
        parent.fullPath = "";
        parent.isDirectory = true;
        g_fileList.push_back(parent);
    }

    std::wstring wCurrentPath = UTF8ToWide(g_currentPath);
    std::wstring wSearchPath = wCurrentPath + L"\\*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(wSearchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                continue;
            FileEntry entry;
            entry.name = WideToUTF8(findData.cFileName);
            std::wstring wFullPath = wCurrentPath + L"\\" + findData.cFileName;
            entry.fullPath = WideToUTF8(wFullPath);
            entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (entry.isDirectory) {
                g_fileList.push_back(entry);
            } else {
                const wchar_t* ext = wcsrchr(findData.cFileName, L'.');
                if (ext && (_wcsicmp(ext, L".mid") == 0 || _wcsicmp(ext, L".midi") == 0))
                    g_fileList.push_back(entry);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    std::sort(g_fileList.begin(), g_fileList.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return a.name < b.name;
    });
}

void NavigateToPath(const char* path) {
    std::wstring wPath = UTF8ToWide(path);
    wchar_t wNormalizedPath[MAX_PATH];
    if (GetFullPathNameW(wPath.c_str(), MAX_PATH, wNormalizedPath, NULL) == 0) {
        YM2163::log_command("ERROR: Invalid path: %s", path);
        return;
    }
    DWORD attr = GetFileAttributesW(wNormalizedPath);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        YM2163::log_command("ERROR: Path does not exist: %s", path);
        return;
    }
    std::string normalizedPath = WideToUTF8(wNormalizedPath);
    strcpy(g_currentPath, normalizedPath.c_str());
    strcpy(g_pathInput,   normalizedPath.c_str());
    if (g_pathHistoryIndex < (int)g_pathHistory.size() - 1)
        g_pathHistory.erase(g_pathHistory.begin() + g_pathHistoryIndex + 1, g_pathHistory.end());
    g_pathHistory.push_back(normalizedPath);
    g_pathHistoryIndex = (int)g_pathHistory.size() - 1;
    RefreshFileList();
    YM2163::log_command("Navigated to: %s", normalizedPath.c_str());
    AddToMIDIFolderHistory(normalizedPath.c_str());
}

void NavigateBack() {
    if (g_pathHistoryIndex > 0) {
        g_pathHistoryIndex--;
        strcpy(g_currentPath, g_pathHistory[g_pathHistoryIndex].c_str());
        strcpy(g_pathInput, g_currentPath);
        RefreshFileList();
    }
}

void NavigateForward() {
    if (g_pathHistoryIndex < (int)g_pathHistory.size() - 1) {
        g_pathHistoryIndex++;
        strcpy(g_currentPath, g_pathHistory[g_pathHistoryIndex].c_str());
        strcpy(g_pathInput, g_currentPath);
        RefreshFileList();
    }
}

void NavigateToParent() {
    char parentPath[MAX_PATH];
    strcpy(parentPath, g_currentPath);
    size_t len = strlen(parentPath);
    if (len > 0 && parentPath[len - 1] == '\\') { parentPath[--len] = '\0'; }
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        g_lastExitedFolder = std::string(lastSlash + 1);
        *lastSlash = '\0';
        NavigateToPath(parentPath);
    }
}

std::vector<std::string> SplitPath(const char* path) {
    std::vector<std::string> segments;
    std::string pathStr(path);
    if (pathStr.length() >= 2 && pathStr[1] == ':') {
        segments.push_back(pathStr.substr(0, 3));
        pathStr = pathStr.substr(3);
    }
    size_t start = 0, end = 0;
    while ((end = pathStr.find('\\', start)) != std::string::npos) {
        if (end > start) segments.push_back(pathStr.substr(start, end - start));
        start = end + 1;
    }
    if (start < pathStr.length()) segments.push_back(pathStr.substr(start));
    return segments;
}

// ===== MIDI Folder History =====

bool ContainsMIDIFiles(const char* folderPath) {
    WIN32_FIND_DATAW findData;
    std::wstring wPath = UTF8ToWide(folderPath);
    std::wstring searchPath = wPath + L"\\*.mid";
    HANDLE findHandle = FindFirstFileW(searchPath.c_str(), &findData);
    if (findHandle != INVALID_HANDLE_VALUE) { FindClose(findHandle); return true; }
    searchPath = wPath + L"\\*.midi";
    findHandle = FindFirstFileW(searchPath.c_str(), &findData);
    if (findHandle != INVALID_HANDLE_VALUE) { FindClose(findHandle); return true; }
    return false;
}

void AddToMIDIFolderHistory(const char* folderPath) {
    if (!folderPath || strlen(folderPath) == 0) return;
    if (!ContainsMIDIFiles(folderPath)) return;
    for (auto it = g_midiFolderHistory.begin(); it != g_midiFolderHistory.end(); ++it) {
        if (*it == folderPath) { g_midiFolderHistory.erase(it); break; }
    }
    g_midiFolderHistory.insert(g_midiFolderHistory.begin(), folderPath);
    if (g_midiFolderHistory.size() > 20) g_midiFolderHistory.pop_back();
    SaveMIDIFolderHistory();
}

void SaveMIDIFolderHistory() {
    char iniPath[MAX_PATH];
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    snprintf(iniPath, MAX_PATH, "%s%s", exePath, g_midiFolderHistoryFile);
    // Clear existing keys before writing
    WritePrivateProfileStringA("MidiFolderHistory", NULL, NULL, iniPath);
    for (int i = 0; i < (int)g_midiFolderHistory.size(); i++) {
        char key[16];
        sprintf(key, "Folder%d", i);
        WritePrivateProfileStringA("MidiFolderHistory", key, g_midiFolderHistory[i].c_str(), iniPath);
    }
    WritePrivateProfileStringA("MidiFolderHistory", "Count", std::to_string(g_midiFolderHistory.size()).c_str(), iniPath);
}

void LoadMIDIFolderHistory() {
    char iniPath[MAX_PATH];
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    snprintf(iniPath, MAX_PATH, "%s%s", exePath, g_midiFolderHistoryFile);
    int count = GetPrivateProfileIntA("MidiFolderHistory", "Count", 0, iniPath);
    for (int i = 0; i < count; i++) {
        char key[16];
        char line[MAX_PATH];
        sprintf(key, "Folder%d", i);
        GetPrivateProfileStringA("MidiFolderHistory", key, "", line, MAX_PATH, iniPath);
        if (strlen(line) > 0) {
            WIN32_FIND_DATAW findData;
            std::wstring wPath = UTF8ToWide(line);
            HANDLE findHandle = FindFirstFileW(wPath.c_str(), &findData);
            if (findHandle != INVALID_HANDLE_VALUE) {
                FindClose(findHandle);
                if (ContainsMIDIFiles(line))
                    g_midiFolderHistory.push_back(line);
            }
        }
    }
}

void ClearMIDIFolderHistory() {
    g_midiFolderHistory.clear();
    SaveMIDIFolderHistory();
}

void RemoveMIDIFolderHistoryEntry(int index) {
    if (index >= 0 && index < (int)g_midiFolderHistory.size()) {
        g_midiFolderHistory.erase(g_midiFolderHistory.begin() + index);
        SaveMIDIFolderHistory();
    }
}

void InitializeFileBrowser() {
    LoadMIDIFolderHistory();
    wchar_t wExePath[MAX_PATH];
    GetModuleFileNameW(NULL, wExePath, MAX_PATH);
    wchar_t* lastSlashW = wcsrchr(wExePath, L'\\');
    if (lastSlashW) *lastSlashW = L'\0';
    std::string exePath = WideToUTF8(wExePath);
    NavigateToPath(exePath.c_str());
}

// ===== Global Media Keys =====

void RegisterGlobalMediaKeys() {
    if (!g_mainWindow) return;
    bool success = true;
    if (!RegisterHotKey(g_mainWindow, HK_PLAY_PAUSE, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE)) {
        YM2163::log_command("Warning: Failed to register Play/Pause media key"); success = false;
    }
    if (!RegisterHotKey(g_mainWindow, HK_NEXT_TRACK, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK)) {
        YM2163::log_command("Warning: Failed to register Next Track media key"); success = false;
    }
    if (!RegisterHotKey(g_mainWindow, HK_PREV_TRACK, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK)) {
        YM2163::log_command("Warning: Failed to register Previous Track media key"); success = false;
    }
    if (success) YM2163::log_command("Global media keys registered successfully");
}

void UnregisterGlobalMediaKeys() {
    if (!g_mainWindow) return;
    UnregisterHotKey(g_mainWindow, HK_PLAY_PAUSE);
    UnregisterHotKey(g_mainWindow, HK_NEXT_TRACK);
    UnregisterHotKey(g_mainWindow, HK_PREV_TRACK);
    YM2163::log_command("Global media keys unregistered");
}

// ===== MIDI File Loading =====

static void AnalyzeVelocityDistribution() {
    YM2163::g_velocityAnalysis.reset();
    if (!g_midiPlayer.midiFile.status()) {
        YM2163::log_command("No MIDI file loaded for velocity analysis");
        return;
    }
    for (int track = 0; track < g_midiPlayer.midiFile.getTrackCount(); track++) {
        for (int evt = 0; evt < g_midiPlayer.midiFile[track].getEventCount(); evt++) {
            MidiEvent& midiEvent = g_midiPlayer.midiFile[track][evt];
            if (midiEvent.isNoteOn()) {
                int velocity = midiEvent.getVelocity();
                if (velocity > 0) {
                    YM2163::g_velocityAnalysis.velocityHistogram[velocity]++;
                    YM2163::g_velocityAnalysis.totalNotes++;
                    if (velocity < YM2163::g_velocityAnalysis.minVelocity) YM2163::g_velocityAnalysis.minVelocity = velocity;
                    if (velocity > YM2163::g_velocityAnalysis.maxVelocity) YM2163::g_velocityAnalysis.maxVelocity = velocity;
                }
            }
        }
    }
    if (YM2163::g_velocityAnalysis.totalNotes == 0) {
        YM2163::log_command("No notes found in MIDI file"); return;
    }
    long long sum = 0;
    for (int i = 0; i < 128; i++) sum += i * YM2163::g_velocityAnalysis.velocityHistogram[i];
    YM2163::g_velocityAnalysis.avgVelocity = (float)sum / YM2163::g_velocityAnalysis.totalNotes;

    // Find peak and two most common velocities
    int peak = 0, peakCount = 0;
    for (int i = 0; i < 128; i++) {
        if (YM2163::g_velocityAnalysis.velocityHistogram[i] > peakCount) {
            peakCount = YM2163::g_velocityAnalysis.velocityHistogram[i]; peak = i;
        }
    }
    YM2163::g_velocityAnalysis.peakVelocity = peak;
    int vel1 = peak, vel2 = 0, vel2Count = 0;
    for (int i = 0; i < 128; i++) {
        if (i != vel1 && YM2163::g_velocityAnalysis.velocityHistogram[i] > vel2Count) {
            vel2Count = YM2163::g_velocityAnalysis.velocityHistogram[i]; vel2 = i;
        }
    }
    if (vel1 < vel2) { int tmp = vel1; vel1 = vel2; vel2 = tmp; }
    YM2163::g_velocityAnalysis.mostCommonVelocity1 = vel1;
    YM2163::g_velocityAnalysis.mostCommonVelocity2 = vel2;

    YM2163::g_velocityAnalysis.threshold_0dB   = YM2163::g_velocityAnalysis.peakVelocity;
    YM2163::g_velocityAnalysis.threshold_6dB   = (vel1 + vel2) / 2;
    YM2163::g_velocityAnalysis.threshold_12dB  = vel2 - (vel1 - vel2) / 2;
    YM2163::g_velocityAnalysis.threshold_mute  = (int)(YM2163::g_velocityAnalysis.avgVelocity * 0.15f);

    if (YM2163::g_velocityAnalysis.threshold_mute  < 1)  YM2163::g_velocityAnalysis.threshold_mute  = 1;
    if (YM2163::g_velocityAnalysis.threshold_12dB  < 20) YM2163::g_velocityAnalysis.threshold_12dB  = 20;
    if (YM2163::g_velocityAnalysis.threshold_6dB   < 40) YM2163::g_velocityAnalysis.threshold_6dB   = 40;
    if (YM2163::g_velocityAnalysis.threshold_0dB   < 90) YM2163::g_velocityAnalysis.threshold_0dB   = 90;
    if (YM2163::g_velocityAnalysis.threshold_12dB <= YM2163::g_velocityAnalysis.threshold_mute)
        YM2163::g_velocityAnalysis.threshold_12dB = YM2163::g_velocityAnalysis.threshold_mute + 10;
    if (YM2163::g_velocityAnalysis.threshold_6dB  <= YM2163::g_velocityAnalysis.threshold_12dB)
        YM2163::g_velocityAnalysis.threshold_6dB  = YM2163::g_velocityAnalysis.threshold_12dB + 10;
    if (YM2163::g_velocityAnalysis.threshold_0dB  <= YM2163::g_velocityAnalysis.threshold_6dB)
        YM2163::g_velocityAnalysis.threshold_0dB  = YM2163::g_velocityAnalysis.threshold_6dB + 10;

    YM2163::log_command("Velocity analysis: avg=%.1f peak=%d thresholds: 0dB=%d -6dB=%d -12dB=%d mute=%d",
        YM2163::g_velocityAnalysis.avgVelocity, YM2163::g_velocityAnalysis.peakVelocity,
        YM2163::g_velocityAnalysis.threshold_0dB, YM2163::g_velocityAnalysis.threshold_6dB,
        YM2163::g_velocityAnalysis.threshold_12dB, YM2163::g_velocityAnalysis.threshold_mute);
}

bool LoadMIDIFile(const char* filename) {
    g_midiPlayer.midiFile.clear();

    std::wstring wFilename = UTF8ToWide(filename);

#ifdef _WIN32
    if (wFilename.length() > 260) {
        if (wFilename.find(L"\\\\?\\") != 0) {
            wFilename = L"\\\\?\\" + wFilename;
        }
    }

    if (!g_midiPlayer.midiFile.read(wFilename)) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            YM2163::log_command("ERROR: File not found: %s", filename);
        else if (error == ERROR_PATH_NOT_FOUND)
            YM2163::log_command("ERROR: Path not found: %s", filename);
        else if (error == ERROR_ACCESS_DENIED)
            YM2163::log_command("ERROR: Access denied: %s", filename);
        else
            YM2163::log_command("ERROR: Failed to load MIDI file (error %d): %s", error, filename);
        return false;
    }
#else
    if (!g_midiPlayer.midiFile.read(filename)) {
        YM2163::log_command("ERROR: Failed to load MIDI file: %s", filename);
        return false;
    }
#endif
    g_midiPlayer.currentFileName = filename;
    g_midiPlayer.currentTick = 0;
    g_midiPlayer.isPlaying = false;
    g_midiPlayer.isPaused = false;
    g_midiPlayer.ticksPerQuarterNote = g_midiPlayer.midiFile.getTicksPerQuarterNote();
    g_midiPlayer.tempo = 500000.0;
    g_midiPlayer.activeNotes.clear();
    YM2163::ResetPianoKeyStates();
    YM2163::g_sustainPedalActive = false;
    g_midiPlayer.midiFile.makeAbsoluteTicks();
    g_midiPlayer.midiFile.joinTracks();
    int numEvents = g_midiPlayer.midiFile.getEventCount(0);
    YM2163::log_command("=== MIDI File Loaded ===");
    YM2163::log_command("File: %s", filename);
    YM2163::log_command("Events: %d", numEvents);
    YM2163::log_command("TPQ: %d", g_midiPlayer.ticksPerQuarterNote);
    if (YM2163::g_enableDynamicVelocityMapping) AnalyzeVelocityDistribution();
    return true;
}

// ===== MIDI Playback Helpers =====

int FindFirstNoteEvent(int& outTick) {
    outTick = 0;
    if (g_midiPlayer.currentFileName.empty()) return 0;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return 0;
    MidiEventList& track = g_midiPlayer.midiFile[0];
    for (int i = 0; i < (int)track.size(); i++) {
        MidiEvent& event = track[i];
        if (event.isNoteOn() && event.getVelocity() > 0 && event.getChannel() != 9) {
            outTick = event.tick;
            YM2163::log_command("First note found at event index: %d, tick: %d", i, event.tick);
            return i;
        }
    }
    return 0;
}

double GetMIDITotalDuration() {
    if (g_midiPlayer.currentFileName.empty()) return 0.0;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return 0.0;
    MidiEventList& track = g_midiPlayer.midiFile[0];
    if (track.size() == 0) return 0.0;
    int lastTick = track[track.size() - 1].tick;
    double microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;
    return (double)lastTick * microsPerTick;
}

std::string FormatTime(double microseconds) {
    int totalSeconds = (int)(microseconds / 1000000.0);
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
    return std::string(buffer);
}

// ===== Playlist Navigation =====

int GetNextMIDIFileIndex() {
    std::vector<int> midiIndices;
    for (int i = 0; i < (int)g_fileList.size(); i++)
        if (!g_fileList[i].isDirectory && !g_fileList[i].fullPath.empty())
            midiIndices.push_back(i);
    if (midiIndices.empty()) return -1;
    if (g_isSequentialPlayback) {
        if (g_currentPlayingIndex < 0) return midiIndices[0];
        auto it = std::find(midiIndices.begin(), midiIndices.end(), g_currentPlayingIndex);
        if (it != midiIndices.end()) {
            it++;
            if (it == midiIndices.end()) return midiIndices[0];
            return *it;
        }
        return midiIndices[0];
    } else {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        if (midiIndices.size() == 1) return midiIndices[0];
        std::vector<int> candidates;
        for (int idx : midiIndices)
            if (idx != g_currentPlayingIndex) candidates.push_back(idx);
        if (candidates.empty()) return midiIndices[0];
        std::uniform_int_distribution<> dis(0, (int)candidates.size() - 1);
        return candidates[dis(gen)];
    }
}

int GetPreviousMIDIFileIndex() {
    std::vector<int> midiIndices;
    for (int i = 0; i < (int)g_fileList.size(); i++)
        if (!g_fileList[i].isDirectory && !g_fileList[i].fullPath.empty())
            midiIndices.push_back(i);
    if (midiIndices.empty()) return -1;
    if (g_isSequentialPlayback) {
        if (g_currentPlayingIndex < 0) return midiIndices.empty() ? -1 : midiIndices.back();
        auto it = std::find(midiIndices.begin(), midiIndices.end(), g_currentPlayingIndex);
        if (it != midiIndices.end()) {
            if (it == midiIndices.begin()) return midiIndices.back();
            it--; return *it;
        }
        return midiIndices.back();
    } else {
        return GetNextMIDIFileIndex();
    }
}

void PlayNextMIDI() {
    int nextIndex = GetNextMIDIFileIndex();
    if (nextIndex >= 0 && nextIndex < (int)g_fileList.size()) {
        g_currentPlayingIndex = nextIndex;
        g_selectedFileIndex = nextIndex;
        YM2163::ResetAllYM2163Chips();
        YM2163::InitializeAllChannels();
        YM2163::stop_all_notes();
        g_midiPlayer.activeNotes.clear();
        YM2163::ResetPianoKeyStates();
        if (LoadMIDIFile(g_fileList[nextIndex].fullPath.c_str())) {
            g_midiPlayer.currentTick = 0;
            g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
            PlayMIDI();
        }
    }
}

void PlayPreviousMIDI() {
    int prevIndex = GetPreviousMIDIFileIndex();
    if (prevIndex >= 0 && prevIndex < (int)g_fileList.size()) {
        g_currentPlayingIndex = prevIndex;
        g_selectedFileIndex = prevIndex;
        YM2163::ResetAllYM2163Chips();
        YM2163::InitializeAllChannels();
        YM2163::stop_all_notes();
        g_midiPlayer.activeNotes.clear();
        YM2163::ResetPianoKeyStates();
        if (LoadMIDIFile(g_fileList[prevIndex].fullPath.c_str())) {
            g_midiPlayer.currentTick = 0;
            g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
            PlayMIDI();
        }
    }
}

// ===== MIDI Playback Control =====

void PlayMIDI() {
    if (g_midiPlayer.currentFileName.empty()) return;
    if (g_midiPlayer.isPaused) {
        g_midiPlayer.isPaused = false;
        ::g_midiUserPaused = false;
        auto now = std::chrono::steady_clock::now();
        g_midiPlayer.pausedDuration += std::chrono::duration_cast<std::chrono::milliseconds>(now - g_midiPlayer.pauseTime);
        QueryPerformanceCounter(&g_midiPlayer.lastPerfCounter);
        YM2163::log_command("MIDI playback resumed");
    } else {
        g_midiPlayer.currentTick = 0;
        g_midiPlayer.isPlaying = true;
        g_midiPlayer.playStartTime = std::chrono::steady_clock::now();
        g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
        YM2163::stop_all_notes();
        g_midiPlayer.activeNotes.clear();
        YM2163::ResetPianoKeyStates();
        YM2163::g_sustainPedalActive = false;

        if (g_enableAutoSkipSilence) {
            int firstNoteTick = 0;
            int firstNoteIndex = FindFirstNoteEvent(firstNoteTick);
            if (firstNoteIndex > 0) {
                MidiEventList& track = g_midiPlayer.midiFile[0];
                for (int i = 0; i < firstNoteIndex; i++) {
                    MidiEvent& event = track[i];
                    if (event.isTempo()) {
                        g_midiPlayer.tempo = event.getTempoMicroseconds();
                    } else if (event.isController()) {
                        int controller = event[1];
                        int value = event[2];
                        if (controller == 64 && YM2163::g_enableSustainPedal)
                            YM2163::g_sustainPedalActive = (value >= 64);
                    }
                }
                g_midiPlayer.currentTick = firstNoteIndex;
                double microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;
                g_midiPlayer.accumulatedTime = (double)firstNoteTick * microsPerTick;
                YM2163::log_command("Auto-skipped to event %d (MIDI tick: %d, time: %.2f ms)",
                    firstNoteIndex, firstNoteTick, g_midiPlayer.accumulatedTime / 1000.0);
            } else {
                g_midiPlayer.accumulatedTime = 0.0;
            }
        } else {
            g_midiPlayer.accumulatedTime = 0.0;
        }
        QueryPerformanceCounter(&g_midiPlayer.lastPerfCounter);
        QueryPerformanceFrequency(&g_midiPlayer.perfCounterFreq);
        YM2163::log_command("MIDI playback started");
    }
    g_midiPlayer.isPlaying = true;
}

void PauseMIDI() {
    if (!g_midiPlayer.isPlaying || g_midiPlayer.isPaused) return;
    g_midiPlayer.isPaused = true;
    ::g_midiUserPaused = true;
    g_midiPlayer.pauseTime = std::chrono::steady_clock::now();
    YM2163::stop_all_notes();
    YM2163::log_command("MIDI playback paused");
}

void StopMIDI() {
    ::g_midiUserPaused = true;
    g_midiPlayer.isPlaying = false;
    g_midiPlayer.isPaused = false;
    g_midiPlayer.currentTick = 0;
    YM2163::stop_all_notes();
    g_midiPlayer.activeNotes.clear();
    YM2163::ResetPianoKeyStates();
    YM2163::g_sustainPedalActive = false;
    YM2163::ResetAllYM2163Chips();
    YM2163::InitializeAllChannels();
    YM2163::log_command("MIDI playback stopped");
}

// ===== Seek / Rebuild =====

void RebuildActiveNotesAfterSeek(int targetTick) {
    if (g_midiPlayer.currentFileName.empty()) return;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return;

    std::map<int, std::map<int, bool>> notesOn;
    MidiEventList& track = g_midiPlayer.midiFile[0];

    for (int i = 0; i < (int)track.size() && i < targetTick; i++) {
        MidiEvent& event = track[i];
        if (event.isNoteOn()) {
            int channel = event.getChannel();
            int note = event.getKeyNumber();
            int velocity = event.getVelocity();
            if (channel == 9) continue;
            notesOn[channel][note] = (velocity > 0);
        } else if (event.isNoteOff()) {
            int channel = event.getChannel();
            int note = event.getKeyNumber();
            notesOn[channel][note] = false;
        }
    }

    for (auto& channelPair : notesOn) {
        int channel = channelPair.first;
        for (auto& notePair : channelPair.second) {
            int note = notePair.first;
            bool isOn = notePair.second;
            if (!isOn) continue;

            int ymNote   = note % 12;
            int ymOctave = (note / 12) - 2;
            while (ymOctave < 0 || (ymOctave == 0 && ymNote < 11)) ymOctave++;
            if (YM2163::g_enableSlot3_2MHz) {
                while (ymOctave > 6) ymOctave--;
            } else {
                while (ymOctave > 5 || (ymOctave == 5 && ymNote > 11)) ymOctave--;
            }

            bool isHighOctave = (YM2163::g_slot3_2MHz_Range == 0) ? (ymOctave == 6) : (ymOctave >= 5 && ymOctave <= 6);
            int ymChannel;
            if (isHighOctave && YM2163::g_enableSlot3_2MHz)
                ymChannel = YM2163::find_free_channel_slot3();
            else
                ymChannel = YM2163::find_free_channel();

            if (ymChannel >= 0) {
                int useWave = YM2163::g_currentTimbre;
                int useEnvelope = YM2163::g_currentEnvelope;
                int useVolume = YM2163::g_currentVolume;
                if (!YM2163::g_useLiveControl && Config::g_instrumentConfigs.count(0) > 0) {
                    Config::InstrumentConfig& cfg = Config::g_instrumentConfigs[0];
                    useWave = cfg.wave;
                    useEnvelope = cfg.envelope;
                }
                YM2163::play_note(ymChannel, ymNote, ymOctave, useWave, useEnvelope, useVolume);
                g_midiPlayer.activeNotes[channel][note] = ymChannel;
            }
        }
    }
}

// ===== Main MIDI Update Loop =====

void UpdateMIDIPlayback() {
    if (!g_midiPlayer.isPlaying || g_midiPlayer.isPaused) return;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return;

    MidiEventList& track = g_midiPlayer.midiFile[0];

    // Advance time using high-precision counter
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMicros = (double)(now.QuadPart - g_midiPlayer.lastPerfCounter.QuadPart)
                          / (double)g_midiPlayer.perfCounterFreq.QuadPart * 1000000.0;
    g_midiPlayer.lastPerfCounter = now;
    g_midiPlayer.accumulatedTime += elapsedMicros;

    double microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;

    while (g_midiPlayer.currentTick < (int)track.size()) {
        MidiEvent& event = track[g_midiPlayer.currentTick];
        double eventTime = (double)event.tick * microsPerTick;
        if (g_midiPlayer.accumulatedTime < eventTime) break;

        if (event.isNoteOn()) {
            int channel  = event.getChannel();
            int note     = event.getKeyNumber();
            int velocity = event.getVelocity();

            if (channel == 9) {
                // Drum channel
                if (velocity > 0 && Config::g_drumConfigs.count(note) > 0) {
                    Config::DrumConfig& drum = Config::g_drumConfigs[note];
                    for (uint8_t drumBits : drum.drumBits)
                        YM2163::play_drum(drumBits);
                }
            } else if (velocity > 0) {
                // Note on
                int ymNote   = note % 12;
                int ymOctave = (note / 12) - 2;
                while (ymOctave < 0 || (ymOctave == 0 && ymNote < 11)) ymOctave++;
                if (YM2163::g_enableSlot3_2MHz) {
                    while (ymOctave > 6) ymOctave--;
                } else {
                    while (ymOctave > 5 || (ymOctave == 5 && ymNote > 11)) ymOctave--;
                }

                bool isHighOctave = (YM2163::g_slot3_2MHz_Range == 0) ? (ymOctave == 6) : (ymOctave >= 5 && ymOctave <= 6);
                int ymChannel;
                if (isHighOctave && YM2163::g_enableSlot3_2MHz) {
                    ymChannel = YM2163::find_free_channel_slot3();
                } else if (ymOctave >= 2 && ymOctave <= 5 && YM2163::g_enableSlot3_2MHz
                           && YM2163::g_enableSlot3Overflow && YM2163::getNormalChannelUsage() >= 0.8f) {
                    ymChannel = YM2163::find_free_channel_slot3();
                    if (ymChannel < 0) ymChannel = YM2163::find_free_channel();
                } else {
                    ymChannel = YM2163::find_free_channel();
                }

                if (ymChannel >= 0) {
                    int useWave = YM2163::g_currentTimbre;
                    int useEnvelope = YM2163::g_currentEnvelope;
                    int useVolume = YM2163::g_currentVolume;
                    int usePedalMode = YM2163::g_pedalMode;

                    if (!YM2163::g_useLiveControl) {
                        int program = 0;
                        if (Config::g_instrumentConfigs.count(program) > 0) {
                            Config::InstrumentConfig& cfg = Config::g_instrumentConfigs[program];
                            useWave = cfg.wave;
                            useEnvelope = cfg.envelope;
                            if (cfg.pedalMode != 0) usePedalMode = cfg.pedalMode;
                        }
                    }

                    if (YM2163::g_enableSustainPedal && YM2163::g_sustainPedalActive) {
                        if (usePedalMode == 1) useEnvelope = 1;  // Piano: Fast
                        else if (usePedalMode == 2) useEnvelope = 3;  // Organ: Slow
                    }

                    useVolume = YM2163::map_velocity_to_volume(velocity);
                    YM2163::play_note(ymChannel, ymNote, ymOctave, useWave, useEnvelope, useVolume);
                    g_midiPlayer.activeNotes[channel][note] = ymChannel;
                }
            } else {
                // velocity 0 = note off
                if (g_midiPlayer.activeNotes[channel].count(note) > 0) {
                    int ymChannel = g_midiPlayer.activeNotes[channel][note];
                    YM2163::stop_note(ymChannel);
                    g_midiPlayer.activeNotes[channel].erase(note);
                }
            }
        } else if (event.isNoteOff()) {
            int channel = event.getChannel();
            int note    = event.getKeyNumber();
            if (g_midiPlayer.activeNotes[channel].count(note) > 0) {
                int ymChannel = g_midiPlayer.activeNotes[channel][note];
                YM2163::stop_note(ymChannel);
                g_midiPlayer.activeNotes[channel].erase(note);
            }
        } else if (event.isTempo()) {
            g_midiPlayer.tempo = event.getTempoMicroseconds();
            microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;
        } else if (event.isController()) {
            int controller = event[1];
            int value      = event[2];
            if (controller == 64 && YM2163::g_pedalMode > 0)
                YM2163::g_sustainPedalActive = (value >= 64);
        }

        g_midiPlayer.currentTick++;
    }

    if (g_midiPlayer.currentTick >= (int)track.size()) {
        StopMIDI();
        YM2163::log_command("MIDI playback finished");
        if (g_autoPlayNext) PlayNextMIDI();
    }
}

} // namespace MidiPlayer







