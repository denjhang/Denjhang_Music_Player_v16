// midi_player.h - MIDI Parsing and Playback Module
// Handles MIDI file loading, playback, file browser, and folder history

#ifndef MIDI_PLAYER_H
#define MIDI_PLAYER_H

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <windows.h>
#include "midifile/include/MidiFile.h"

namespace MidiPlayer {

using smf::MidiFile;
using smf::MidiEvent;
using smf::MidiEventList;

// ===== File Browser Types =====
struct FileEntry {
    std::string name;
    std::string fullPath;
    bool isDirectory;
};

struct TextScrollState {
    float scrollOffset;
    float scrollDirection;
    float pauseTimer;
    std::chrono::steady_clock::time_point lastUpdateTime;
};

// ===== File Browser State =====
// Each tab (YM2163, OPL3, VGM) holds its own independent instance.
struct FileBrowserState {
    std::vector<FileEntry> fileList;
    char currentPath[MAX_PATH];
    char pathInput[MAX_PATH];
    std::vector<std::string> pathHistory;  // back/forward nav stack
    int pathHistoryIndex;
    int selectedFileIndex;
    bool pathEditMode;
    bool pathEditModeJustActivated;
    std::map<std::string, float> pathScrollPositions;
    std::string lastExitedFolder;
    std::string currentPlayingFilePath;
    std::map<int, TextScrollState> textScrollStates;
    int hoveredFileIndex;
    int currentPlayingIndex;
    bool isSequentialPlayback;
    bool autoPlayNext;
    std::vector<std::string> folderHistory;  // recent folders dropdown
    const char* historyFile;  // persistence filename (set once at init)
    bool initialized;

    FileBrowserState()
        : pathHistoryIndex(-1), selectedFileIndex(-1),
          pathEditMode(false), pathEditModeJustActivated(false),
          hoveredFileIndex(-1), currentPlayingIndex(-1),
          isSequentialPlayback(true), autoPlayNext(true),
          historyFile(nullptr), initialized(false)
    {
        currentPath[0] = 0;
        pathInput[0] = 0;
    }
};

// ===== MIDI Player State =====
struct MidiPlayerState {
    MidiFile midiFile;
    std::string currentFileName;
    bool isPlaying;
    bool isPaused;
    int currentTick;
    std::chrono::steady_clock::time_point playStartTime;
    std::chrono::steady_clock::time_point pauseTime;
    std::chrono::milliseconds pausedDuration;
    double tempo;
    int ticksPerQuarterNote;

    LARGE_INTEGER perfCounterFreq;
    LARGE_INTEGER lastPerfCounter;
    double accumulatedTime;

    std::map<int, std::map<int, int>> activeNotes;  // channel -> note -> YM2163 channel

    MidiPlayerState();
};

// ===== Global State =====
extern MidiPlayerState g_midiPlayer;

extern std::vector<FileEntry> g_fileList;
extern char g_currentPath[MAX_PATH];
extern char g_pathInput[MAX_PATH];
extern std::vector<std::string> g_pathHistory;
extern int g_pathHistoryIndex;
extern int g_selectedFileIndex;
extern bool g_pathEditMode;
extern bool g_pathEditModeJustActivated;
extern std::map<std::string, float> g_pathScrollPositions;
extern std::string g_lastExitedFolder;
extern std::string g_currentPlayingFilePath;
extern std::map<int, TextScrollState> g_textScrollStates;
extern int g_hoveredFileIndex;
extern int g_currentPlayingIndex;
extern bool g_isSequentialPlayback;
extern bool g_autoPlayNext;
extern std::vector<std::string> g_midiFolderHistory;
extern bool g_isWindowDragging;
extern bool g_showTuningWindow;
extern bool g_isInputActive;
extern bool g_enableGlobalMediaKeys;
extern HWND g_mainWindow;
extern bool g_enableAutoSkipSilence;

// Hot key IDs
#define HK_PLAY_PAUSE  1001
#define HK_NEXT_TRACK  1002
#define HK_PREV_TRACK  1003

// Timer ID
#define TIMER_MIDI_UPDATE 1

// ===== Utility Functions =====
std::string WideToUTF8(const std::wstring& wstr);
std::wstring UTF8ToWide(const std::string& str);
int UTF8CharCount(const std::string& str);
size_t UTF8CharOffset(const std::string& str, int charIndex);
std::string TruncateFolderName(const std::string& name, int maxLength = 20);

// ===== File Browser =====
void RefreshFileList();
void NavigateToPath(const char* path);
void NavigateBack();
void NavigateForward();
void NavigateToParent();
std::vector<std::string> SplitPath(const char* path);
void InitializeFileBrowser();
bool ContainsMIDIFiles(const char* folderPath);

// ===== Folder History =====
void AddToMIDIFolderHistory(const char* folderPath);
void SaveMIDIFolderHistory();
void LoadMIDIFolderHistory();
void ClearMIDIFolderHistory();
void RemoveMIDIFolderHistoryEntry(int index);

// ===== Media Keys =====
void RegisterGlobalMediaKeys();
void UnregisterGlobalMediaKeys();

// ===== MIDI Playback =====
bool LoadMIDIFile(const char* filename);
void PlayMIDI();
void PauseMIDI();
void StopMIDI();
void UpdateMIDIPlayback();
void RebuildActiveNotesAfterSeek(int targetTick);

// ===== Playlist Navigation =====
int GetNextMIDIFileIndex();
void PlayNextMIDI();
void PlayPreviousMIDI();

// ===== Helpers =====
int FindFirstNoteEvent(int& outTick);
double GetMIDITotalDuration();
std::string FormatTime(double microseconds);

} // namespace MidiPlayer

// Tab-switch auto-pause: defined in main.cpp, used by PauseMIDI/StopMIDI.
extern bool g_midiUserPaused;

#endif // MIDI_PLAYER_H
