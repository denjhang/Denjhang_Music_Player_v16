// YM2163 Piano GUI v11 - ImGui + DirectX 11 + MIDI File Player + Quad YM2163
// Features: 4-octave keyboard, drums, tuning component, MIDI file playback
// Modern UI with ImGui framework
// v10: Support for 4 YM2163 chips (Slot0-Slot3), 16 channels total
// v11: Connect/Disconnect button, auto-detect hardware, C8-B8 octave via Slot3 2MHz

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <random>
#include <mmsystem.h>  // For multimedia timer

extern "C" {
#include "ftdi_driver/ftd2xx.h"
}

// Include midifile library
#include "midifile/include/MidiFile.h"
using namespace smf;

// ===== Global Variables =====

// DirectX 11 globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// FTDI and YM2163
static FT_HANDLE g_ftHandle = NULL;

// UI State
static int g_currentOctave = 2;
static bool g_keyStates[256] = {0};
static std::string g_logBuffer;
static bool g_autoScroll = true;
static char g_logDisplayBuffer[32768] = {0};
static size_t g_lastLogSize = 0;
static bool g_logScrollToBottom = false;

// YM2163 Settings
static int g_currentTimbre = 4;
static int g_currentEnvelope = 1;
static int g_currentVolume = 0;
static bool g_useLiveControl = false;  // true=Live Control Mode, false=Config Mode (default: Config Mode)
static int g_selectedInstrument = 0;  // Currently selected instrument (0-127) for editing
static bool g_enableVelocityMapping = true;  // Map MIDI velocity to 4-level volume
static bool g_enableDynamicVelocityMapping = true;  // Dynamic velocity mapping based on MIDI analysis (default: ON)
static bool g_enableSustainPedal = true;  // Map sustain pedal to envelope
static bool g_sustainPedalActive = false;  // Current sustain pedal state
static int g_pedalMode = 0;  // 0=Disabled, 1=Piano Pedal (Fast/Decay), 2=Organ Pedal (Slow/Medium)
static bool g_enableSecondYM2163 = true;  // Enable second YM2163 chip (Slot1) for 8 channels total - DEFAULT ON
static bool g_enableThirdYM2163 = false;  // Enable third YM2163 chip (Slot2) for 12 channels total
static bool g_enableFourthYM2163 = false; // Enable fourth YM2163 chip (Slot3) for 16 channels total

// v11: Hardware connection state
static bool g_hardwareConnected = false;  // Track if FTDI hardware is connected
static bool g_manualDisconnect = false;   // User manually clicked Disconnect - suppress auto-connect
static std::chrono::steady_clock::time_point g_lastHardwareCheck;  // Last time we checked hardware presence
static const int HARDWARE_CHECK_INTERVAL_MS = 500;  // Check hardware every 500ms
static int g_hardwareConnectRetries = 0;  // Retry counter for connection attempts
static const int HARDWARE_MAX_RETRIES = 6;  // Max retries before giving up this cycle

// v11: Slot3 2MHz mode for C8-B8 octave
static bool g_enableSlot3_2MHz = false;  // Enable Slot3 as 2MHz chip for C8-B8 octave
static bool g_enableSlot3Overflow = false;  // Enable overflow of C4-B7 to Slot3 2MHz (default: off)
static int g_slot3_2MHz_Range = 1;  // 0=C8-B8 only, 1=C7-B8 (default: C7-B8)

// Dynamic velocity mapping state
struct VelocityAnalysis {
    int velocityHistogram[128];  // Count of each velocity value
    int totalNotes;
    int minVelocity;
    int maxVelocity;
    float avgVelocity;
    int peakVelocity;
    int mostCommonVelocity1;  // Most frequent velocity
    int mostCommonVelocity2;  // Second most frequent velocity

    // Calculated mapping thresholds
    int threshold_0dB;    // Velocities >= this map to 0dB (max volume)
    int threshold_6dB;    // Velocities >= this map to -6dB
    int threshold_12dB;   // Velocities >= this map to -12dB
    int threshold_mute;   // Velocities < this map to mute

    VelocityAnalysis() {
        memset(velocityHistogram, 0, sizeof(velocityHistogram));
        totalNotes = 0;
        minVelocity = 127;
        maxVelocity = 0;
        avgVelocity = 0.0f;
        peakVelocity = 0;
        mostCommonVelocity1 = 64;
        mostCommonVelocity2 = 80;
        threshold_0dB = 100;
        threshold_6dB = 80;
        threshold_12dB = 60;
        threshold_mute = 20;
    }
};

static VelocityAnalysis g_velocityAnalysis;

static const char* g_timbreNames[] = {
    "", "String", "Organ", "Clarinet", "Piano", "Harpsichord"
};

static const char* g_envelopeNames[] = {
    "Decay", "Fast", "Medium", "Slow"
};

static const char* g_volumeNames[] = {
    "0dB", "-6dB", "-12dB", "Mute"
};

// Channel state
typedef struct {
    int note;
    int octave;
    uint16_t fnum;
    bool active;
    int midiChannel;  // Track which MIDI channel this is assigned to
    int timbre;       // Current timbre (wave) setting
    int envelope;     // Current envelope setting
    int volume;       // Current volume setting
    int chipIndex;    // Which YM2163 chip (0=Slot0, 1=Slot1, 2=Slot2, 3=Slot3)
    std::chrono::steady_clock::time_point startTime;  // When this note started playing
    std::chrono::steady_clock::time_point releaseTime;  // When this note was released (stop_note called)
    bool hasBeenUsed;  // Whether this channel has ever been used
    float currentLevel;  // Current envelope level (0.0 to 1.0) for level meter display
} ChannelState;

// 16 channels total: 4 on each Slot (Slot0-Slot3)
static ChannelState g_channels[16] = {
    {0, 0, 0, false, -1, 0, 0, 0, 0, {}, {}, false, 0.0f},  // Chip 0, Channel 0
    {0, 0, 0, false, -1, 0, 0, 0, 0, {}, {}, false, 0.0f},  // Chip 0, Channel 1
    {0, 0, 0, false, -1, 0, 0, 0, 0, {}, {}, false, 0.0f},  // Chip 0, Channel 2
    {0, 0, 0, false, -1, 0, 0, 0, 0, {}, {}, false, 0.0f},  // Chip 0, Channel 3
    {0, 0, 0, false, -1, 0, 0, 0, 1, {}, {}, false, 0.0f},  // Chip 1, Channel 0
    {0, 0, 0, false, -1, 0, 0, 0, 1, {}, {}, false, 0.0f},  // Chip 1, Channel 1
    {0, 0, 0, false, -1, 0, 0, 0, 1, {}, {}, false, 0.0f},  // Chip 1, Channel 2
    {0, 0, 0, false, -1, 0, 0, 0, 1, {}, {}, false, 0.0f},  // Chip 1, Channel 3
    {0, 0, 0, false, -1, 0, 0, 0, 2, {}, {}, false, 0.0f},  // Chip 2, Channel 0
    {0, 0, 0, false, -1, 0, 0, 0, 2, {}, {}, false, 0.0f},  // Chip 2, Channel 1
    {0, 0, 0, false, -1, 0, 0, 0, 2, {}, {}, false, 0.0f},  // Chip 2, Channel 2
    {0, 0, 0, false, -1, 0, 0, 0, 2, {}, {}, false, 0.0f},  // Chip 2, Channel 3
    {0, 0, 0, false, -1, 0, 0, 0, 3, {}, {}, false, 0.0f},  // Chip 3, Channel 0
    {0, 0, 0, false, -1, 0, 0, 0, 3, {}, {}, false, 0.0f},  // Chip 3, Channel 1
    {0, 0, 0, false, -1, 0, 0, 0, 3, {}, {}, false, 0.0f},  // Chip 3, Channel 2
    {0, 0, 0, false, -1, 0, 0, 0, 3, {}, {}, false, 0.0f}   // Chip 3, Channel 3
};
static int g_nextFIFOChannel = 0;  // FIFO channel allocation index

// Drum pads
static bool g_drumPressed[5] = {false};
static bool g_drumActive[4][5] = {{false}};  // Track drum active state for each chip [chipIndex][drumIndex]
static const char* g_drumNames[] = {"BD", "HC", "SDN", "HHO", "HHD"};
static uint8_t g_drumBits[] = {0x01, 0x02, 0x04, 0x08, 0x10};
static std::chrono::steady_clock::time_point g_drumTriggerTime[4][5];  // [chipIndex][drumIndex]
static int g_currentDrumChip = 0;  // Track which chip to use for next drum (0-3)
static float g_drumLevel[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // Combined drum level for each chip (0.0 to 1.0)

// Piano keys state (61 keys B2-B7 + 12 keys C8-B8 = 73 keys)
static bool g_pianoKeyPressed[73] = {false};
static int g_pianoKeyVelocity[73] = {0};  // Store velocity (0-127) for each key
static bool g_pianoKeyFromKeyboard[73] = {false};  // Track if key was triggered by keyboard (not MIDI)
static bool g_pianoKeyOnSlot3_2MHz[73] = {false};  // v11: Track if key is played on Slot3 2MHz (for green color)

// Frequency tables
static int g_fnums[12] = {
    951, 900, 852, 803, 756, 716, 674, 637, 601, 567, 535, 507
};

static int g_fnum_b2 = 1014;

static int g_fnums_c7[12] = {
    475, 450, 426, 401, 378, 358, 337, 318, 300, 283, 267, 0
};


static const char* g_noteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static const bool g_isBlackNote[12] = {
    false, true, false, true, false, false, true, false, true, false, true, false
};

// INI file paths
static char g_iniFilePath[MAX_PATH] = {0};
static char g_midiConfigPath[MAX_PATH] = {0};

// ===== MIDI Configuration Structures =====

struct InstrumentConfig {
    std::string name;
    int envelope;  // 0=Decay, 1=Fast, 2=Medium, 3=Slow
    int wave;      // 1=String, 2=Organ, 3=Clarinet, 4=Piano, 5=Harpsichord
    int pedalMode; // 0=Disabled, 1=Piano, 2=Organ (optional per-instrument override)
};

struct DrumConfig {
    std::string name;
    std::vector<uint8_t> drumBits;  // Can have multiple drums combined
};

static std::map<int, InstrumentConfig> g_instrumentConfigs;
static std::map<int, DrumConfig> g_drumConfigs;

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
    double tempo;  // microseconds per quarter note
    int ticksPerQuarterNote;

    // High-precision timer for accurate MIDI playback (yasp-style optimization)
    LARGE_INTEGER perfCounterFreq;
    LARGE_INTEGER lastPerfCounter;
    double accumulatedTime;  // Accumulated microseconds for precise timing

    // Track which notes are currently playing for each MIDI channel
    std::map<int, std::map<int, int>> activeNotes;  // channel -> note -> YM2163 channel

    MidiPlayerState() : isPlaying(false), isPaused(false), currentTick(0),
                        tempo(500000.0), ticksPerQuarterNote(120),
                        pausedDuration(0), accumulatedTime(0.0) {
        // Initialize high-precision counter
        QueryPerformanceFrequency(&perfCounterFreq);
        QueryPerformanceCounter(&lastPerfCounter);
    }
};

static MidiPlayerState g_midiPlayer;

// ===== File Browser State =====

struct FileEntry {
    std::string name;
    std::string fullPath;
    bool isDirectory;
};

static char g_currentPath[MAX_PATH] = {0};
static char g_pathInput[MAX_PATH] = {0};
static std::vector<FileEntry> g_fileList;
static std::vector<std::string> g_pathHistory;
static int g_pathHistoryIndex = -1;
static int g_selectedFileIndex = -1;
static bool g_pathEditMode = false;  // Win11-style address bar: false=breadcrumb buttons, true=text input
static bool g_pathEditModeJustActivated = false;  // Track if edit mode was just activated this frame

// Scroll position memory for each path
static std::map<std::string, float> g_pathScrollPositions;
static std::string g_lastExitedFolder;  // Remember which folder we just exited from (persistent until entering another folder)
static std::string g_currentPlayingFilePath;  // Full path of currently playing MIDI file

// Text scrolling for long filenames
struct TextScrollState {
    float scrollOffset;
    float scrollDirection;  // 1.0 = right, -1.0 = left
    float pauseTimer;
    std::chrono::steady_clock::time_point lastUpdateTime;
};
static std::map<int, TextScrollState> g_textScrollStates;  // fileIndex -> scroll state
static int g_hoveredFileIndex = -1;

// Playlist control
static int g_currentPlayingIndex = -1;  // Index in g_fileList
static bool g_isSequentialPlayback = true;  // true=Sequential, false=Random
static bool g_autoPlayNext = true;  // Auto-play next track when current finishes (default: enabled)

// MIDI Folder history
static std::vector<std::string> g_midiFolderHistory;  // History of folders containing MIDI files
static const char* g_midiFolderHistoryFile = "ym2163_folder_history.ini";

// Timer for MIDI playback during window drag
#define TIMER_MIDI_UPDATE 1
static bool g_isWindowDragging = false;

// Tuning window control
static bool g_showTuningWindow = false;

// Input focus state
static bool g_isInputActive = false;  // Track if any input field is being edited

// Global media keys support
static bool g_enableGlobalMediaKeys = true;  // Default enabled
static HWND g_mainWindow = NULL;

// MIDI playback options
static bool g_enableAutoSkipSilence = true;  // Auto-skip silence at start of MIDI

// Hot key IDs
#define HK_PLAY_PAUSE 1001
#define HK_NEXT_TRACK 1002
#define HK_PREV_TRACK 1003

// ===== Forward Declarations =====
void log_command(const char* format, ...);
void stop_note(int channel);
void PlayMIDI();
void StopMIDI();
void PlayNextMIDI();
void PlayPreviousMIDI();
void AddToMIDIFolderHistory(const char* folderPath);
void SaveMIDIFolderHistory();
void LoadMIDIFolderHistory();
void ClearMIDIFolderHistory();
void RemoveMIDIFolderHistoryEntry(int index);
bool ContainsMIDIFiles(const char* folderPath);
void UpdateChannelLevels();  // v10: Update envelope levels for level meters
void UpdateDrumLevels();     // v10: Update drum levels for level meters
void stop_all_notes();
void ResetPianoKeyStates();
void ResetAllYM2163Chips();
void InitializeAllChannels();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// v11: Hardware connection management
void ym2163_init();
void init_single_ym2163(int chipIndex);
void SaveSlotConfigToINI();
void LoadSlotConfigFromINI();
void ConnectHardware();
void DisconnectHardware();
bool CheckHardwarePresent();
void CheckHardwareAutoConnect();

// ===== FTDI Communication =====

static uint8_t g_lastRegAddr = 0xFF;
static bool g_expectingData = false;

int ftdi_init(int dev_idx) {
    FT_STATUS status;

    DWORD numDevs = 0;
    status = FT_CreateDeviceInfoList(&numDevs);
    if (status == FT_OK && numDevs > 0) {
        log_command("=== FTDI Device Detection ===");
        log_command("Found %lu FTDI device(s)", numDevs);

        FT_DEVICE_LIST_INFO_NODE *devInfo = (FT_DEVICE_LIST_INFO_NODE*)
            malloc(sizeof(FT_DEVICE_LIST_INFO_NODE) * numDevs);
        status = FT_GetDeviceInfoList(devInfo, &numDevs);

        if (status == FT_OK) {
            for (DWORD i = 0; i < numDevs; i++) {
                log_command("Device %lu: %s (Serial: %s)",
                    i, devInfo[i].Description, devInfo[i].SerialNumber);
            }
        }
        free(devInfo);
    }

    log_command("Opening device index %d...", dev_idx);
    status = FT_Open(dev_idx, &g_ftHandle);
    if (status != FT_OK) {
        log_command("ERROR: Failed to open device (status=%d)", status);
        return -1;
    }

    log_command("Configuring FTDI parameters...");
    FT_SetBaudRate(g_ftHandle, 1500000);
    FT_SetDataCharacteristics(g_ftHandle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(g_ftHandle, FT_FLOW_NONE, 0, 0);
    FT_SetTimeouts(g_ftHandle, 100, 100);
    FT_SetLatencyTimer(g_ftHandle, 2);
    FT_Purge(g_ftHandle, FT_PURGE_RX | FT_PURGE_TX);

    log_command("FTDI initialized successfully");
    return 0;
}

// v11: Check if FTDI hardware is physically present
bool CheckHardwarePresent() {
    DWORD numDevs = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevs);
    return (status == FT_OK && numDevs > 0);
}

// v11: Check if FTDI device is fully ready to open (driver enumeration complete)
bool CheckHardwareReady() {
    DWORD numDevs = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevs);
    if (status != FT_OK || numDevs == 0) return false;

    FT_DEVICE_LIST_INFO_NODE devInfo;
    status = FT_GetDeviceInfoDetail(0, &devInfo.Flags, &devInfo.Type,
        &devInfo.ID, &devInfo.LocId, devInfo.SerialNumber,
        devInfo.Description, &devInfo.ftHandle);
    if (status != FT_OK) return false;

    // Flags bit 0: device is open (1) or not (0)
    // If device is not already opened by another process, it's ready
    return !(devInfo.Flags & 0x01);
}

// v11: Connect to hardware
void ConnectHardware() {
    if (g_hardwareConnected) return;

    if (ftdi_init(0) == 0) {
        g_hardwareConnected = true;
        ym2163_init();
        log_command("Hardware connected");
    } else {
        log_command("ERROR: Failed to connect hardware");
    }
}

// v11: Disconnect from hardware
void DisconnectHardware() {
    if (!g_hardwareConnected) return;

    // Stop all playing notes first
    stop_all_notes();

    if (g_ftHandle) {
        FT_Close(g_ftHandle);
        g_ftHandle = NULL;
    }
    g_hardwareConnected = false;
    log_command("Hardware disconnected");
}

// v11: Auto-detect hardware plug/unplug
void CheckHardwareAutoConnect() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastHardwareCheck).count();

    // When retrying connection, poll faster (200ms); otherwise normal interval
    int interval = (g_hardwareConnectRetries > 0) ? 200 : HARDWARE_CHECK_INTERVAL_MS;
    if (elapsed < interval) return;
    g_lastHardwareCheck = now;

    bool present = CheckHardwarePresent();

    if (g_hardwareConnected && !present) {
        // Hardware was removed
        stop_all_notes();
        ResetPianoKeyStates();
        g_ftHandle = NULL;
        g_hardwareConnected = false;
        g_manualDisconnect = false;
        g_hardwareConnectRetries = 0;
        log_command("Hardware removed - auto disconnected");
    } else if (!g_hardwareConnected && present && !g_manualDisconnect) {
        // Device listed - check if driver is ready before opening
        if (CheckHardwareReady()) {
            if (ftdi_init(0) == 0) {
                g_hardwareConnected = true;
                ym2163_init();
                g_hardwareConnectRetries = 0;
                log_command("Hardware auto connected");
            } else {
                g_hardwareConnectRetries++;
                if (g_hardwareConnectRetries >= HARDWARE_MAX_RETRIES) {
                    g_hardwareConnectRetries = 0;
                }
            }
        } else {
            // Device present but not ready yet (still enumerating)
            g_hardwareConnectRetries++;
            if (g_hardwareConnectRetries >= HARDWARE_MAX_RETRIES) {
                g_hardwareConnectRetries = 0;
            }
        }
    } else if (!present) {
        g_hardwareConnectRetries = 0;
    }
}

// Write to YM2163 melody channel with chip selection
// chipIndex: 0=Slot0, 1=Slot1
void write_melody_cmd_chip(uint8_t data, int chipIndex) {
    if (!g_ftHandle) return;
    // SPFM format: {slot_select, command, data}
    // Slot0: 0x00, Slot1: 0x01
    uint8_t cmd[3] = {(uint8_t)chipIndex, 0x80, data};
    DWORD written;
    FT_Write(g_ftHandle, cmd, 3, &written);
    FT_Purge(g_ftHandle, FT_PURGE_TX);

    if (!g_expectingData) {
        g_lastRegAddr = data;
        g_expectingData = true;
    } else {
        g_expectingData = false;
    }
}

// Legacy function for backward compatibility (uses Slot0)
void write_melody_cmd(uint8_t data) {
    write_melody_cmd_chip(data, 0);
}

// Initialize one YM2163 chip
void init_single_ym2163(int chipIndex) {
    log_command(chipIndex == 0 ? "=== Initializing YM2163 Slot0 ===" : "=== Initializing YM2163 Slot1 ===");

    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x88 + ch, chipIndex);
        write_melody_cmd_chip(0x14, chipIndex);
        write_melody_cmd_chip(0x8C + ch, chipIndex);
        write_melody_cmd_chip(0x0F, chipIndex);
        write_melody_cmd_chip(0x84 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);
    }

    for (int reg = 0x94; reg <= 0x97; reg++) {
        write_melody_cmd_chip(reg, chipIndex);
        write_melody_cmd_chip((31 << 1) | 0, chipIndex);
    }

    write_melody_cmd_chip(0x90, chipIndex);
    write_melody_cmd_chip(0x00, chipIndex);

    write_melody_cmd_chip(0x98, chipIndex); write_melody_cmd_chip(0x00, chipIndex);
    write_melody_cmd_chip(0x99, chipIndex); write_melody_cmd_chip(0x0D, chipIndex);
    write_melody_cmd_chip(0x9C, chipIndex); write_melody_cmd_chip(0x04, chipIndex);
    write_melody_cmd_chip(0x9D, chipIndex); write_melody_cmd_chip(0x04, chipIndex);

    log_command(chipIndex == 0 ? "YM2163 Slot0 initialized" : "YM2163 Slot1 initialized");
}

void ym2163_init() {
    uint8_t reset_cmd[4] = {0, 0, 0xFE, 0};
    DWORD written;
    FT_Write(g_ftHandle, reset_cmd, 4, &written);
    FT_Purge(g_ftHandle, FT_PURGE_TX);
    Sleep(200);

    log_command("=== YM2163 Initialization ===");

    // Always initialize Slot0
    init_single_ym2163(0);

    // Initialize Slot1 if enabled
    if (g_enableSecondYM2163) {
        init_single_ym2163(1);
    }

    // Initialize Slot2 if enabled
    if (g_enableThirdYM2163) {
        init_single_ym2163(2);
    }

    // Initialize Slot3 if enabled
    if (g_enableFourthYM2163) {
        init_single_ym2163(3);
    }

    // Log active configuration
    int totalChannels = 4;
    if (g_enableSecondYM2163) totalChannels += 4;
    if (g_enableThirdYM2163) totalChannels += 4;
    if (g_enableFourthYM2163) totalChannels += 4;
    log_command("YM2163 mode: %d chips, %d channels", totalChannels / 4, totalChannels);
}

// ===== Logging =====

void log_command(const char* format, ...) {
    char temp[256];
    va_list args;
    va_start(args, format);
    vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);

    g_logBuffer += temp;
    g_logBuffer += "\n";

    if (g_logBuffer.size() > 32000) {
        g_logBuffer.erase(0, 8000);
    }
}

// ===== Helper Functions =====

int get_key_index(int octave, int note) {
    if (octave == 0 && note == 11) {
        return 0;
    } else if (octave >= 1 && octave <= 5) {
        return (octave - 1) * 12 + note + 1;
    } else if (octave == 6) {
        // v11: C8-B8 octave (keys 61-72)
        return 61 + note;
    }
    return -1;
}

// ===== Configuration File Loading =====

void LoadMIDIConfig() {
    log_command("=== Loading MIDI Configuration ===");

    // Load global settings
    char pedalModeStr[32];
    GetPrivateProfileStringA("Settings", "PedalMode", "Disabled", pedalModeStr, sizeof(pedalModeStr), g_midiConfigPath);

    if (strcmp(pedalModeStr, "Piano") == 0) g_pedalMode = 1;
    else if (strcmp(pedalModeStr, "Organ") == 0) g_pedalMode = 2;
    else g_pedalMode = 0;

    // Parse instrument configs (0-127)
    for (int i = 0; i < 128; i++) {
        char section[32];
        sprintf(section, "Instrument_%d", i);

        char name[128];
        char envelope[32];
        char wave[32];
        char pedalModeStr[32];

        GetPrivateProfileStringA(section, "Name", "", name, sizeof(name), g_midiConfigPath);
        GetPrivateProfileStringA(section, "Envelope", "Decay", envelope, sizeof(envelope), g_midiConfigPath);
        GetPrivateProfileStringA(section, "Wave", "Piano", wave, sizeof(wave), g_midiConfigPath);
        GetPrivateProfileStringA(section, "PedalMode", "", pedalModeStr, sizeof(pedalModeStr), g_midiConfigPath);

        InstrumentConfig config;
        config.name = name;

        // Parse envelope
        if (strcmp(envelope, "Decay") == 0) config.envelope = 0;
        else if (strcmp(envelope, "Fast") == 0) config.envelope = 1;
        else if (strcmp(envelope, "Medium") == 0) config.envelope = 2;
        else if (strcmp(envelope, "Slow") == 0) config.envelope = 3;
        else config.envelope = 0;

        // Parse wave
        if (strcmp(wave, "String") == 0) config.wave = 1;
        else if (strcmp(wave, "Organ") == 0) config.wave = 2;
        else if (strcmp(wave, "Clarinet") == 0) config.wave = 3;
        else if (strcmp(wave, "Piano") == 0) config.wave = 4;
        else if (strcmp(wave, "Harpsichord") == 0) config.wave = 5;
        else config.wave = 4;

        // Parse pedal mode (per-instrument override)
        if (strcmp(pedalModeStr, "Piano") == 0) config.pedalMode = 1;
        else if (strcmp(pedalModeStr, "Organ") == 0) config.pedalMode = 2;
        else config.pedalMode = 0;  // Use global setting if not specified

        g_instrumentConfigs[i] = config;
    }

    // Parse drum configs (note 27-63)
    for (int i = 27; i <= 63; i++) {
        char section[32];
        sprintf(section, "Drum_%d", i);

        char name[128];
        char drums[128];

        GetPrivateProfileStringA(section, "Name", "", name, sizeof(name), g_midiConfigPath);
        GetPrivateProfileStringA(section, "Drums", "SDN", drums, sizeof(drums), g_midiConfigPath);

        DrumConfig config;
        config.name = name;

        // Parse drum combinations (e.g., "BD,SDN")
        char* token = strtok(drums, ",");
        while (token != NULL) {
            // Trim whitespace
            while (*token == ' ') token++;

            if (strcmp(token, "BD") == 0) config.drumBits.push_back(0x01);
            else if (strcmp(token, "HC") == 0) config.drumBits.push_back(0x02);
            else if (strcmp(token, "SDN") == 0) config.drumBits.push_back(0x04);
            else if (strcmp(token, "HHO") == 0) config.drumBits.push_back(0x08);
            else if (strcmp(token, "HHD") == 0) config.drumBits.push_back(0x10);

            token = strtok(NULL, ",");
        }

        g_drumConfigs[i] = config;
    }

    log_command("MIDI configuration loaded: %d instruments, %d drums, Pedal Mode: %d",
                (int)g_instrumentConfigs.size(), (int)g_drumConfigs.size(), g_pedalMode);
}

void SaveFrequenciesToINI() {
    char buffer[32];

    sprintf(buffer, "%d", g_fnum_b2);
    WritePrivateProfileStringA("Frequencies", "B2", buffer, g_iniFilePath);

    for (int i = 0; i < 12; i++) {
        sprintf(buffer, "%d", g_fnums[i]);
        WritePrivateProfileStringA("Frequencies", g_noteNames[i], buffer, g_iniFilePath);
    }

    for (int i = 0; i < 12; i++) {
        sprintf(buffer, "%d", g_fnums_c7[i]);
        WritePrivateProfileStringA("Frequencies_C7", g_noteNames[i], buffer, g_iniFilePath);
    }
}

void LoadFrequenciesFromINI() {
    UINT b2_value = GetPrivateProfileIntA("Frequencies", "B2", 0, g_iniFilePath);
    if (b2_value > 0 && b2_value <= 2047) {
        g_fnum_b2 = b2_value;
    }

    for (int i = 0; i < 12; i++) {
        UINT value = GetPrivateProfileIntA("Frequencies", g_noteNames[i], 0, g_iniFilePath);
        if (value > 0 && value <= 2047) {
            g_fnums[i] = value;
        }
    }

    for (int i = 0; i < 12; i++) {
        UINT value = GetPrivateProfileIntA("Frequencies_C7", g_noteNames[i], 0, g_iniFilePath);
        if (value >= 0 && value <= 2047) {
            g_fnums_c7[i] = value;
        }
    }
}

// v11: Save slot configuration to INI
void SaveSlotConfigToINI() {
    char buffer[32];
    sprintf(buffer, "%d", g_enableSecondYM2163 ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot1", buffer, g_iniFilePath);
    sprintf(buffer, "%d", g_enableThirdYM2163 ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot2", buffer, g_iniFilePath);
    sprintf(buffer, "%d", g_enableFourthYM2163 ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot3", buffer, g_iniFilePath);
    sprintf(buffer, "%d", g_enableSlot3_2MHz ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot3_2MHz", buffer, g_iniFilePath);
    sprintf(buffer, "%d", g_enableSlot3Overflow ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot3Overflow", buffer, g_iniFilePath);
    sprintf(buffer, "%d", g_slot3_2MHz_Range);
    WritePrivateProfileStringA("Slots", "Slot3_2MHz_Range", buffer, g_iniFilePath);
}

// v11: Load slot configuration from INI
void LoadSlotConfigFromINI() {
    g_enableSecondYM2163 = GetPrivateProfileIntA("Slots", "Slot1", 1, g_iniFilePath) != 0;
    g_enableThirdYM2163 = GetPrivateProfileIntA("Slots", "Slot2", 0, g_iniFilePath) != 0;
    g_enableFourthYM2163 = GetPrivateProfileIntA("Slots", "Slot3", 0, g_iniFilePath) != 0;
    g_enableSlot3_2MHz = GetPrivateProfileIntA("Slots", "Slot3_2MHz", 0, g_iniFilePath) != 0;
    g_enableSlot3Overflow = GetPrivateProfileIntA("Slots", "Slot3Overflow", 0, g_iniFilePath) != 0;
    g_slot3_2MHz_Range = GetPrivateProfileIntA("Slots", "Slot3_2MHz_Range", 1, g_iniFilePath);
    // Ensure consistency: if 2MHz is on, Slot3 must be enabled
    if (g_enableSlot3_2MHz) g_enableFourthYM2163 = true;
}

// Save current instrument settings to config file
void SaveInstrumentConfig(int instrument) {
    if (instrument < 0 || instrument > 127) return;

    char section[32];
    sprintf(section, "Instrument_%d", instrument);

    // Convert current settings to strings
    const char* envelopeStr = g_envelopeNames[g_currentEnvelope];
    const char* waveStr = g_timbreNames[g_currentTimbre];

    WritePrivateProfileStringA(section, "Envelope", envelopeStr, g_midiConfigPath);
    WritePrivateProfileStringA(section, "Wave", waveStr, g_midiConfigPath);

    // Update in-memory config
    if (g_instrumentConfigs.count(instrument) > 0) {
        g_instrumentConfigs[instrument].envelope = g_currentEnvelope;
        g_instrumentConfigs[instrument].wave = g_currentTimbre;
    }

    log_command("Saved Instrument %d: %s, %s", instrument, waveStr, envelopeStr);
}

// Load instrument settings from config to UI
void LoadInstrumentConfigToUI(int instrument) {
    if (instrument < 0 || instrument > 127) return;

    if (g_instrumentConfigs.count(instrument) > 0) {
        InstrumentConfig& config = g_instrumentConfigs[instrument];
        g_currentEnvelope = config.envelope;
        g_currentTimbre = config.wave;
        log_command("Loaded Instrument %d (%s): %s, %s",
                    instrument, config.name.c_str(),
                    g_timbreNames[g_currentTimbre],
                    g_envelopeNames[g_currentEnvelope]);
    }
}

// ===== YM2163 Control Functions =====

// Helper function: Calculate absolute pitch for comparison (higher value = higher pitch)
int get_absolute_pitch(int note, int octave) {
    return octave * 12 + note;
}

// Helper function: Check if note is within YM2163 valid range (B2 to B7, or B8 with Slot3 2MHz)
bool is_in_valid_range(int note, int octave) {
    // B2 is octave=0, note=11
    if (octave == 0 && note == 11) return true;
    // C3-B7 is octave=1-5
    if (octave >= 1 && octave <= 5) return true;
    // v11: C8-B8 is octave=6 (only with Slot3 2MHz)
    if (octave == 6 && g_enableSlot3_2MHz) return true;
    return false;
}

// Helper function: Map MIDI velocity (0-127) to YM2163 4-level volume
// Two modes: Fixed mapping or Dynamic mapping based on MIDI analysis
int map_velocity_to_volume(int velocity) {
    if (!g_enableDynamicVelocityMapping) {
        // Fixed mapping (original behavior)
        // velocity 0 -> volume 3 (Mute) - only for velocity 0
        // velocity 1-63 -> volume 2 (-12dB) - soft notes
        // velocity 64-112 -> volume 1 (-6dB) - medium to strong notes
        // velocity 113-127 -> volume 0 (0dB) - only very strong notes
        if (velocity == 0) return 3;  // Mute only for velocity 0
        if (velocity <= 63) return 2;  // -12dB for softer velocities
        if (velocity <= 112) return 1;  // -6dB for medium to strong velocities
        return 0;  // 0dB only for very strong velocities (113-127)
    } else {
        // Dynamic mapping based on analyzed velocity distribution
        if (velocity < g_velocityAnalysis.threshold_mute) {
            return 3;  // Mute for very soft notes
        } else if (velocity < g_velocityAnalysis.threshold_12dB) {
            return 2;  // -12dB for soft notes
        } else if (velocity < g_velocityAnalysis.threshold_6dB) {
            return 1;  // -6dB for medium notes
        } else if (velocity < g_velocityAnalysis.threshold_0dB) {
            return 1;  // -6dB for strong notes (prefer -6dB over 0dB)
        } else {
            return 0;  // 0dB only for peak velocities
        }
    }
}

// Analyze velocity distribution in MIDI file
void AnalyzeVelocityDistribution() {
    // Reset analysis
    g_velocityAnalysis = VelocityAnalysis();

    if (!g_midiPlayer.midiFile.status()) {
        log_command("No MIDI file loaded for velocity analysis");
        return;
    }

    // Scan all note-on events in the MIDI file
    for (int track = 0; track < g_midiPlayer.midiFile.getTrackCount(); track++) {
        for (int event = 0; event < g_midiPlayer.midiFile[track].getEventCount(); event++) {
            MidiEvent& midiEvent = g_midiPlayer.midiFile[track][event];

            if (midiEvent.isNoteOn()) {
                int velocity = midiEvent.getVelocity();
                if (velocity > 0) {  // Ignore note-off (velocity 0)
                    g_velocityAnalysis.velocityHistogram[velocity]++;
                    g_velocityAnalysis.totalNotes++;

                    if (velocity < g_velocityAnalysis.minVelocity) {
                        g_velocityAnalysis.minVelocity = velocity;
                    }
                    if (velocity > g_velocityAnalysis.maxVelocity) {
                        g_velocityAnalysis.maxVelocity = velocity;
                    }
                }
            }
        }
    }

    if (g_velocityAnalysis.totalNotes == 0) {
        log_command("No notes found in MIDI file");
        return;
    }

    // Calculate average velocity
    long long sum = 0;
    for (int i = 0; i < 128; i++) {
        sum += i * g_velocityAnalysis.velocityHistogram[i];
    }
    g_velocityAnalysis.avgVelocity = (float)sum / g_velocityAnalysis.totalNotes;

    // Find most common velocities
    int maxCount1 = 0, maxCount2 = 0;
    for (int i = 1; i < 128; i++) {  // Skip velocity 0
        if (g_velocityAnalysis.velocityHistogram[i] > maxCount1) {
            maxCount2 = maxCount1;
            g_velocityAnalysis.mostCommonVelocity2 = g_velocityAnalysis.mostCommonVelocity1;
            maxCount1 = g_velocityAnalysis.velocityHistogram[i];
            g_velocityAnalysis.mostCommonVelocity1 = i;
        } else if (g_velocityAnalysis.velocityHistogram[i] > maxCount2) {
            maxCount2 = g_velocityAnalysis.velocityHistogram[i];
            g_velocityAnalysis.mostCommonVelocity2 = i;
        }
    }

    // Find peak velocity (95th percentile to avoid outliers)
    int cumulativeCount = 0;
    int percentile95 = (int)(g_velocityAnalysis.totalNotes * 0.95f);
    for (int i = 127; i >= 0; i--) {
        cumulativeCount += g_velocityAnalysis.velocityHistogram[i];
        if (cumulativeCount >= (g_velocityAnalysis.totalNotes - percentile95)) {
            g_velocityAnalysis.peakVelocity = i;
            break;
        }
    }

    // Calculate dynamic thresholds
    // Strategy:
    // - Map most common velocities to -6dB and -12dB (most used levels)
    // - Map peak velocities to 0dB (maximum volume)
    // - Map very low velocities to mute

    int vel1 = g_velocityAnalysis.mostCommonVelocity1;
    int vel2 = g_velocityAnalysis.mostCommonVelocity2;

    // Ensure vel1 > vel2 for easier threshold calculation
    if (vel1 < vel2) {
        int temp = vel1;
        vel1 = vel2;
        vel2 = temp;
    }

    // Set thresholds based on velocity distribution
    // 0dB: Peak velocities (top 10%)
    g_velocityAnalysis.threshold_0dB = g_velocityAnalysis.peakVelocity;

    // -6dB: Higher of the two most common velocities
    g_velocityAnalysis.threshold_6dB = (vel1 + vel2) / 2;

    // -12dB: Lower of the two most common velocities
    g_velocityAnalysis.threshold_12dB = vel2 - (vel1 - vel2) / 2;

    // Mute: Very low velocities (below 15% of average)
    g_velocityAnalysis.threshold_mute = (int)(g_velocityAnalysis.avgVelocity * 0.15f);

    // Clamp thresholds to valid ranges
    if (g_velocityAnalysis.threshold_mute < 1) g_velocityAnalysis.threshold_mute = 1;
    if (g_velocityAnalysis.threshold_12dB < 20) g_velocityAnalysis.threshold_12dB = 20;
    if (g_velocityAnalysis.threshold_6dB < 40) g_velocityAnalysis.threshold_6dB = 40;
    if (g_velocityAnalysis.threshold_0dB < 90) g_velocityAnalysis.threshold_0dB = 90;

    // Ensure proper ordering
    if (g_velocityAnalysis.threshold_12dB <= g_velocityAnalysis.threshold_mute) {
        g_velocityAnalysis.threshold_12dB = g_velocityAnalysis.threshold_mute + 10;
    }
    if (g_velocityAnalysis.threshold_6dB <= g_velocityAnalysis.threshold_12dB) {
        g_velocityAnalysis.threshold_6dB = g_velocityAnalysis.threshold_12dB + 10;
    }
    if (g_velocityAnalysis.threshold_0dB <= g_velocityAnalysis.threshold_6dB) {
        g_velocityAnalysis.threshold_0dB = g_velocityAnalysis.threshold_6dB + 10;
    }

    log_command("=== Velocity Analysis ===");
    log_command("Total notes: %d", g_velocityAnalysis.totalNotes);
    log_command("Velocity range: %d - %d", g_velocityAnalysis.minVelocity, g_velocityAnalysis.maxVelocity);
    log_command("Average velocity: %.1f", g_velocityAnalysis.avgVelocity);
    log_command("Peak velocity (95%%): %d", g_velocityAnalysis.peakVelocity);
    log_command("Most common velocities: %d (count: %d), %d (count: %d)",
                g_velocityAnalysis.mostCommonVelocity1, maxCount1,
                g_velocityAnalysis.mostCommonVelocity2, maxCount2);
    log_command("Dynamic thresholds:");
    log_command("  0dB: >= %d", g_velocityAnalysis.threshold_0dB);
    log_command("  -6dB: %d - %d", g_velocityAnalysis.threshold_6dB, g_velocityAnalysis.threshold_0dB - 1);
    log_command("  -12dB: %d - %d", g_velocityAnalysis.threshold_12dB, g_velocityAnalysis.threshold_6dB - 1);
    log_command("  Mute: < %d", g_velocityAnalysis.threshold_mute);
}

// Minimum note duration before it can be replaced (in milliseconds)
static const int MIN_NOTE_DURATION_MS = 50;

// v11: Check if a channel's slot is enabled for normal (non-C8) note allocation
bool isChannelEnabled(int ch) {
    int chip = g_channels[ch].chipIndex;
    if (chip == 0) return true;  // Slot0 always enabled
    if (chip == 1) return g_enableSecondYM2163;
    if (chip == 2) return g_enableThirdYM2163;
    if (chip == 3) return g_enableFourthYM2163 && !g_enableSlot3_2MHz;  // Slot3 1MHz only
    return false;
}

// v11: Check normal channel usage ratio (0.0 ~ 1.0)
float getNormalChannelUsage() {
    int total = 0, active = 0;
    for (int i = 0; i < 16; i++) {
        if (!isChannelEnabled(i)) continue;
        total++;
        if (g_channels[i].active) active++;
    }
    if (total == 0) return 1.0f;
    return (float)active / (float)total;
}

int find_free_channel() {
    // Strategy 1: Find completely unused channels (never been used before)
    for (int i = 0; i < 16; i++) {
        if (!isChannelEnabled(i)) continue;
        if (!g_channels[i].active && !g_channels[i].hasBeenUsed) {
            g_channels[i].hasBeenUsed = true;
            return i;
        }
    }

    // Strategy 2: Find free channels (released but previously used)
    // Prefer channels that were released longest ago (envelope has more time to complete)
    int bestFreeChannel = -1;
    auto oldestReleaseTime = std::chrono::steady_clock::now();

    for (int i = 0; i < 16; i++) {
        if (!isChannelEnabled(i)) continue;
        if (!g_channels[i].active && g_channels[i].hasBeenUsed) {
            if (bestFreeChannel < 0 || g_channels[i].releaseTime < oldestReleaseTime) {
                bestFreeChannel = i;
                oldestReleaseTime = g_channels[i].releaseTime;
            }
        }
    }

    if (bestFreeChannel >= 0) {
        return bestFreeChannel;
    }

    // Strategy 3: No free channels - use intelligent strategy to choose which channel to replace
    // Priority:
    // 1. Replace out-of-range notes first
    // 2. Among valid-range notes, prefer replacing lower-pitched notes (high pitch priority)
    // 3. Prefer notes that have played for at least MIN_NOTE_DURATION_MS

    auto now = std::chrono::steady_clock::now();

    int channelToReplace = -1;
    int lowestOutOfRangePitch = INT_MAX;
    int lowestInRangePitch = INT_MAX;
    bool hasOutOfRange = false;

    // Track which channels have played long enough
    bool canReplace[16] = {false};
    for (int i = 0; i < 16; i++) {
        if (!isChannelEnabled(i)) continue;
        if (g_channels[i].active) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_channels[i].startTime);

            // Adjust minimum duration based on envelope type
            int minDuration = MIN_NOTE_DURATION_MS;
            if (g_channels[i].envelope == 0) {  // Decay envelope: ~1 second release
                minDuration = 1000;  // 1 second to allow full envelope release
            } else if (g_channels[i].envelope == 1) {  // Fast envelope
                minDuration = 500;   // 500ms
            } else if (g_channels[i].envelope == 2) {  // Medium envelope
                minDuration = 2000;  // 2 seconds
            } else if (g_channels[i].envelope == 3) {  // Slow envelope
                minDuration = 3000;  // 3 seconds
            }

            canReplace[i] = (duration.count() >= minDuration);
        }
    }

    for (int i = 0; i < 16; i++) {
        if (!isChannelEnabled(i)) continue;
        if (!g_channels[i].active) continue;

        int pitch = get_absolute_pitch(g_channels[i].note, g_channels[i].octave);
        bool inRange = is_in_valid_range(g_channels[i].note, g_channels[i].octave);

        if (!inRange) {
            // Out of range note found
            hasOutOfRange = true;
            // Prefer out-of-range notes that have played long enough
            if (canReplace[i] && pitch < lowestOutOfRangePitch) {
                lowestOutOfRangePitch = pitch;
                channelToReplace = i;
            } else if (!canReplace[i] && channelToReplace < 0) {
                // If no replaceable out-of-range notes found yet, use this as fallback
                lowestOutOfRangePitch = pitch;
                channelToReplace = i;
            }
        } else {
            // Valid range note
            if (canReplace[i] && pitch < lowestInRangePitch) {
                lowestInRangePitch = pitch;
            }
        }
    }

    // If we found out-of-range notes, replace one
    if (hasOutOfRange && channelToReplace >= 0) {
        stop_note(channelToReplace);
        return channelToReplace;
    }

    // All notes are in valid range - replace the lowest pitch note that has played long enough
    for (int i = 0; i < 16; i++) {
        if (!isChannelEnabled(i)) continue;
        if (!g_channels[i].active) continue;

        int pitch = get_absolute_pitch(g_channels[i].note, g_channels[i].octave);

        // Find the lowest pitch note that can be replaced
        if (canReplace[i] && pitch == lowestInRangePitch) {
            channelToReplace = i;
            break;
        }
    }

    // If no long-enough notes found, replace any lowest pitch note as last resort
    if (channelToReplace < 0) {
        int lowestValidPitch = INT_MAX;
        for (int i = 0; i < 16; i++) {
            if (!isChannelEnabled(i)) continue;
            if (!g_channels[i].active) continue;
            int pitch = get_absolute_pitch(g_channels[i].note, g_channels[i].octave);
            if (pitch < lowestValidPitch) {
                lowestValidPitch = pitch;
                channelToReplace = i;
            }
        }
    }

    // Force stop the note on this channel
    if (channelToReplace >= 0 && g_channels[channelToReplace].active) {
        stop_note(channelToReplace);
    }

    return channelToReplace >= 0 ? channelToReplace : 0;  // Fallback to channel 0
}

// v11: Find free channel on Slot3 only (for C8-B8 octave)
int find_free_channel_slot3() {
    if (!g_enableSlot3_2MHz) return -1;

    // Slot3 channels are 12-15
    for (int i = 12; i < 16; i++) {
        if (!g_channels[i].active && !g_channels[i].hasBeenUsed) {
            g_channels[i].hasBeenUsed = true;
            return i;
        }
    }
    // Find released channel
    int bestFreeChannel = -1;
    auto oldestReleaseTime = std::chrono::steady_clock::now();
    for (int i = 12; i < 16; i++) {
        if (!g_channels[i].active && g_channels[i].hasBeenUsed) {
            if (bestFreeChannel < 0 || g_channels[i].releaseTime < oldestReleaseTime) {
                bestFreeChannel = i;
                oldestReleaseTime = g_channels[i].releaseTime;
            }
        }
    }
    if (bestFreeChannel >= 0) return bestFreeChannel;

    // All busy - replace note by priority: lower octave overflow gets stolen first
    // C8-B8 (octave 6) has absolute priority, then C7-B7, C6-B6, C5-B5, C4-B4
    int channelToReplace = -1;
    int lowestOctave = INT_MAX;
    auto oldestStart = std::chrono::steady_clock::now();

    for (int i = 12; i < 16; i++) {
        if (!g_channels[i].active) continue;
        int oct = g_channels[i].octave;
        // Prefer stealing lower octave; among same octave, steal oldest
        if (oct < lowestOctave || (oct == lowestOctave && g_channels[i].startTime < oldestStart)) {
            lowestOctave = oct;
            oldestStart = g_channels[i].startTime;
            channelToReplace = i;
        }
    }

    if (channelToReplace < 0) channelToReplace = 12;
    stop_note(channelToReplace);
    return channelToReplace;
}

int find_channel_playing(int note, int octave) {
    // Search all 16 channels (including Slot3 2MHz channels)
    for (int i = 0; i < 16; i++) {
        if (g_channels[i].active &&
            g_channels[i].note == note &&
            g_channels[i].octave == octave) {
            return i;
        }
    }
    return -1;
}

void play_note(int channel, int note, int octave, int timbre = -1, int envelope = -1, int volume = -1) {
    if (channel < 0 || channel >= 16) return;

    // Determine chip and local channel
    int chipIndex = g_channels[channel].chipIndex;
    int localChannel = channel % 4;  // 0-3 for each chip

    uint16_t fnum;
    uint8_t hw_octave;

    if (g_enableSlot3_2MHz && chipIndex == 3) {
        // v11: Slot3 2MHz mode - handle based on range setting
        // Range 0: C8-B8 only (octave 6)
        // Range 1: C7-B8 (octave 5-6)
        bool isInRange = (g_slot3_2MHz_Range == 0) ? (octave == 6) : (octave >= 5 && octave <= 6);

        if (isInRange) {
            if (octave == 6) {
                // C8-B8: use C7-B7 config, 2MHz doubles to C8-B8
                fnum = g_fnums_c7[note];
                hw_octave = 3;
            } else if (octave == 5) {
                // C7-B7: use C6-B6 config, 2MHz doubles to C7-B7
                fnum = g_fnums[note];
                hw_octave = 3;
            }
        } else if (octave == 6 || (octave == 5 && g_slot3_2MHz_Range == 1)) {
            // Trying to play high octave on non-Slot3 chip - reject
            return;
        }
    } else if (g_enableSlot3_2MHz && (octave == 6 || (octave == 5 && g_slot3_2MHz_Range == 1))) {
        // High octave note but not on Slot3 - reject
        return;
    } else if (chipIndex == 3 && g_enableSlot3_2MHz && octave >= 2 && octave <= 5) {
        // v11: C4-B7 overflow to Slot3 2MHz - remap pitch down one octave
        // 2MHz clock doubles pitch, so use (octave-1) register config
        int remappedOctave = octave - 1;  // 2->1, 3->2, 4->3, 5->4
        fnum = g_fnums[note];
        hw_octave = (remappedOctave - 1) & 0x03;  // 1->0, 2->1, 3->2, 4->3
    } else if (octave == 0 && note == 11) {
        fnum = g_fnum_b2;
        hw_octave = 0;
    } else if (octave >= 1 && octave <= 4) {
        fnum = g_fnums[note];
        hw_octave = (octave - 1) & 0x03;
    } else if (octave == 5) {
        fnum = g_fnums_c7[note];
        hw_octave = 3;
    } else {
        return;
    }

    uint8_t fnum_low = fnum & 0x7F;
    uint8_t fnum_high = (fnum >> 7) & 0x07;

    g_channels[channel].note = note;
    g_channels[channel].octave = octave;
    g_channels[channel].fnum = fnum;
    g_channels[channel].active = true;
    g_channels[channel].startTime = std::chrono::steady_clock::now();  // Record start time

    // Use provided timbre/envelope or fall back to current settings
    int useTimbre = (timbre >= 0) ? timbre : g_currentTimbre;
    int useEnvelope = (envelope >= 0) ? envelope : g_currentEnvelope;
    int useVolume = (volume >= 0) ? volume : g_currentVolume;

    // Store settings in channel state
    g_channels[channel].timbre = useTimbre;
    g_channels[channel].envelope = useEnvelope;
    g_channels[channel].volume = useVolume;

    write_melody_cmd_chip(0x88 + localChannel, chipIndex);
    uint8_t timbre_val = (useTimbre & 0x07) | ((useEnvelope & 0x03) << 4);
    write_melody_cmd_chip(timbre_val, chipIndex);

    write_melody_cmd_chip(0x8C + localChannel, chipIndex);
    write_melody_cmd_chip(0x0F | ((useVolume & 0x03) << 4), chipIndex);

    write_melody_cmd_chip(0x84 + localChannel, chipIndex);
    write_melody_cmd_chip((hw_octave << 3) | fnum_high, chipIndex);

    write_melody_cmd_chip(0x80 + localChannel, chipIndex);
    write_melody_cmd_chip(fnum_low, chipIndex);

    write_melody_cmd_chip(0x84 + localChannel, chipIndex);
    write_melody_cmd_chip(0x40 | (hw_octave << 3) | fnum_high, chipIndex);
}

void stop_note(int channel) {
    if (channel < 0 || channel >= 16) return;

    // Determine chip and local channel
    int chipIndex = g_channels[channel].chipIndex;
    int localChannel = channel % 4;

    int note = g_channels[channel].note;
    int octave = g_channels[channel].octave;
    uint16_t fnum = g_channels[channel].fnum;

    uint8_t hw_octave;
    if (g_enableSlot3_2MHz && chipIndex == 3) {
        // v11: Slot3 2MHz mode - handle based on range setting
        bool isInRange = (g_slot3_2MHz_Range == 0) ? (octave == 6) : (octave >= 5 && octave <= 6);
        if (isInRange) {
            hw_octave = 3;  // Both C7-B7 and C8-B8 use hw_octave=3 on 2MHz
        } else if (octave >= 2 && octave <= 5) {
            // C4-B7 overflow on Slot3 2MHz - remapped down one octave
            int remappedOctave = octave - 1;
            hw_octave = (remappedOctave - 1) & 0x03;
        } else {
            return;
        }
    } else if (octave == 0 && note == 11) {
        hw_octave = 0;
    } else if (octave >= 1 && octave <= 4) {
        hw_octave = (octave - 1) & 0x03;
    } else if (octave == 5) {
        hw_octave = 3;
    } else {
        return;
    }

    uint8_t fnum_low = fnum & 0x7F;
    uint8_t fnum_high = (fnum >> 7) & 0x07;

    write_melody_cmd_chip(0x80 + localChannel, chipIndex);
    write_melody_cmd_chip(fnum_low, chipIndex);

    write_melody_cmd_chip(0x84 + localChannel, chipIndex);
    write_melody_cmd_chip((hw_octave << 3) | fnum_high, chipIndex);

    // Clear piano key visual
    int keyIdx = get_key_index(octave, note);
    if (keyIdx >= 0 && keyIdx < 73) {
        g_pianoKeyPressed[keyIdx] = false;
        g_pianoKeyOnSlot3_2MHz[keyIdx] = false;
    }

    // Record release time for intelligent channel allocation
    g_channels[channel].releaseTime = std::chrono::steady_clock::now();
    g_channels[channel].active = false;
    g_channels[channel].midiChannel = -1;
}

void stop_all_notes() {
    int maxChannels = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0) + (g_enableFourthYM2163 ? 4 : 0);
    for (int i = 0; i < maxChannels; i++) {
        if (g_channels[i].active) {
            stop_note(i);

            int keyIdx = get_key_index(g_channels[i].octave, g_channels[i].note);
            if (keyIdx >= 0 && keyIdx < 73) {
                g_pianoKeyPressed[keyIdx] = false;
            }
        }
    }
}

// Reset all piano key UI states (call when switching songs to clear residual pressed keys)
void ResetPianoKeyStates() {
    for (int i = 0; i < 73; i++) {
        g_pianoKeyPressed[i] = false;
        g_pianoKeyVelocity[i] = 0;
        g_pianoKeyFromKeyboard[i] = false;
        g_pianoKeyOnSlot3_2MHz[i] = false;
    }
}

// Reset YM2163 chip to eliminate residual sound/notes
void ResetYM2163Chip(int chipIndex) {
    if (!g_ftHandle) return;

    log_command("Resetting YM2163 Chip %d...", chipIndex);

    // Send all note-off commands for all 4 channels on this chip
    for (int ch = 0; ch < 4; ch++) {
        // Send note-off (key off) command: 0x88 + channel
        write_melody_cmd_chip(0x88 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);  // Key off
    }

    // Reset all volume to mute (volume = 3)
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x8C + ch, chipIndex);
        write_melody_cmd_chip(0x03, chipIndex);  // Mute
    }

    // Reset all envelope to decay
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x84 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);  // Decay envelope
    }

    // Reset all wave/timbre to 0
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x80 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);  // Timbre 0
    }

    // Reset rhythm section
    write_melody_cmd_chip(0x90, chipIndex);
    write_melody_cmd_chip(0x00, chipIndex);  // All rhythm off

    log_command("YM2163 Chip %d reset complete", chipIndex);
}

// Reset all YM2163 chips to eliminate residual sound
void ResetAllYM2163Chips() {
    log_command("=== Resetting all YM2163 chips ===");

    // Reset Slot0 (always present)
    ResetYM2163Chip(0);

    // Reset Slot1 if enabled
    if (g_enableSecondYM2163) {
        ResetYM2163Chip(1);
    }

    // Reset Slot2 if enabled
    if (g_enableThirdYM2163) {
        ResetYM2163Chip(2);
    }

    // Reset Slot3 if enabled (either normal or 2MHz mode)
    if (g_enableFourthYM2163 || g_enableSlot3_2MHz) {
        ResetYM2163Chip(3);
    }

    Sleep(50);  // Wait for chips to settle
}

// Initialize all channels to clean state (eliminate residual sound)
void InitializeAllChannels() {
    int maxChannels = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0) + (g_enableFourthYM2163 ? 4 : 0);
    auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < maxChannels; i++) {
        g_channels[i].active = false;
        g_channels[i].midiChannel = -1;
        g_channels[i].note = 0;
        g_channels[i].octave = 0;
        g_channels[i].fnum = 0;
        g_channels[i].timbre = 0;
        g_channels[i].envelope = 0;
        g_channels[i].volume = 0;
        g_channels[i].startTime = now;
        g_channels[i].releaseTime = now;
        g_channels[i].hasBeenUsed = false;

        // Clear piano key visual
        int keyIdx = get_key_index(g_channels[i].octave, g_channels[i].note);
        if (keyIdx >= 0 && keyIdx < 73) {
            g_pianoKeyPressed[keyIdx] = false;
        }
    }

    // Reset drum states for all chips
    for (int chip = 0; chip < 4; chip++) {
        for (int i = 0; i < 5; i++) {
            g_drumActive[chip][i] = false;
        }
    }
}

// Clean up channels in Release phase that have exceeded timeout
void play_drum(uint8_t rhythm_bit) {
    // Dynamic chip allocation when second chip is enabled
    int chipIndex = 0;
    if (g_enableSecondYM2163) {
        chipIndex = g_currentDrumChip;
        // Alternate between chips for next drum hit
        g_currentDrumChip = 1 - g_currentDrumChip;
        log_command("Drum triggered on Chip %d (next will use Chip %d)", chipIndex, g_currentDrumChip);
    }

    write_melody_cmd_chip(0x90, chipIndex);
    write_melody_cmd_chip(rhythm_bit, chipIndex);

    // Track which drums were triggered on this specific chip
    for (int i = 0; i < 5; i++) {
        if (rhythm_bit & g_drumBits[i]) {
            g_drumActive[chipIndex][i] = true;
            g_drumTriggerTime[chipIndex][i] = std::chrono::steady_clock::now();
        }
    }
}

void UpdateDrumStates() {
    // Auto-clear drum states after 100ms for each chip
    auto now = std::chrono::steady_clock::now();
    for (int chip = 0; chip < 4; chip++) {  // v10: Support 4 chips
        for (int i = 0; i < 5; i++) {
            if (g_drumActive[chip][i]) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_drumTriggerTime[chip][i]);
                if (elapsed.count() > 100) {
                    g_drumActive[chip][i] = false;
                }
            }
        }
    }
}

void CleanupStuckChannels() {
    // Force-release channels that have been active for more than 10 seconds
    // This prevents "stuck" channels from permanently occupying slots
    auto now = std::chrono::steady_clock::now();
    int maxChannels = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0) + (g_enableFourthYM2163 ? 4 : 0);

    for (int i = 0; i < maxChannels; i++) {
        if (g_channels[i].active) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_channels[i].startTime);
            if (duration.count() > 10000) {  // 10 seconds
                // Force-stop this stuck channel
                stop_note(i);
            }
        }
    }
}

// ===== Level Meter Functions =====

// Calculate envelope level based on YM2163 envelope behavior
// Returns 0.0 (silent) to 1.0 (full volume)
float CalculateEnvelopeLevel(int envelope, bool active,
                             std::chrono::steady_clock::time_point startTime,
                             std::chrono::steady_clock::time_point releaseTime) {
    auto now = std::chrono::steady_clock::now();

    if (active) {
        // Note is playing - calculate attack/sustain phase
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
        float t = elapsed.count() / 1000.0f;  // Time in seconds

        // Envelope behavior based on YM2163 documentation:
        // Envelope 0 (Decay): Fast attack, immediate decay
        // Envelope 1 (Fast): Fast attack, fast decay to sustain
        // Envelope 2 (Medium): Medium attack, medium decay to sustain
        // Envelope 3 (Slow): Slow attack, slow decay to sustain

        switch (envelope) {
            case 0:  // Decay envelope - medium decay to 0 while held, very fast decay on release
                return expf(-t * 1.0f);  // Medium decay to 0

            case 1:  // Fast envelope - medium decay to 0 while held, fast decay on release
                if (t < 0.05f) return t / 0.05f;  // 50ms attack
                else return expf(-(t - 0.05f) * 1.0f);  // Medium decay to 0

            case 2:  // Medium envelope - organ style: sustain at full volume
                return 1.0f;  // Sustain at full volume

            case 3:  // Slow envelope - organ style: sustain at full volume
                return 1.0f;  // Sustain at full volume

            default:
                return 1.0f;
        }
    } else {
        // Note released - calculate release phase
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - releaseTime);
        float t = elapsed.count() / 1000.0f;  // Time in seconds

        // Release times vary by envelope
        float releaseTime_sec = 0.0f;
        switch (envelope) {
            case 0: releaseTime_sec = 0.2f; break;   // Very fast release (0.2 seconds)
            case 1: releaseTime_sec = 0.5f; break;   // Fast release (0.5 seconds)
            case 2: releaseTime_sec = 2.0f; break;   // 2 seconds
            case 3: releaseTime_sec = 3.0f; break;   // 3 seconds
        }

        if (t >= releaseTime_sec) return 0.0f;
        return expf(-t * (3.0f / releaseTime_sec));  // Exponential decay
    }
}

void UpdateChannelLevels() {
    // Update envelope levels for all channels
    int maxChannels = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0) + (g_enableFourthYM2163 ? 4 : 0);

    for (int i = 0; i < maxChannels; i++) {
        if (g_channels[i].active || g_channels[i].hasBeenUsed) {
            float envelopeLevel = CalculateEnvelopeLevel(
                g_channels[i].envelope,
                g_channels[i].active,
                g_channels[i].startTime,
                g_channels[i].releaseTime
            );

            // Apply volume attenuation (0dB, -6dB, -12dB, Mute)
            float volumeMultiplier = 1.0f;
            switch (g_channels[i].volume) {
                case 0: volumeMultiplier = 1.0f; break;    // 0dB
                case 1: volumeMultiplier = 0.5f; break;    // -6dB
                case 2: volumeMultiplier = 0.25f; break;   // -12dB
                case 3: volumeMultiplier = 0.0f; break;    // Mute
            }

            g_channels[i].currentLevel = envelopeLevel * volumeMultiplier;
        } else {
            g_channels[i].currentLevel = 0.0f;
        }
    }
}

void UpdateDrumLevels() {
    // Update drum levels for all chips
    auto now = std::chrono::steady_clock::now();

    for (int chip = 0; chip < 4; chip++) {
        float maxLevel = 0.0f;

        // Check all 5 drums for this chip
        for (int i = 0; i < 5; i++) {
            if (g_drumActive[chip][i]) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_drumTriggerTime[chip][i]);
                float t = elapsed.count() / 1000.0f;  // Time in seconds

                // Drum envelope: fast attack, exponential decay (~100ms)
                float level = expf(-t * 20.0f);  // Fast decay
                if (level > maxLevel) maxLevel = level;
            }
        }

        g_drumLevel[chip] = maxLevel;
    }
}

// ===== File Browser Functions =====

// Helper functions for UTF-8 and wide character conversion
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

// v11: Count UTF-8 characters (not bytes)
int UTF8CharCount(const std::string& str) {
    int count = 0;
    for (size_t i = 0; i < str.size(); ) {
        unsigned char c = str[i];
        if (c < 0x80) i += 1;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else i += 4;
        count++;
    }
    return count;
}

// v11: Get byte offset of the N-th UTF-8 character
size_t UTF8CharOffset(const std::string& str, int charIndex) {
    size_t offset = 0;
    for (int i = 0; i < charIndex && offset < str.size(); i++) {
        unsigned char c = str[offset];
        if (c < 0x80) offset += 1;
        else if (c < 0xE0) offset += 2;
        else if (c < 0xF0) offset += 3;
        else offset += 4;
    }
    return offset;
}

// Truncate long folder names for address bar display (e.g., "1234...7890")
// v11: UTF-8 aware truncation to avoid breaking multi-byte characters
std::string TruncateFolderName(const std::string& name, int maxLength = 20) {
    int charCount = UTF8CharCount(name);
    if (charCount <= maxLength) {
        return name;
    }

    // Calculate how many characters to show on each side
    int sideLength = (maxLength - 3) / 2;  // -3 for "..."

    size_t prefixEnd = UTF8CharOffset(name, sideLength);
    size_t suffixStart = UTF8CharOffset(name, charCount - sideLength);

    return name.substr(0, prefixEnd) + "..." + name.substr(suffixStart);
}

void RefreshFileList() {
    g_fileList.clear();
    g_selectedFileIndex = -1;

    // Add parent directory entry if not at root
    if (strlen(g_currentPath) > 3) {
        FileEntry parent;
        parent.name = "..";
        parent.fullPath = "";
        parent.isDirectory = true;
        g_fileList.push_back(parent);
    }

    // Scan directory using wide character API for Unicode support
    std::wstring wCurrentPath = UTF8ToWide(g_currentPath);
    std::wstring wSearchPath = wCurrentPath + L"\\*";

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(wSearchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            // Skip "." and ".."
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
                continue;
            }

            FileEntry entry;
            entry.name = WideToUTF8(findData.cFileName);
            std::wstring wFullPath = wCurrentPath + L"\\" + findData.cFileName;
            entry.fullPath = WideToUTF8(wFullPath);
            entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            // Add directories or .mid/.midi files only
            if (entry.isDirectory) {
                g_fileList.push_back(entry);
            } else {
                const wchar_t* ext = wcsrchr(findData.cFileName, L'.');
                if (ext && (_wcsicmp(ext, L".mid") == 0 || _wcsicmp(ext, L".midi") == 0)) {
                    g_fileList.push_back(entry);
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    // Sort: directories first, then files
    std::sort(g_fileList.begin(), g_fileList.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return a.name < b.name;
    });
}

void NavigateToPath(const char* path) {
    // Convert to wide string for Unicode support
    std::wstring wPath = UTF8ToWide(path);

    // Normalize path
    wchar_t wNormalizedPath[MAX_PATH];
    if (GetFullPathNameW(wPath.c_str(), MAX_PATH, wNormalizedPath, NULL) == 0) {
        log_command("ERROR: Invalid path: %s", path);
        return;
    }

    // Check if path exists
    DWORD attr = GetFileAttributesW(wNormalizedPath);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        log_command("ERROR: Path does not exist: %s", path);
        return;
    }

    // Convert back to UTF-8 and update current path
    std::string normalizedPath = WideToUTF8(wNormalizedPath);

    // Extract folder name from current path to remember which folder we're entering
    std::string oldPath = g_currentPath;

    strcpy(g_currentPath, normalizedPath.c_str());
    strcpy(g_pathInput, normalizedPath.c_str());

    // Add to history
    if (g_pathHistoryIndex < (int)g_pathHistory.size() - 1) {
        // Remove forward history
        g_pathHistory.erase(g_pathHistory.begin() + g_pathHistoryIndex + 1, g_pathHistory.end());
    }
    g_pathHistory.push_back(normalizedPath);
    g_pathHistoryIndex = g_pathHistory.size() - 1;

    RefreshFileList();
    log_command("Navigated to: %s", normalizedPath.c_str());

    // Add to MIDI folder history if contains MIDI files
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

    // Remove trailing backslash if present
    size_t len = strlen(parentPath);
    if (len > 0 && parentPath[len - 1] == '\\') {
        parentPath[len - 1] = '\0';
        len--;
    }

    // Find last backslash
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        // Extract the folder name we're exiting from (persistent until entering another folder)
        g_lastExitedFolder = std::string(lastSlash + 1);

        *lastSlash = '\0';
        NavigateToPath(parentPath);
    }
}

// Split path into segments for Win11-style breadcrumb navigation
std::vector<std::string> SplitPath(const char* path) {
    std::vector<std::string> segments;
    std::string pathStr(path);

    // Handle drive letter (e.g., "C:\")
    if (pathStr.length() >= 2 && pathStr[1] == ':') {
        segments.push_back(pathStr.substr(0, 3));  // "C:\"
        pathStr = pathStr.substr(3);  // Remove drive part
    }

    // Split remaining path by backslash
    size_t start = 0;
    size_t end = 0;
    while ((end = pathStr.find('\\', start)) != std::string::npos) {
        if (end > start) {
            segments.push_back(pathStr.substr(start, end - start));
        }
        start = end + 1;
    }

    // Add last segment if exists
    if (start < pathStr.length()) {
        segments.push_back(pathStr.substr(start));
    }

    return segments;
}

// ===== MIDI Folder History Management =====

bool ContainsMIDIFiles(const char* folderPath) {
    WIN32_FIND_DATAW findData;
    HANDLE findHandle;

    // Convert to wide string
    std::wstring wPath = UTF8ToWide(folderPath);
    std::wstring searchPath = wPath + L"\\*.mid";

    findHandle = FindFirstFileW(searchPath.c_str(), &findData);
    if (findHandle != INVALID_HANDLE_VALUE) {
        FindClose(findHandle);
        return true;  // Found at least one .mid file
    }

    // Try .midi extension
    searchPath = wPath + L"\\*.midi";
    findHandle = FindFirstFileW(searchPath.c_str(), &findData);
    if (findHandle != INVALID_HANDLE_VALUE) {
        FindClose(findHandle);
        return true;  // Found at least one .midi file
    }

    return false;  // No MIDI files found
}

void AddToMIDIFolderHistory(const char* folderPath) {
    if (!folderPath || strlen(folderPath) == 0) return;
    if (!ContainsMIDIFiles(folderPath)) return;  // Only add if contains MIDI files

    // Check if already exists and remove it (so we can add it to the top)
    for (auto it = g_midiFolderHistory.begin(); it != g_midiFolderHistory.end(); ++it) {
        if (*it == folderPath) {
            g_midiFolderHistory.erase(it);
            break;
        }
    }

    // Add to history at the beginning (most recently used at top)
    g_midiFolderHistory.insert(g_midiFolderHistory.begin(), folderPath);

    // Keep only the most recent 20 entries
    if (g_midiFolderHistory.size() > 20) {
        g_midiFolderHistory.pop_back();
    }

    SaveMIDIFolderHistory();
}

void SaveMIDIFolderHistory() {
    char historyPath[MAX_PATH];
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
    snprintf(historyPath, MAX_PATH, "%s%s", exePath, g_midiFolderHistoryFile);

    FILE* file = fopen(historyPath, "w");
    if (file) {
        for (const auto& path : g_midiFolderHistory) {
            fprintf(file, "%s\n", path.c_str());
        }
        fclose(file);
    }
}

void LoadMIDIFolderHistory() {
    char historyPath[MAX_PATH];
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
    snprintf(historyPath, MAX_PATH, "%s%s", exePath, g_midiFolderHistoryFile);

    FILE* file = fopen(historyPath, "r");
    if (file) {
        char line[MAX_PATH];
        while (fgets(line, sizeof(line), file)) {
            // Remove newline
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }

            // Only add if folder still exists and contains MIDI files
            if (strlen(line) > 0) {
                WIN32_FIND_DATAW findData;
                std::wstring wPath = UTF8ToWide(line);
                HANDLE findHandle = FindFirstFileW(wPath.c_str(), &findData);
                if (findHandle != INVALID_HANDLE_VALUE) {
                    FindClose(findHandle);
                    if (ContainsMIDIFiles(line)) {
                        g_midiFolderHistory.push_back(line);
                    }
                }
            }
        }
        fclose(file);
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
    // Load MIDI folder history
    LoadMIDIFolderHistory();

    // Get exe directory as default path using wide character API
    wchar_t wExePath[MAX_PATH];
    GetModuleFileNameW(NULL, wExePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(wExePath, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    }

    std::string exePath = WideToUTF8(wExePath);
    NavigateToPath(exePath.c_str());
}

// ===== Global Media Keys Support =====

void RegisterGlobalMediaKeys() {
    if (!g_mainWindow) return;

    // Register hotkeys for media control
    // MOD_NOREPEAT prevents key repeat
    bool success = true;

    if (!RegisterHotKey(g_mainWindow, HK_PLAY_PAUSE, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE)) {
        log_command("Warning: Failed to register Play/Pause media key");
        success = false;
    }

    if (!RegisterHotKey(g_mainWindow, HK_NEXT_TRACK, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK)) {
        log_command("Warning: Failed to register Next Track media key");
        success = false;
    }

    if (!RegisterHotKey(g_mainWindow, HK_PREV_TRACK, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK)) {
        log_command("Warning: Failed to register Previous Track media key");
        success = false;
    }

    if (success) {
        log_command("Global media keys registered successfully");
    }
}

void UnregisterGlobalMediaKeys() {
    if (!g_mainWindow) return;

    UnregisterHotKey(g_mainWindow, HK_PLAY_PAUSE);
    UnregisterHotKey(g_mainWindow, HK_NEXT_TRACK);
    UnregisterHotKey(g_mainWindow, HK_PREV_TRACK);

    log_command("Global media keys unregistered");
}

// ===== Auto-Skip Silence Function =====

// Find the first note-on event (excluding drum channel)
// Returns the event index, and sets the tick value via parameter
int FindFirstNoteEvent(int& outTick) {
    outTick = 0;
    if (g_midiPlayer.currentFileName.empty()) return 0;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return 0;

    MidiEventList& track = g_midiPlayer.midiFile[0];

    for (int i = 0; i < (int)track.size(); i++) {
        MidiEvent& event = track[i];

        // Look for note-on events (not drum channel)
        if (event.isNoteOn() && event.getVelocity() > 0) {
            int channel = event.getChannel();
            // Skip drum channel (MIDI channel 10 = index 9)
            if (channel != 9) {
                outTick = event.tick;
                log_command("First note found at event index: %d, tick: %d", i, event.tick);
                return i;  // Return event index
            }
        }
    }

    // No note-on event found
    return 0;
}

// Calculate total MIDI duration in microseconds
double GetMIDITotalDuration() {
    if (g_midiPlayer.currentFileName.empty()) return 0.0;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return 0.0;

    MidiEventList& track = g_midiPlayer.midiFile[0];
    if (track.size() == 0) return 0.0;

    // Get the last event's tick
    int lastTick = track[track.size() - 1].tick;

    // Convert MIDI tick to microseconds
    double microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;
    return (double)lastTick * microsPerTick;
}

// Format time in microseconds to MM:SS format
std::string FormatTime(double microseconds) {
    int totalSeconds = (int)(microseconds / 1000000.0);
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
    return std::string(buffer);
}

// ===== MIDI Player Functions =====

bool LoadMIDIFile(const char* filename) {
    g_midiPlayer.midiFile.clear();

    // Convert UTF-8 path to wide string for Unicode support
    std::wstring wFilename = UTF8ToWide(filename);

#ifdef _WIN32
    // Add long path prefix if path is too long (> 260 chars)
    if (wFilename.length() > 260) {
        // Add \\?\ prefix for long path support
        if (wFilename.find(L"\\\\?\\") != 0) {
            wFilename = L"\\\\?\\" + wFilename;
        }
    }

    // Use wide string version for proper Unicode path support on Windows
    if (!g_midiPlayer.midiFile.read(wFilename)) {
        // Try to provide more helpful error message
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            log_command("ERROR: File not found: %s", filename);
        } else if (error == ERROR_PATH_NOT_FOUND) {
            log_command("ERROR: Path not found: %s", filename);
        } else if (error == ERROR_ACCESS_DENIED) {
            log_command("ERROR: Access denied: %s", filename);
        } else if (wFilename.length() > 260) {
            log_command("ERROR: Path too long (%d chars): %s", (int)strlen(filename), filename);
            log_command("Windows MAX_PATH limit is 260 characters. Please move the file to a shorter path.");
        } else {
            log_command("ERROR: Failed to load MIDI file (error %d): %s", error, filename);
        }
        return false;
    }
#else
    // On non-Windows platforms, use standard read method
    if (!g_midiPlayer.midiFile.read(filename)) {
        log_command("ERROR: Failed to load MIDI file: %s", filename);
        return false;
    }
#endif

    g_midiPlayer.currentFileName = filename;
    g_midiPlayer.currentTick = 0;
    g_midiPlayer.isPlaying = false;
    g_midiPlayer.isPaused = false;
    g_midiPlayer.ticksPerQuarterNote = g_midiPlayer.midiFile.getTicksPerQuarterNote();
    g_midiPlayer.tempo = 500000.0;  // Default tempo
    g_midiPlayer.activeNotes.clear();
    ResetPianoKeyStates();

    // Reset sustain pedal state when loading new file
    g_sustainPedalActive = false;

    // Make sure ticks are absolute
    g_midiPlayer.midiFile.makeAbsoluteTicks();
    g_midiPlayer.midiFile.joinTracks();  // Merge all tracks for easier playback

    int numEvents = g_midiPlayer.midiFile.getEventCount(0);
    log_command("=== MIDI File Loaded ===");
    log_command("File: %s", filename);
    log_command("Events: %d", numEvents);
    log_command("TPQ: %d", g_midiPlayer.ticksPerQuarterNote);

    // Analyze velocity distribution for dynamic mapping
    if (g_enableDynamicVelocityMapping) {
        AnalyzeVelocityDistribution();
    }

    return true;
}

// ===== Playlist Navigation Functions =====

int GetNextMIDIFileIndex() {
    // Build list of MIDI file indices
    std::vector<int> midiIndices;
    for (int i = 0; i < (int)g_fileList.size(); i++) {
        if (!g_fileList[i].isDirectory && !g_fileList[i].fullPath.empty()) {
            midiIndices.push_back(i);
        }
    }

    if (midiIndices.empty()) return -1;

    if (g_isSequentialPlayback) {
        // Sequential: find next MIDI file after current
        if (g_currentPlayingIndex < 0) {
            return midiIndices[0];  // Start from first
        }

        // Find current index in MIDI list
        auto it = std::find(midiIndices.begin(), midiIndices.end(), g_currentPlayingIndex);
        if (it != midiIndices.end()) {
            // Move to next, wrap around if at end
            it++;
            if (it == midiIndices.end()) {
                return midiIndices[0];  // Loop to first
            }
            return *it;
        }

        return midiIndices[0];  // Fallback to first
    } else {
        // Random: pick a random MIDI file (excluding current)
        static std::random_device rd;
        static std::mt19937 gen(rd());

        if (midiIndices.size() == 1) {
            return midiIndices[0];  // Only one file
        }

        // Exclude current playing file from random selection
        std::vector<int> candidates;
        for (int idx : midiIndices) {
            if (idx != g_currentPlayingIndex) {
                candidates.push_back(idx);
            }
        }

        if (candidates.empty()) {
            return midiIndices[0];
        }

        std::uniform_int_distribution<> dis(0, candidates.size() - 1);
        return candidates[dis(gen)];
    }
}

int GetPreviousMIDIFileIndex() {
    // Build list of MIDI file indices
    std::vector<int> midiIndices;
    for (int i = 0; i < (int)g_fileList.size(); i++) {
        if (!g_fileList[i].isDirectory && !g_fileList[i].fullPath.empty()) {
            midiIndices.push_back(i);
        }
    }

    if (midiIndices.empty()) return -1;

    if (g_isSequentialPlayback) {
        // Sequential: find previous MIDI file
        if (g_currentPlayingIndex < 0) {
            return midiIndices.empty() ? -1 : midiIndices.back();  // Start from last
        }

        // Find current index in MIDI list
        auto it = std::find(midiIndices.begin(), midiIndices.end(), g_currentPlayingIndex);
        if (it != midiIndices.end()) {
            // Move to previous, wrap around if at beginning
            if (it == midiIndices.begin()) {
                return midiIndices.back();  // Loop to last
            }
            it--;
            return *it;
        }

        return midiIndices.back();  // Fallback to last
    } else {
        // Random mode: same as next (pick random)
        return GetNextMIDIFileIndex();
    }
}

void PlayNextMIDI() {
    int nextIndex = GetNextMIDIFileIndex();
    if (nextIndex >= 0 && nextIndex < (int)g_fileList.size()) {
        g_currentPlayingIndex = nextIndex;
        g_selectedFileIndex = nextIndex;

        // Reset YM2163 chips to eliminate residual sound
        ResetAllYM2163Chips();

        // Initialize all channels to eliminate residual sound
        InitializeAllChannels();
        stop_all_notes();
        g_midiPlayer.activeNotes.clear();
        ResetPianoKeyStates();

        if (LoadMIDIFile(g_fileList[nextIndex].fullPath.c_str())) {
            // Ensure progress bar is reset to 0
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

        // Reset YM2163 chips to eliminate residual sound
        ResetAllYM2163Chips();

        // Initialize all channels to eliminate residual sound
        InitializeAllChannels();
        stop_all_notes();
        g_midiPlayer.activeNotes.clear();
        ResetPianoKeyStates();

        if (LoadMIDIFile(g_fileList[prevIndex].fullPath.c_str())) {
            // Ensure progress bar is reset to 0
            g_midiPlayer.currentTick = 0;
            g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
            PlayMIDI();
        }
    }
}

void PlayMIDI() {
    if (g_midiPlayer.currentFileName.empty()) return;

    if (g_midiPlayer.isPaused) {
        // Resume from pause
        g_midiPlayer.isPaused = false;
        auto now = std::chrono::steady_clock::now();
        g_midiPlayer.pausedDuration += std::chrono::duration_cast<std::chrono::milliseconds>(now - g_midiPlayer.pauseTime);
        log_command("MIDI playback resumed");
    } else {
        // Start from beginning
        g_midiPlayer.currentTick = 0;
        g_midiPlayer.isPlaying = true;
        g_midiPlayer.playStartTime = std::chrono::steady_clock::now();
        g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
        stop_all_notes();
        g_midiPlayer.activeNotes.clear();
        ResetPianoKeyStates();
        // Reset sustain pedal state when starting new playback
        g_sustainPedalActive = false;

        // Auto-skip silence at the beginning if enabled
        if (g_enableAutoSkipSilence) {
            int firstNoteTick = 0;
            int firstNoteIndex = FindFirstNoteEvent(firstNoteTick);

            // If there's silence before the first note (firstNoteIndex > 0), skip it
            if (firstNoteIndex > 0) {
                // Pre-process all control events before the first note
                // This ensures tempo and other control changes are applied correctly
                MidiEventList& track = g_midiPlayer.midiFile[0];

                for (int i = 0; i < firstNoteIndex; i++) {
                    MidiEvent& event = track[i];

                    // Process control events (tempo, program change, etc.)
                    if (event.isTempo()) {
                        g_midiPlayer.tempo = event.getTempoMicroseconds();
                    } else if (event.isController()) {
                        int controller = event[1];
                        int value = event[2];
                        if (controller == 64 && g_enableSustainPedal) {
                            g_sustainPedalActive = (value >= 64);
                        }
                    }
                    // Skip note events - we don't want to play them
                }

                // Set the current tick to first note INDEX (not tick value)
                g_midiPlayer.currentTick = firstNoteIndex;

                // Set accumulated time to match the first note's MIDI tick
                // This ensures the timing is correct when UpdateMIDIPlayback starts
                double microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;
                g_midiPlayer.accumulatedTime = (double)firstNoteTick * microsPerTick;

                log_command("Auto-skipped to event %d (MIDI tick: %d, time: %.2f ms)",
                           firstNoteIndex, firstNoteTick, g_midiPlayer.accumulatedTime / 1000.0);
            } else {
                // No silence to skip, start from beginning
                g_midiPlayer.accumulatedTime = 0.0;
            }
        } else {
            // Auto-skip disabled, start from beginning
            g_midiPlayer.accumulatedTime = 0.0;
        }

        // Initialize performance counter
        QueryPerformanceCounter(&g_midiPlayer.lastPerfCounter);
        QueryPerformanceFrequency(&g_midiPlayer.perfCounterFreq);

        log_command("MIDI playback started");
    }

    g_midiPlayer.isPlaying = true;
}

void PauseMIDI() {
    if (!g_midiPlayer.isPlaying || g_midiPlayer.isPaused) return;

    g_midiPlayer.isPaused = true;
    g_midiPlayer.pauseTime = std::chrono::steady_clock::now();
    stop_all_notes();
    log_command("MIDI playback paused");
}

void StopMIDI() {
    g_midiPlayer.isPlaying = false;
    g_midiPlayer.isPaused = false;
    g_midiPlayer.currentTick = 0;
    stop_all_notes();
    g_midiPlayer.activeNotes.clear();
    ResetPianoKeyStates();
    // Reset sustain pedal state when stopping playback
    g_sustainPedalActive = false;
    // Reset and initialize all YM2163 chips to eliminate residual sound
    ResetAllYM2163Chips();
    InitializeAllChannels();
    log_command("MIDI playback stopped");
}

// Rebuild active notes state after seeking
void RebuildActiveNotesAfterSeek(int targetTick) {
    if (g_midiPlayer.currentFileName.empty()) return;
    if (g_midiPlayer.midiFile.getEventCount(0) == 0) return;

    // Track which notes are on at the target tick by scanning backwards
    std::map<int, std::map<int, bool>> notesOn;  // channel -> note -> isOn

    MidiEventList& track = g_midiPlayer.midiFile[0];

    // Scan all events up to targetTick to find which notes should be playing
    for (int i = 0; i < track.size() && i < targetTick; i++) {
        MidiEvent& event = track[i];

        if (event.isNoteOn()) {
            int channel = event.getChannel();
            int note = event.getKeyNumber();
            int velocity = event.getVelocity();

            if (channel == 9) continue;  // Skip drum channel

            if (velocity > 0) {
                notesOn[channel][note] = true;
            } else {
                notesOn[channel][note] = false;
            }
        } else if (event.isNoteOff()) {
            int channel = event.getChannel();
            int note = event.getKeyNumber();
            notesOn[channel][note] = false;
        }
    }

    // Now replay all notes that should be active at this point
    for (auto& channelPair : notesOn) {
        int channel = channelPair.first;
        for (auto& notePair : channelPair.second) {
            int note = notePair.first;
            bool isOn = notePair.second;

            if (isOn) {
                // This note should be playing - start it
                    // Map MIDI note to YM2163 note/octave
                    int ymNote = note % 12;
                    int ymOctave = (note / 12) - 2;

                    // Auto-adjust octave if out of range
                    while (ymOctave < 0 || (ymOctave == 0 && ymNote < 11)) {
                        ymOctave++;
                    }

                    // v11: If Slot3 2MHz enabled, allow C8-B8 (octave 6), clamp above B8
                    if (g_enableSlot3_2MHz) {
                        while (ymOctave > 6) {
                            ymOctave--;
                        }
                    } else {
                        while (ymOctave > 5 || (ymOctave == 5 && ymNote > 11)) {
                            ymOctave--;
                        }
                    }

                    // v11: Route high octave to Slot3 based on range setting
                    int ymChannel;
                    bool onSlot3_2MHz = false;
                    bool isHighOctave = (g_slot3_2MHz_Range == 0) ? (ymOctave == 6) : (ymOctave >= 5 && ymOctave <= 6);

                    if (isHighOctave && g_enableSlot3_2MHz) {
                        ymChannel = find_free_channel_slot3();
                        onSlot3_2MHz = true;
                    } else if (ymOctave >= 2 && ymOctave <= 5 && g_enableSlot3_2MHz && g_enableSlot3Overflow && getNormalChannelUsage() >= 0.8f) {
                        ymChannel = find_free_channel_slot3();
                        if (ymChannel >= 0) {
                            onSlot3_2MHz = true;
                        } else {
                            ymChannel = find_free_channel();
                        }
                    } else {
                        ymChannel = find_free_channel();
                    }
                    if (ymChannel >= 0) {

                    // Choose instrument settings based on mode
                    int useWave, useEnvelope, useVolume;
                    if (g_useLiveControl) {
                        useWave = g_currentTimbre;
                        useEnvelope = g_currentEnvelope;
                        useVolume = g_currentVolume;
                    } else {
                        int program = 0;
                        if (g_instrumentConfigs.count(program) > 0) {
                            InstrumentConfig& config = g_instrumentConfigs[program];
                            useWave = config.wave;
                            useEnvelope = config.envelope;
                        } else {
                            useWave = 4;
                            useEnvelope = 0;
                        }
                        useVolume = g_currentVolume;
                    }

                    // Use default velocity for seek (no velocity mapping)
                    int defaultVelocity = 96;  // Default velocity for seek
                    if (g_enableVelocityMapping) {
                        useVolume = map_velocity_to_volume(defaultVelocity);
                    }

                    g_channels[ymChannel].midiChannel = channel;
                    play_note(ymChannel, ymNote, ymOctave, useWave, useEnvelope, useVolume);

                    // Update piano key visual with velocity info
                    int keyIdx = get_key_index(ymOctave, ymNote);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyPressed[keyIdx] = true;
                        g_pianoKeyVelocity[keyIdx] = defaultVelocity;  // Store default velocity for seek
                        g_pianoKeyOnSlot3_2MHz[keyIdx] = onSlot3_2MHz;
                    }

                    g_midiPlayer.activeNotes[channel][note] = ymChannel;
                }
            }
        }
    }
}

void UpdateMIDIPlayback() {
    if (!g_midiPlayer.isPlaying || g_midiPlayer.isPaused) return;
    if (g_midiPlayer.currentFileName.empty()) return;

    // High-precision timer (yasp-style optimization)
    // Use QueryPerformanceCounter for more accurate timing than chrono
    LARGE_INTEGER currentCounter;
    QueryPerformanceCounter(&currentCounter);

    double deltaTime = (double)(currentCounter.QuadPart - g_midiPlayer.lastPerfCounter.QuadPart) /
                       (double)g_midiPlayer.perfCounterFreq.QuadPart * 1000000.0;  // Convert to microseconds
    g_midiPlayer.lastPerfCounter = currentCounter;

    // Accumulate time for precise tick calculation
    g_midiPlayer.accumulatedTime += deltaTime;

    // Calculate current tick based on accumulated time
    double ticksPerMicrosecond = (double)g_midiPlayer.ticksPerQuarterNote / g_midiPlayer.tempo;
    int targetTick = (int)(g_midiPlayer.accumulatedTime * ticksPerMicrosecond);

    // Process all events up to current tick
    MidiEventList& track = g_midiPlayer.midiFile[0];

    while (g_midiPlayer.currentTick < track.size()) {
        MidiEvent& event = track[g_midiPlayer.currentTick];

        if (event.tick > targetTick) {
            break;  // Haven't reached this event yet
        }

        // Process this event
        if (event.isNoteOn()) {
            int channel = event.getChannel();
            int note = event.getKeyNumber();
            int velocity = event.getVelocity();

            if (velocity > 0) {
                // Check if this is a drum channel (MIDI channel 10 = index 9)
                if (channel == 9) {
                    // Drum event - map MIDI drum note to YM2163 drum
                    if (g_drumConfigs.count(note) > 0) {
                        DrumConfig& drumConfig = g_drumConfigs[note];
                        // Trigger all mapped drums
                        uint8_t drumBits = 0;
                        for (uint8_t bit : drumConfig.drumBits) {
                            drumBits |= bit;
                        }
                        play_drum(drumBits);
                    }
                } else {
                    // Note on - melody channel
                    // Map MIDI note to YM2163 note/octave (降低一个八度)
                    int ymNote = note % 12;
                    int ymOctave = (note / 12) - 2;  // MIDI octave starts at C-1, 降低一个八度

                    // Auto-adjust octave if out of range
                    while (ymOctave < 0 || (ymOctave == 0 && ymNote < 11)) {
                        ymOctave++;  // Move up one octave
                    }

                    // v11: If Slot3 2MHz enabled, allow C8-B8 (octave 6), clamp above B8
                    if (g_enableSlot3_2MHz) {
                        while (ymOctave > 6) {
                            ymOctave--;
                        }
                    } else {
                        while (ymOctave > 5 || (ymOctave == 5 && ymNote > 11)) {
                            ymOctave--;  // Move down one octave
                        }
                    }

                    // v11: Route high octave to Slot3 based on range setting
                    int ymChannel;
                    bool onSlot3_2MHz = false;
                    bool isHighOctave = (g_slot3_2MHz_Range == 0) ? (ymOctave == 6) : (ymOctave >= 5 && ymOctave <= 6);

                    if (isHighOctave && g_enableSlot3_2MHz) {
                        ymChannel = find_free_channel_slot3();
                        onSlot3_2MHz = true;
                    } else if (ymOctave >= 2 && ymOctave <= 5 && g_enableSlot3_2MHz && g_enableSlot3Overflow && getNormalChannelUsage() >= 0.8f) {
                        ymChannel = find_free_channel_slot3();
                        if (ymChannel >= 0) {
                            onSlot3_2MHz = true;
                        } else {
                            ymChannel = find_free_channel();
                        }
                    } else {
                        ymChannel = find_free_channel();
                    }
                    if (ymChannel >= 0) {

                    // Choose instrument settings based on mode
                    int useWave, useEnvelope, useVolume;
                    int usePedalMode = g_pedalMode;  // Default to global pedal mode

                    if (g_useLiveControl) {
                        // Live Control Mode: use UI settings
                        useWave = g_currentTimbre;
                        useEnvelope = g_currentEnvelope;
                        useVolume = g_currentVolume;
                    } else {
                        // Config Mode: use instrument config from MIDI program
                        int program = 0;  // TODO: Track program changes per channel
                        if (g_instrumentConfigs.count(program) > 0) {
                            InstrumentConfig& config = g_instrumentConfigs[program];
                            useWave = config.wave;
                            useEnvelope = config.envelope;
                            // Use per-instrument pedal mode if specified (non-zero), otherwise use global
                            if (config.pedalMode != 0) {
                                usePedalMode = config.pedalMode;
                            }
                        } else {
                            // Fallback to default (Piano with Decay)
                            useWave = 4;
                            useEnvelope = 0;
                        }
                        useVolume = g_currentVolume;
                    }

                    // Map velocity to volume if enabled
                    if (g_enableVelocityMapping) {
                        useVolume = map_velocity_to_volume(velocity);
                    }

                    // Map pedal mode to envelope if enabled
                    if (usePedalMode == 1) {
                        // Piano Pedal: Fast when pedal down, Decay when pedal up
                        if (g_sustainPedalActive) {
                            useEnvelope = 1;  // Fast envelope when pedal is down
                        } else {
                            useEnvelope = 0;  // Decay envelope when pedal is up
                        }
                    } else if (usePedalMode == 2) {
                        // Organ Pedal: Slow when pedal down, Medium when pedal up
                        if (g_sustainPedalActive) {
                            useEnvelope = 3;  // Slow envelope when pedal is down
                        } else {
                            useEnvelope = 2;  // Medium envelope when pedal is up
                        }
                    }

                    g_channels[ymChannel].midiChannel = channel;
                    play_note(ymChannel, ymNote, ymOctave, useWave, useEnvelope, useVolume);

                    // Update piano key visual with velocity info
                    int keyIdx = get_key_index(ymOctave, ymNote);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyPressed[keyIdx] = true;
                        g_pianoKeyVelocity[keyIdx] = velocity;  // Store velocity
                        g_pianoKeyFromKeyboard[keyIdx] = false;  // MIDI source, not keyboard
                        g_pianoKeyOnSlot3_2MHz[keyIdx] = onSlot3_2MHz;
                    }

                    g_midiPlayer.activeNotes[channel][note] = ymChannel;
                }
                }  // End of melody channel handling
            } else {
                // Note off (velocity 0)
                if (g_midiPlayer.activeNotes[channel].count(note) > 0) {
                    int ymChannel = g_midiPlayer.activeNotes[channel][note];

                    // Clear piano key visual
                    if (g_channels[ymChannel].active) {
                        int keyIdx = get_key_index(g_channels[ymChannel].octave, g_channels[ymChannel].note);
                        if (keyIdx >= 0 && keyIdx < 73) {
                            g_pianoKeyPressed[keyIdx] = false;
                            g_pianoKeyVelocity[keyIdx] = 0;  // Clear velocity
                        }
                    }

                    stop_note(ymChannel);
                    g_midiPlayer.activeNotes[channel].erase(note);
                }
            }
        } else if (event.isNoteOff()) {
            int channel = event.getChannel();
            int note = event.getKeyNumber();

            if (g_midiPlayer.activeNotes[channel].count(note) > 0) {
                int ymChannel = g_midiPlayer.activeNotes[channel][note];

                // Clear piano key visual
                if (g_channels[ymChannel].active) {
                    int keyIdx = get_key_index(g_channels[ymChannel].octave, g_channels[ymChannel].note);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyPressed[keyIdx] = false;
                    }
                }

                stop_note(ymChannel);
                g_midiPlayer.activeNotes[channel].erase(note);
            }
        } else if (event.isTempo()) {
            // Get tempo in microseconds per quarter note
            g_midiPlayer.tempo = event.getTempoMicroseconds();
            // Recalculate accumulated time based on current tick
            g_midiPlayer.accumulatedTime = (double)g_midiPlayer.currentTick * g_midiPlayer.tempo / g_midiPlayer.ticksPerQuarterNote;
        } else if (event.isController()) {
            // Handle MIDI Control Change messages
            int controller = event[1];
            int value = event[2];

            // CC64: Sustain Pedal
            if (controller == 64 && g_pedalMode > 0) {
                g_sustainPedalActive = (value >= 64);
            }
        }

        g_midiPlayer.currentTick++;
    }

    // Check if playback finished
    if (g_midiPlayer.currentTick >= track.size()) {
        StopMIDI();
        log_command("MIDI playback finished");

        // Auto-play next track if enabled
        if (g_autoPlayNext) {
            PlayNextMIDI();
        }
    }
}

// ===== Keyboard Mapping =====

typedef struct {
    int vk;
    int note;
    int octave_offset;
} KeyMapping;

static KeyMapping g_keyMappings[] = {
    {'Z', 0, 0}, {'X', 2, 0}, {'C', 4, 0}, {'V', 5, 0}, {'B', 7, 0}, {'N', 9, 0}, {'M', 11, 0},
    {VK_OEM_COMMA, 0, 1}, {VK_OEM_PERIOD, 2, 1}, {VK_OEM_2, 4, 1},
    {'S', 1, 0}, {'D', 3, 0}, {'G', 6, 0}, {'H', 8, 0}, {'J', 10, 0}, {'K', 1, 1}, {'L', 3, 1},
    {'Q', 0, 1}, {'W', 2, 1}, {'E', 4, 1}, {'R', 5, 1}, {'T', 7, 1}, {'Y', 9, 1}, {'U', 11, 1},
    {'I', 0, 2}, {'O', 2, 2}, {'P', 4, 2}, {VK_OEM_4, 5, 2}, {VK_OEM_6, 7, 2},
    {'2', 1, 1}, {'3', 3, 1}, {'5', 6, 1}, {'6', 8, 1}, {'7', 10, 1}, {'8', 1, 2}, {'9', 3, 2}, {'0', 6, 2},
};

static const int g_numKeyMappings = sizeof(g_keyMappings) / sizeof(KeyMapping);

// ===== ImGui UI Functions =====

void RenderMIDIPlayer() {
    ImGui::Text("MIDI Player");
    ImGui::Separator();

    // Playback controls (at top)
    float buttonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
    if (ImGui::Button("Play", ImVec2(buttonWidth, 30))) {
        PlayMIDI();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(buttonWidth, 30))) {
        PauseMIDI();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(buttonWidth, 30))) {
        StopMIDI();
    }

    // Playlist navigation
    float navButtonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
    if (ImGui::Button("<< Prev", ImVec2(navButtonWidth, 25))) {
        PlayPreviousMIDI();
    }
    ImGui::SameLine();
    if (ImGui::Button("Next >>", ImVec2(navButtonWidth, 25))) {
        PlayNextMIDI();
    }

    // Playback mode and options
    ImGui::Checkbox("Auto-play next", &g_autoPlayNext);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically play next track when current finishes");
    }

    ImGui::SameLine();

    const char* modeText = g_isSequentialPlayback ? "Sequential" : "Random";
    if (ImGui::Button(modeText, ImVec2(85, 0))) {
        g_isSequentialPlayback = !g_isSequentialPlayback;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click to toggle: Sequential (loop) / Random");
    }

    ImGui::Spacing();

    // Status and current file
    if (g_midiPlayer.isPlaying && !g_midiPlayer.isPaused) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Playing:");
    } else if (g_midiPlayer.isPaused) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused:");
    } else {
        ImGui::Text("Ready:");
    }

    ImGui::SameLine();

    if (!g_midiPlayer.currentFileName.empty()) {
        const char* filename = strrchr(g_midiPlayer.currentFileName.c_str(), '\\');
        if (!filename) filename = strrchr(g_midiPlayer.currentFileName.c_str(), '/');
        if (!filename) filename = g_midiPlayer.currentFileName.c_str();
        else filename++;
        ImGui::Text("%s", filename);
    } else {
        ImGui::TextDisabled("No file loaded");
    }

    // Progress bar with time display (clickable)
    if (!g_midiPlayer.currentFileName.empty() && g_midiPlayer.midiFile.getEventCount(0) > 0) {
        // Calculate current time and total duration based on MIDI ticks
        MidiEventList& track = g_midiPlayer.midiFile[0];

        // Get last MIDI tick (total duration)
        int lastMidiTick = 0;
        if (track.size() > 0) {
            lastMidiTick = track[track.size() - 1].tick;
        }

        // Convert MIDI ticks to microseconds
        double microsPerTick = g_midiPlayer.tempo / (double)g_midiPlayer.ticksPerQuarterNote;
        double totalTimeMicros = (double)lastMidiTick * microsPerTick;

        // Calculate progress based on accumulated time (completely time-driven, not event-driven)
        float progress = (totalTimeMicros > 0) ? (float)(g_midiPlayer.accumulatedTime / totalTimeMicros) : 0.0f;
        progress = (progress < 0.0f) ? 0.0f : (progress > 1.0f ? 1.0f : progress);

        // Convert accumulated time to display format
        double currentTimeMicros = g_midiPlayer.accumulatedTime;
        int currentSeconds = (int)(currentTimeMicros / 1000000.0);
        int currentMinutes = currentSeconds / 60;
        currentSeconds %= 60;

        int totalSeconds = (int)(totalTimeMicros / 1000000.0);
        int totalMinutes = totalSeconds / 60;
        totalSeconds %= 60;

        char currentTimeStr[32], totalTimeStr[32];
        sprintf(currentTimeStr, "%02d:%02d", currentMinutes, currentSeconds);
        sprintf(totalTimeStr, "%02d:%02d", totalMinutes, totalSeconds);

        // Display time information above progress bar
        ImGui::Text("%s / %s", currentTimeStr, totalTimeStr);

        ImVec2 progressPos = ImGui::GetCursorScreenPos();
        ImVec2 progressSize = ImVec2(ImGui::GetContentRegionAvail().x, 20);

        ImGui::ProgressBar(progress, progressSize, "");

        // Handle progress bar click to seek
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float clickPos = (mousePos.x - progressPos.x) / progressSize.x;
            clickPos = clickPos < 0.0f ? 0.0f : (clickPos > 1.0f ? 1.0f : clickPos);

            // Calculate target MIDI tick based on time
            int targetMidiTick = (int)(clickPos * lastMidiTick);

            // Find the event index that corresponds to this MIDI tick
            int targetEventIndex = 0;
            for (int i = 0; i < (int)track.size(); i++) {
                if (track[i].tick >= targetMidiTick) {
                    targetEventIndex = i;
                    break;
                }
            }

            g_midiPlayer.currentTick = targetEventIndex;

            // Remember if we were playing before seek
            bool wasPlaying = g_midiPlayer.isPlaying && !g_midiPlayer.isPaused;

            // Stop all currently playing notes before seek
            stop_all_notes();
            g_midiPlayer.activeNotes.clear();
            ResetPianoKeyStates();

            // Reset high-precision timer and accumulated time
            QueryPerformanceCounter(&g_midiPlayer.lastPerfCounter);

            // Calculate accumulated time based on target MIDI tick
            double ticksPerMicrosecond = (double)g_midiPlayer.ticksPerQuarterNote / g_midiPlayer.tempo;
            g_midiPlayer.accumulatedTime = targetMidiTick / ticksPerMicrosecond;

            // Recalculate timing based on actual MIDI tick value
            auto now = std::chrono::steady_clock::now();
            if (wasPlaying) {
                // If playing, adjust playStartTime to maintain playing state
                g_midiPlayer.playStartTime = now - std::chrono::microseconds((int)(targetMidiTick * microsPerTick));
                g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
            } else if (g_midiPlayer.isPaused) {
                // If paused, update pauseTime to new position
                g_midiPlayer.playStartTime = now - std::chrono::microseconds((int)(targetMidiTick * microsPerTick));
                g_midiPlayer.pauseTime = now;
                g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
            }

            // v11: Don't rebuild active notes after seek to avoid loud burst
            // Let normal UpdateMIDIPlayback handle notes from the new position

            log_command("Seek to progress: %.1f%% (time: %s)", clickPos * 100.0f, currentTimeStr);
        }
    } else {
        ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("File Browser");
    ImGui::Separator();

    // Navigation bar
    if (ImGui::Button("<", ImVec2(25, 0))) {
        NavigateBack();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");

    ImGui::SameLine();

    if (ImGui::Button(">", ImVec2(25, 0))) {
        NavigateForward();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");

    ImGui::SameLine();

    // Up arrow button (navigate to parent directory)
    if (ImGui::Button("^", ImVec2(25, 0))) {
        NavigateToParent();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");

    ImGui::SameLine();

    // Win11-style address bar: breadcrumb buttons or text input
    if (!g_pathEditMode) {
        // Breadcrumb mode: show path segments as clickable buttons
        // Priority: show rightmost (current) directory, truncate left with "..." if needed

        float availWidth = ImGui::GetContentRegionAvail().x;
        std::vector<std::string> segments = SplitPath(g_currentPath);

        // Calculate button widths more accurately
        std::vector<float> buttonWidths;
        std::vector<std::string> accumulatedPaths;
        std::string accumulatedPath;

        // Get style parameters for accurate width calculation
        ImGuiStyle& style = ImGui::GetStyle();
        float framePaddingX = style.FramePadding.x;
        float itemSpacingX = style.ItemSpacing.x;
        float buttonBorderSize = style.FrameBorderSize;

        for (size_t i = 0; i < segments.size(); i++) {
            // Build accumulated path for this segment
            if (i == 0) {
                accumulatedPath = segments[i];
            } else {
                if (accumulatedPath.back() != '\\') {
                    accumulatedPath += "\\";
                }
                accumulatedPath += segments[i];
            }
            accumulatedPaths.push_back(accumulatedPath);

            // Calculate button width accurately:
            // text width + left padding + right padding + border + extra safety margin
            ImVec2 textSize = ImGui::CalcTextSize(segments[i].c_str());
            float buttonWidth = textSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;  // +4 for safety
            buttonWidths.push_back(buttonWidth);
        }

        // Calculate separator width (includes spacing on both sides)
        ImVec2 separatorTextSize = ImGui::CalcTextSize(">");
        float separatorWidth = separatorTextSize.x + itemSpacingX * 2.0f;

        // Calculate ellipsis button width
        ImVec2 ellipsisTextSize = ImGui::CalcTextSize("...");
        float ellipsisButtonWidth = ellipsisTextSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
        float ellipsisWidth = ellipsisButtonWidth + separatorWidth;

        // Reserve some safety margin from available width to prevent overflow
        float safeAvailWidth = availWidth - 10.0f;  // Reserve 10px safety margin

        // Determine which segments to show (from right to left)
        // Priority: always show the rightmost (current) directory
        int firstVisibleSegment = 0;

        if (segments.empty()) {
            firstVisibleSegment = 0;
        } else {
            // Start with the last segment (always visible)
            float usedWidth = buttonWidths[segments.size() - 1];
            firstVisibleSegment = segments.size() - 1;

            // Try to add more segments from right to left
            for (int i = segments.size() - 2; i >= 0; i--) {
                float segmentWidth = buttonWidths[i] + separatorWidth;

                // Check if we need ellipsis
                if (i > 0 && usedWidth + segmentWidth > safeAvailWidth) {
                    // Need to truncate, check if we have space for ellipsis
                    if (usedWidth + ellipsisWidth <= safeAvailWidth) {
                        // Can show ellipsis, current firstVisibleSegment is correct
                        break;
                    } else {
                        // Not enough space even for ellipsis, keep current firstVisibleSegment
                        break;
                    }
                } else if (i == 0 && usedWidth + segmentWidth > safeAvailWidth) {
                    // First segment doesn't fit, will show ellipsis
                    break;
                } else {
                    // This segment fits, include it
                    usedWidth += segmentWidth;
                    firstVisibleSegment = i;
                }
            }
        }

        // Create an invisible button covering the entire address bar area for click detection
        ImVec2 barStartPos = ImGui::GetCursorScreenPos();
        float barHeight = ImGui::GetFrameHeight();

        // Render breadcrumb bar
        ImGui::BeginGroup();

        // Show ellipsis if we're truncating
        if (firstVisibleSegment > 0) {
            if (ImGui::Button("...##ellipsis")) {
                // Click on ellipsis shows the full path in edit mode
                g_pathEditMode = true;
                g_pathEditModeJustActivated = true;
                strcpy(g_pathInput, g_currentPath);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", g_currentPath);
            }
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }

        // Render visible segments
        for (size_t i = firstVisibleSegment; i < segments.size(); i++) {
            if (i > firstVisibleSegment) {
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
            }

            // Truncate long folder names for display
            std::string displayName = TruncateFolderName(segments[i], 20);

            // Create button for this segment
            char buttonId[256];
            snprintf(buttonId, sizeof(buttonId), "%s##seg%d", displayName.c_str(), (int)i);

            if (ImGui::Button(buttonId)) {
                NavigateToPath(accumulatedPaths[i].c_str());
            }

            // Show full name in tooltip if truncated
            if (displayName != segments[i] && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", segments[i].c_str());
            }
        }

        ImGui::EndGroup();

        // Get the actual size of the rendered breadcrumb bar
        ImVec2 barEndPos = ImGui::GetItemRectMax();

        // Draw an invisible button over the right side empty space for Win11-style click to edit
        // Calculate the empty space on the right side
        float usedBarWidth = barEndPos.x - barStartPos.x;
        float emptySpaceWidth = availWidth - usedBarWidth;

        if (emptySpaceWidth > 10.0f) {  // Only show clickable area if there's enough space
            ImGui::SetCursorScreenPos(ImVec2(barEndPos.x, barStartPos.y));
            ImGui::InvisibleButton("##AddressBarEmptySpace", ImVec2(emptySpaceWidth, barHeight));

            // Win11-style: single click on empty space to enter edit mode
            if (ImGui::IsItemClicked(0)) {
                g_pathEditMode = true;
                g_pathEditModeJustActivated = true;
                strcpy(g_pathInput, g_currentPath);
            }
        }
    } else {
        // Edit mode: show text input
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##PathInput", g_pathInput, MAX_PATH, ImGuiInputTextFlags_EnterReturnsTrue)) {
            NavigateToPath(g_pathInput);
            g_pathEditMode = false;
            g_pathEditModeJustActivated = false;
        }

        // Exit edit mode on Escape or focus loss (but not on the first frame when just activated)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_pathEditMode = false;
            g_pathEditModeJustActivated = false;
            strcpy(g_pathInput, g_currentPath);  // Restore current path
        } else if (!g_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            // Only exit on focus loss if not just activated
            g_pathEditMode = false;
            strcpy(g_pathInput, g_currentPath);  // Restore current path
        }

        // Auto-focus the input when entering edit mode
        if (g_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            g_pathEditModeJustActivated = false;  // Clear the flag after setting focus
        }
    }

    // File list
    ImGui::BeginChild("FileList", ImVec2(-1, 0), true);

    // Save current scroll position for this path (every frame)
    std::string currentPathStr(g_currentPath);
    if (strlen(g_currentPath) > 0) {
        g_pathScrollPositions[currentPathStr] = ImGui::GetScrollY();
    }

    // Restore scroll position if we have one saved for this path (only once after navigation)
    static std::string lastRestoredPath;
    if (currentPathStr != lastRestoredPath && g_pathScrollPositions.count(currentPathStr) > 0) {
        ImGui::SetScrollY(g_pathScrollPositions[currentPathStr]);
        lastRestoredPath = currentPathStr;
    }

    for (int i = 0; i < (int)g_fileList.size(); i++) {
        const FileEntry& entry = g_fileList[i];

        bool isSelected = (g_selectedFileIndex == i);

        // Check if this folder is the one we just exited from (persistent highlight)
        bool isExitedFolder = (!g_lastExitedFolder.empty() && entry.isDirectory && entry.name == g_lastExitedFolder);

        // Check if this is the folder containing the currently playing file, or a parent folder in the path
        bool isPlayingPath = false;
        if (!g_currentPlayingFilePath.empty() && entry.isDirectory) {
            // Check if the playing file is in this directory or a subdirectory
            std::string entryPath = entry.fullPath;
            if (entryPath.back() != '\\') entryPath += "\\";
            if (g_currentPlayingFilePath.find(entryPath) == 0) {
                isPlayingPath = true;
            }
        }

        // Build label with icon prefix using std::string to properly handle UTF-8
        std::string label;
        if (entry.name == "..") {
            label = "[UP] " + entry.name;
        } else if (entry.isDirectory) {
            label = "[DIR] " + entry.name;
        } else {
            label = entry.name;
        }

        // Apply highlight colors
        if (isExitedFolder) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));  // Yellow highlight for exited folder
        } else if (isPlayingPath) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 1.0f, 1.0f));  // Light blue for playing path
        }

        // Check if text is too long and needs scrolling
        ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        float availWidth = ImGui::GetContentRegionAvail().x;
        bool needsScrolling = textSize.x > availWidth;

        // Track hover state
        bool isHovered = false;

        // Enable scrolling for selected, exited folder, or hovered items
        if (needsScrolling) {
            // Use custom rendering for scrolling text
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            ImVec2 itemSize = ImVec2(availWidth, ImGui::GetTextLineHeightWithSpacing());

            // Invisible button for interaction
            ImGui::InvisibleButton(("##item" + std::to_string(i)).c_str(), itemSize);
            isHovered = ImGui::IsItemHovered();

            if (ImGui::IsItemClicked()) {
                g_selectedFileIndex = i;

                // Single-click handling
                if (entry.name == "..") {
                    NavigateToParent();
                } else if (entry.isDirectory) {
                    g_lastExitedFolder.clear();
                    NavigateToPath(entry.fullPath.c_str());
                } else {
                    g_currentPlayingIndex = i;
                    g_currentPlayingFilePath = entry.fullPath;
                    ResetAllYM2163Chips();
                    InitializeAllChannels();
                    stop_all_notes();
                    g_midiPlayer.activeNotes.clear();
                    ResetPianoKeyStates();
                    if (LoadMIDIFile(entry.fullPath.c_str())) {
                        g_midiPlayer.currentTick = 0;
                        g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
                        PlayMIDI();
                    }
                }
            }

            // Only animate scrolling if item is selected, exited folder, or hovered
            bool shouldScroll = (isSelected || isExitedFolder || isHovered);

            if (shouldScroll) {
                // Initialize scroll state if needed
                if (g_textScrollStates.count(i) == 0) {
                    TextScrollState state;
                    state.scrollOffset = 0.0f;
                    state.scrollDirection = 1.0f;
                    state.pauseTimer = 1.0f;
                    state.lastUpdateTime = std::chrono::steady_clock::now();
                    g_textScrollStates[i] = state;
                }

                TextScrollState& scrollState = g_textScrollStates[i];
                auto now = std::chrono::steady_clock::now();
                float deltaTime = std::chrono::duration<float>(now - scrollState.lastUpdateTime).count();
                scrollState.lastUpdateTime = now;

                // Update scroll animation
                if (scrollState.pauseTimer > 0.0f) {
                    scrollState.pauseTimer -= deltaTime;
                } else {
                    float scrollSpeed = 30.0f;  // pixels per second
                    scrollState.scrollOffset += scrollState.scrollDirection * scrollSpeed * deltaTime;

                    float maxScroll = textSize.x - availWidth + 20.0f;
                    if (scrollState.scrollOffset >= maxScroll) {
                        scrollState.scrollOffset = maxScroll;
                        scrollState.scrollDirection = -1.0f;
                        scrollState.pauseTimer = 1.0f;
                    } else if (scrollState.scrollOffset <= 0.0f) {
                        scrollState.scrollOffset = 0.0f;
                        scrollState.scrollDirection = 1.0f;
                        scrollState.pauseTimer = 1.0f;
                    }
                }

                // Draw background for selected item
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                if (isSelected) {
                    ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Header);
                    drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), bgColor);
                } else if (isHovered) {
                    ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
                    drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), bgColor);
                }

                // Clip text rendering
                drawList->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), true);
                ImVec2 textPos = ImVec2(cursorPos.x - scrollState.scrollOffset, cursorPos.y);
                ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
                drawList->AddText(textPos, textColor, label.c_str());
                drawList->PopClipRect();
            } else {
                // Not scrolling, just draw static text
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                if (isSelected) {
                    ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Header);
                    drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), bgColor);
                } else if (isHovered) {
                    ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
                    drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), bgColor);
                }

                // Draw text without scrolling
                ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
                drawList->AddText(cursorPos, textColor, label.c_str());

                // Reset scroll state when not scrolling
                if (g_textScrollStates.count(i) > 0) {
                    g_textScrollStates.erase(i);
                }
            }

        } else {
            // Normal selectable for short text
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                g_selectedFileIndex = i;

                // Single-click handling (unified with history folder behavior)
                if (entry.name == "..") {
                    NavigateToParent();
                } else if (entry.isDirectory) {
                    // Clear the exited folder highlight when entering any folder
                    g_lastExitedFolder.clear();
                    NavigateToPath(entry.fullPath.c_str());
                } else {
                    // Load MIDI file and start playing immediately
                    g_currentPlayingIndex = i;  // Update current playing index
                    g_currentPlayingFilePath = entry.fullPath;  // Store full path of playing file

                    // Reset YM2163 chips to eliminate residual sound
                    ResetAllYM2163Chips();

                    // Initialize all channels
                    InitializeAllChannels();
                    stop_all_notes();
                    g_midiPlayer.activeNotes.clear();
                    ResetPianoKeyStates();

                    if (LoadMIDIFile(entry.fullPath.c_str())) {
                        // Reset progress bar
                        g_midiPlayer.currentTick = 0;
                        g_midiPlayer.pausedDuration = std::chrono::milliseconds(0);
                        PlayMIDI();  // Auto-play on single-click
                    }
                }
            }
            isHovered = ImGui::IsItemHovered();
        }

        // Track hovered item
        if (isHovered) {
            g_hoveredFileIndex = i;
        }

        if (isExitedFolder || isPlayingPath) {
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();
}

void RenderPianoKeyboard() {
    ImGui::BeginChild("Piano", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    float whiteKeyWidth = 20.0f;
    float whiteKeyHeight = 100.0f;
    float blackKeyWidth = 12.0f;
    float blackKeyHeight = 60.0f;

    // Calculate piano width and center offset
    // v11: Add 7 white keys for C8-B8 octave when Slot3 2MHz is enabled
    int c8WhiteKeys = g_enableSlot3_2MHz ? 7 : 0;  // C D E F G A B = 7 white keys
    int totalWhiteKeys = 36 + c8WhiteKeys;  // B2 + 5 octaves (C3-B7) + optional C8-B8
    float pianoWidth = totalWhiteKeys * whiteKeyWidth;
    float availWidth = ImGui::GetContentRegionAvail().x;
    float centerOffset = (availWidth > pianoWidth) ? (availWidth - pianoWidth) * 0.5f : 0.0f;

    int whiteKeyCount = 0;

    // Draw B2
    {
        int keyIdx = 0;
        float x = p.x + centerOffset + whiteKeyCount * whiteKeyWidth;
        float y = p.y;

        ImU32 color;
        if (g_pianoKeyPressed[keyIdx]) {
            if (g_pianoKeyFromKeyboard[keyIdx] || g_pianoKeyOnSlot3_2MHz[keyIdx]) {
                // Green for keyboard input or Slot3 2MHz
                int velocity = g_pianoKeyVelocity[keyIdx];
                float intensity = velocity / 127.0f;  // 0.0 to 1.0
                int r = (int)(50 + 155 * intensity);   // 50 to 205
                int g = 255;  // Full green
                int b = (int)(100 + 100 * intensity);  // 100 to 200
                color = IM_COL32(r, g, b, 255);
            } else {
                // Blue for MIDI playback
                int velocity = g_pianoKeyVelocity[keyIdx];
                float intensity = velocity / 127.0f;  // 0.0 to 1.0
                int r = (int)(50 + 150 * intensity);  // 50 to 200
                int g = (int)(100 + 155 * intensity); // 100 to 255
                int b = 255;
                color = IM_COL32(r, g, b, 255);
            }
        } else {
            color = IM_COL32(255, 255, 255, 255);
        }

        draw_list->AddRectFilled(ImVec2(x, y),
            ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), color);
        draw_list->AddRect(ImVec2(x, y),
            ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), IM_COL32(0, 0, 0, 255));

        draw_list->AddText(ImVec2(x + 2, y + whiteKeyHeight - 18),
            IM_COL32(0, 0, 0, 255), "B2");

        whiteKeyCount++;
    }

    // Draw C3-B7 white keys
    for (int octave = 1; octave <= 5; octave++) {
        for (int note = 0; note <= 11; note++) {
            if (g_isBlackNote[note]) continue;

            int keyIdx = get_key_index(octave, note);
            if (keyIdx < 0) continue;

            float x = p.x + centerOffset + whiteKeyCount * whiteKeyWidth;
            float y = p.y;

            ImU32 color;
            if (g_pianoKeyPressed[keyIdx]) {
                if (g_pianoKeyFromKeyboard[keyIdx] || g_pianoKeyOnSlot3_2MHz[keyIdx]) {
                    // Green for keyboard input or Slot3 2MHz
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;  // 0.0 to 1.0
                    int r = (int)(50 + 155 * intensity);   // 50 to 205
                    int g = 255;  // Full green
                    int b = (int)(100 + 100 * intensity);  // 100 to 200
                    color = IM_COL32(r, g, b, 255);
                } else {
                    // Blue for MIDI playback
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;  // 0.0 to 1.0
                    int r = (int)(50 + 150 * intensity);  // 50 to 200
                    int g = (int)(100 + 155 * intensity); // 100 to 255
                    int b = 255;
                    color = IM_COL32(r, g, b, 255);
                }
            } else {
                color = IM_COL32(255, 255, 255, 255);
            }

            draw_list->AddRectFilled(ImVec2(x, y),
                ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y),
                ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), IM_COL32(0, 0, 0, 255));

            if (note == 0) {
                char label[8];
                sprintf(label, "C%d", octave + 2);
                draw_list->AddText(ImVec2(x + 2, y + whiteKeyHeight - 18),
                    IM_COL32(0, 0, 0, 255), label);
            }

            whiteKeyCount++;
        }
    }

    // v11: Draw C8-B8 white keys (when Slot3 2MHz enabled)
    if (g_enableSlot3_2MHz) {
        for (int note = 0; note <= 11; note++) {
            if (g_isBlackNote[note]) continue;

            int keyIdx = get_key_index(6, note);  // octave 6 = C8-B8
            if (keyIdx < 0) continue;

            float x = p.x + centerOffset + whiteKeyCount * whiteKeyWidth;
            float y = p.y;

            ImU32 color;
            if (g_pianoKeyPressed[keyIdx]) {
                if (g_pianoKeyFromKeyboard[keyIdx] || g_pianoKeyOnSlot3_2MHz[keyIdx]) {
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;
                    int r = (int)(50 + 155 * intensity);
                    int g = 255;
                    int b = (int)(100 + 100 * intensity);
                    color = IM_COL32(r, g, b, 255);
                } else {
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;
                    int r = (int)(50 + 150 * intensity);
                    int g = (int)(100 + 155 * intensity);
                    int b = 255;
                    color = IM_COL32(r, g, b, 255);
                }
            } else {
                // Slightly different tint for 2MHz octave keys
                color = IM_COL32(255, 250, 230, 255);
            }

            draw_list->AddRectFilled(ImVec2(x, y),
                ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y),
                ImVec2(x + whiteKeyWidth, y + whiteKeyHeight), IM_COL32(0, 0, 0, 255));

            if (note == 0) {
                draw_list->AddText(ImVec2(x + 2, y + whiteKeyHeight - 18),
                    IM_COL32(0, 0, 0, 255), "C8");
            }

            whiteKeyCount++;
        }
    }

    // Draw black keys
    whiteKeyCount = 1;
    for (int octave = 1; octave <= 5; octave++) {
        for (int note = 0; note <= 11; note++) {
            if (!g_isBlackNote[note]) continue;

            int keyIdx = get_key_index(octave, note);
            if (keyIdx < 0) continue;

            int whiteKeyIdx = 0;
            int blackOffset = 14;

            if (note == 1) { whiteKeyIdx = 0; blackOffset = 14; }
            else if (note == 3) { whiteKeyIdx = 1; blackOffset = 14; }
            else if (note == 6) { whiteKeyIdx = 3; blackOffset = 14; }
            else if (note == 8) { whiteKeyIdx = 4; blackOffset = 14; }
            else if (note == 10) { whiteKeyIdx = 5; blackOffset = 14; }

            float baseX = p.x + centerOffset + (whiteKeyCount + whiteKeyIdx) * whiteKeyWidth;
            float x = baseX + blackOffset;
            float y = p.y;

            ImU32 color;
            if (g_pianoKeyPressed[keyIdx]) {
                if (g_pianoKeyFromKeyboard[keyIdx] || g_pianoKeyOnSlot3_2MHz[keyIdx]) {
                    // Green for keyboard input or Slot3 2MHz
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;  // 0.0 to 1.0
                    int r = (int)(40 + 140 * intensity);   // 40 to 180
                    int g = 255;  // Full green
                    int b = (int)(80 + 120 * intensity);   // 80 to 200
                    color = IM_COL32(r, g, b, 255);
                } else {
                    // Blue for MIDI playback
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;  // 0.0 to 1.0
                    int r = (int)(40 + 140 * intensity);  // 40 to 180
                    int g = (int)(80 + 175 * intensity);  // 80 to 255
                    int b = 255;
                    color = IM_COL32(r, g, b, 255);
                }
            } else {
                color = IM_COL32(0, 0, 0, 255);
            }

            draw_list->AddRectFilled(ImVec2(x, y),
                ImVec2(x + blackKeyWidth, y + blackKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y),
                ImVec2(x + blackKeyWidth, y + blackKeyHeight), IM_COL32(128, 128, 128, 255));
        }
        whiteKeyCount += 7;
    }

    // v11: Draw C8-B8 black keys (when Slot3 2MHz enabled)
    if (g_enableSlot3_2MHz) {
        for (int note = 0; note <= 11; note++) {
            if (!g_isBlackNote[note]) continue;

            int keyIdx = get_key_index(6, note);  // octave 6 = C8-B8
            if (keyIdx < 0) continue;

            int whiteKeyIdx = 0;
            int blackOffset = 14;

            if (note == 1) { whiteKeyIdx = 0; blackOffset = 14; }
            else if (note == 3) { whiteKeyIdx = 1; blackOffset = 14; }
            else if (note == 6) { whiteKeyIdx = 3; blackOffset = 14; }
            else if (note == 8) { whiteKeyIdx = 4; blackOffset = 14; }
            else if (note == 10) { whiteKeyIdx = 5; blackOffset = 14; }

            // whiteKeyCount at this point = 36 (B2 + C3-B7) + C8 octave white keys already drawn
            // The C8 octave starts at white key index 36
            float baseX = p.x + centerOffset + (36 + whiteKeyIdx) * whiteKeyWidth;
            float x = baseX + blackOffset;
            float y = p.y;

            ImU32 color;
            if (g_pianoKeyPressed[keyIdx]) {
                if (g_pianoKeyFromKeyboard[keyIdx] || g_pianoKeyOnSlot3_2MHz[keyIdx]) {
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;
                    int r = (int)(40 + 140 * intensity);
                    int g = 255;
                    int b = (int)(80 + 120 * intensity);
                    color = IM_COL32(r, g, b, 255);
                } else {
                    int velocity = g_pianoKeyVelocity[keyIdx];
                    float intensity = velocity / 127.0f;
                    int r = (int)(40 + 140 * intensity);
                    int g = (int)(80 + 175 * intensity);
                    int b = 255;
                    color = IM_COL32(r, g, b, 255);
                }
            } else {
                color = IM_COL32(30, 30, 20, 255);  // Slightly warm tint for 2MHz octave
            }

            draw_list->AddRectFilled(ImVec2(x, y),
                ImVec2(x + blackKeyWidth, y + blackKeyHeight), color);
            draw_list->AddRect(ImVec2(x, y),
                ImVec2(x + blackKeyWidth, y + blackKeyHeight), IM_COL32(128, 128, 128, 255));
        }
    }

    // Display SUS indicator when sustain pedal is active
    if (g_sustainPedalActive && g_enableSustainPedal) {
        ImVec2 susPos = ImVec2(p.x + centerOffset + 10, p.y + whiteKeyHeight + 10);
        draw_list->AddText(susPos, IM_COL32(255, 200, 0, 255), "SUS");
    }

    ImGui::EndChild();
}

// v10: Render level meters below piano keyboard
void RenderLevelMeters() {
    ImGui::BeginChild("LevelMeters", ImVec2(0, 0), true);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    float availWidth = ImGui::GetContentRegionAvail().x;
    float availHeight = ImGui::GetContentRegionAvail().y;

    // Layout: 4 vertical boxes (one for each Slot), each containing melody and rhythm sections
    float chipGroupWidth = availWidth / 4.0f;  // 4 chip groups
    float boxPadding = 8.0f;  // Padding inside each box
    float meterWidth = 18.0f;  // Width for melody meters (thinner)
    float rhythmMeterWidth = 25.0f;  // Width for rhythm meters (wider to fit drum labels)
    float spacing = 10.0f;  // Spacing between meters
    float verticalSpacing = 15.0f;  // Vertical spacing between melody and rhythm sections

    // Section heights - increased for better visibility
    float slotLabelHeight = 25.0f;
    float melodyMeterHeight = (availHeight - slotLabelHeight - verticalSpacing - boxPadding * 3) * 0.5f;
    float rhythmMeterHeight = (availHeight - slotLabelHeight - verticalSpacing - boxPadding * 3) * 0.5f;
    float channelLabelHeight = 20.0f;  // Space for channel/drum labels at bottom

    // Helper function to convert linear level (0.0-1.0) to dB scale display position
    // 0dB=100%, -6dB=75%, -12dB=50%, -24dB=25%
    auto levelToDBScale = [](float level) -> float {
        if (level <= 0.0f) return 0.0f;
        // Convert to dB: dB = 20 * log10(level)
        // Then map to display position: 0dB->1.0, -24dB->0.0
        float db = 20.0f * log10f(level);
        if (db < -24.0f) db = -24.0f;
        return (db + 24.0f) / 24.0f;  // Map -24dB to 0.0, 0dB to 1.0
    };

    // Helper function to get gradient color based on level (0.0 to 1.0)
    auto getLevelColor = [](float level) -> ImU32 {
        if (level <= 0.0f) return IM_COL32(40, 40, 40, 255);  // Dark gray for empty

        // Blue (0.0) -> Green (0.33) -> Yellow (0.66) -> Red (1.0)
        if (level < 0.33f) {
            // Blue to Green
            float t = level / 0.33f;
            int r = (int)(0 + 0 * t);
            int g = (int)(100 + 155 * t);  // 100 to 255
            int b = (int)(255 - 155 * t);  // 255 to 100
            return IM_COL32(r, g, b, 255);
        } else if (level < 0.66f) {
            // Green to Yellow
            float t = (level - 0.33f) / 0.33f;
            int r = (int)(0 + 255 * t);    // 0 to 255
            int g = 255;
            int b = (int)(100 - 100 * t);  // 100 to 0
            return IM_COL32(r, g, b, 255);
        } else {
            // Yellow to Red
            float t = (level - 0.66f) / 0.34f;
            int r = 255;
            int g = (int)(255 - 155 * t);  // 255 to 100
            int b = 0;
            return IM_COL32(r, g, b, 255);
        }
    };

    // Removed Melody and Rhythm labels - using only Slot labels at top-left of each box

    // ===== Draw 4 Slot boxes (each containing melody and rhythm sections) =====
    const char* chipLabels[] = {"Slot0", "Slot1", "Slot2", "Slot3"};

    for (int chip = 0; chip < 4; chip++) {
        float chipX = p.x + chip * chipGroupWidth;

        // Draw box border around this Slot's meters
        draw_list->AddRect(
            ImVec2(chipX + 2, p.y + 2),
            ImVec2(chipX + chipGroupWidth - 2, p.y + availHeight - 2),
            IM_COL32(120, 120, 120, 255),
            4.0f,  // Rounded corners
            0,
            2.0f   // Border thickness
        );

        // Slot label at top-left corner of box
        ImVec2 slotLabelPos = ImVec2(chipX + 8, p.y + 8);
        draw_list->AddText(slotLabelPos, IM_COL32(200, 200, 200, 255), chipLabels[chip]);

        float currentY = p.y + slotLabelHeight + boxPadding;

        // ===== Melody Section =====
        // Calculate starting position to center the 4 melody meters
        float melodyTotalWidth = 4 * meterWidth + 3 * spacing;
        float melodyStartX = chipX + (chipGroupWidth - melodyTotalWidth) * 0.5f;

        for (int ch = 0; ch < 4; ch++) {
            int channelIndex = chip * 4 + ch;
            float meterX = melodyStartX + ch * (meterWidth + spacing);
            float meterY = currentY;

            // Background (dark gray)
            draw_list->AddRectFilled(
                ImVec2(meterX, meterY),
                ImVec2(meterX + meterWidth, meterY + melodyMeterHeight),
                IM_COL32(30, 30, 30, 255)
            );

            // Border
            draw_list->AddRect(
                ImVec2(meterX, meterY),
                ImVec2(meterX + meterWidth, meterY + melodyMeterHeight),
                IM_COL32(100, 100, 100, 255)
            );

            // Get level value and convert to dB scale
            float level = g_channels[channelIndex].currentLevel;
            float displayLevel = levelToDBScale(level);

            // Draw level bar with gradient
            if (displayLevel > 0.01f) {
                float barHeight = melodyMeterHeight * displayLevel;
                float barY = meterY + melodyMeterHeight - barHeight;

                // Draw gradient from bottom to top
                int segments = 20;
                for (int i = 0; i < segments; i++) {
                    float segmentHeight = barHeight / segments;
                    float segmentY = barY + i * segmentHeight;
                    float segmentLevel = (float)(segments - i) / segments * displayLevel;

                    ImU32 color = getLevelColor(segmentLevel);
                    draw_list->AddRectFilled(
                        ImVec2(meterX + 1, segmentY),
                        ImVec2(meterX + meterWidth - 1, segmentY + segmentHeight),
                        color
                    );
                }
            }

            // Channel label at top-left of meter
            char label[8];
            sprintf(label, "%d", ch);
            ImVec2 labelPos = ImVec2(meterX + 2, meterY + 2);
            draw_list->AddText(labelPos, IM_COL32(180, 180, 180, 255), label);
        }

        currentY += melodyMeterHeight + verticalSpacing;

        // ===== Rhythm Section =====
        // Calculate starting position to center the 5 rhythm meters
        float rhythmTotalWidth = 5 * rhythmMeterWidth + 4 * spacing;
        float rhythmStartX = chipX + (chipGroupWidth - rhythmTotalWidth) * 0.5f;

        for (int drum = 0; drum < 5; drum++) {
            float meterX = rhythmStartX + drum * (rhythmMeterWidth + spacing);
            float meterY = currentY;

            // Background (dark gray)
            draw_list->AddRectFilled(
                ImVec2(meterX, meterY),
                ImVec2(meterX + rhythmMeterWidth, meterY + rhythmMeterHeight),
                IM_COL32(30, 30, 30, 255)
            );

            // Border
            draw_list->AddRect(
                ImVec2(meterX, meterY),
                ImVec2(meterX + rhythmMeterWidth, meterY + rhythmMeterHeight),
                IM_COL32(100, 100, 100, 255)
            );

            // Get level value - check if this specific drum is active
            float level = 0.0f;
            if (g_drumActive[chip][drum]) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_drumTriggerTime[chip][drum]);
                float t = elapsed.count() / 1000.0f;
                level = expf(-t * 20.0f);  // Fast decay
            }
            float displayLevel = levelToDBScale(level);

            // Draw level bar with gradient
            if (displayLevel > 0.01f) {
                float barHeight = rhythmMeterHeight * displayLevel;
                float barY = meterY + rhythmMeterHeight - barHeight;

                // Draw gradient from bottom to top
                int segments = 20;
                for (int i = 0; i < segments; i++) {
                    float segmentHeight = barHeight / segments;
                    float segmentY = barY + i * segmentHeight;
                    float segmentLevel = (float)(segments - i) / segments * displayLevel;

                    ImU32 color = getLevelColor(segmentLevel);
                    draw_list->AddRectFilled(
                        ImVec2(meterX + 1, segmentY),
                        ImVec2(meterX + rhythmMeterWidth - 1, segmentY + segmentHeight),
                        color
                    );
                }
            }

            // Drum label at top-left of meter
            const char* drumLabels[] = {"BD", "HC", "SD", "HO", "HD"};
            ImVec2 labelPos = ImVec2(meterX + 1, meterY + 2);
            draw_list->AddText(labelPos, IM_COL32(180, 180, 180, 255), drumLabels[drum]);
        }
    }

    ImGui::EndChild();
}


void RenderChannelStatus() {
    ImVec4 slot0Color = ImVec4(0.0f, 1.0f, 0.5f, 1.0f);  // Cyan-green for Slot0
    ImVec4 slot1Color = ImVec4(0.5f, 0.5f, 1.0f, 1.0f);  // Light blue for Slot1
    ImVec4 slot2Color = ImVec4(1.0f, 0.5f, 0.5f, 1.0f);  // Light red for Slot2
    ImVec4 slot3Color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);  // Orange for Slot3
    ImVec4 slotDisabledColor = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);  // Gray for disabled slots
    ImVec4 drumActiveColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // Green for active drums
    ImVec4 releaseColor = ImVec4(1.0f, 1.0f, 0.0f, 0.8f);  // Yellow for Release phase

    const int RELEASE_DISPLAY_TIME_MS = 1000;  // Show Release for 1 second after key release

    // 2x2 layout for 4 chip status boxes
    float availWidth = ImGui::GetContentRegionAvail().x;
    float availHeight = ImGui::GetContentRegionAvail().y;
    float boxWidth = (availWidth / 2.0f - 5);
    float boxHeight = (availHeight / 2.0f - 5);

    auto now = std::chrono::steady_clock::now();

    // Helper lambda to render a single chip status box
    auto renderChipBox = [&](int chipIndex, const char* childName, ImVec4 activeColor, bool isEnabled) {
        ImGui::BeginChild(childName, ImVec2(boxWidth, boxHeight), true);

        // Title
        if (isEnabled) {
            ImGui::TextColored(activeColor, "YM2163 Slot%d (used)", chipIndex);
        } else {
            ImGui::TextColored(slotDisabledColor, "YM2163 Slot%d (unused)", chipIndex);
        }
        ImGui::Separator();

        // Display 4 channels
        int baseChannel = chipIndex * 4;
        for (int i = 0; i < 4; i++) {
            int ch = baseChannel + i;
            if (g_channels[ch].active) {
                ImGui::TextColored(activeColor, "CH%d: %s%d", i,
                    g_noteNames[g_channels[ch].note],
                    g_channels[ch].octave + 2);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    "[%s/%s/%s]",
                    g_timbreNames[g_channels[ch].timbre],
                    g_envelopeNames[g_channels[ch].envelope],
                    g_volumeNames[g_channels[ch].volume]);
            } else {
                auto timeSinceRelease = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_channels[ch].releaseTime).count();

                if (g_channels[ch].hasBeenUsed && timeSinceRelease < RELEASE_DISPLAY_TIME_MS) {
                    ImGui::TextColored(releaseColor, "CH%d: Release", i);
                } else {
                    ImGui::TextDisabled("CH%d: ---", i);
                }
            }
        }

        // Drum status
        ImGui::Separator();
        ImGui::Text("Drums:");
        ImGui::SameLine();
        for (int i = 0; i < 5; i++) {
            if (g_drumActive[chipIndex][i]) {
                ImGui::TextColored(drumActiveColor, "%s", g_drumNames[i]);
            } else {
                ImGui::TextDisabled("%s", g_drumNames[i]);
            }
            if (i < 4) ImGui::SameLine();
        }
        ImGui::EndChild();
    };

    // Row 1: Slot0 and Slot1
    renderChipBox(0, "Slot0Channels", slot0Color, true);  // Slot0 always enabled
    ImGui::SameLine();
    renderChipBox(1, "Slot1Channels", slot1Color, g_enableSecondYM2163);

    // Row 2: Slot2 and Slot3
    renderChipBox(2, "Slot2Channels", slot2Color, g_enableThirdYM2163);
    ImGui::SameLine();
    renderChipBox(3, "Slot3Channels", slot3Color, g_enableFourthYM2163);
}

void RenderControls() {
    ImGui::BeginChild("Controls", ImVec2(280, 0), true);

    ImGui::Text("Controls");
    ImGui::Separator();

    ImGui::Text("Octave: B=%d", g_currentOctave);
    if (g_currentOctave == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(B2 only)");
    } else {
        ImGui::SameLine();
        ImGui::Text("(C%d-B%d)", g_currentOctave + 2, g_currentOctave + 2);
    }
    if (ImGui::Button("Oct +") && g_currentOctave < 5) {
        stop_all_notes();
        g_currentOctave++;
    }
    ImGui::SameLine();
    if (ImGui::Button("Oct -") && g_currentOctave > 0) {
        stop_all_notes();
        g_currentOctave--;
    }

    ImGui::Spacing();

    ImGui::Text("Volume: %-15s", g_volumeNames[g_currentVolume]);
    if (ImGui::Button("Vol +") && g_currentVolume > 0) g_currentVolume--;
    ImGui::SameLine();
    if (ImGui::Button("Vol -") && g_currentVolume < 3) g_currentVolume++;

    ImGui::Spacing();
    ImGui::Separator();

    // MIDI Control Mode
    ImGui::Text("MIDI Control Mode");
    if (ImGui::RadioButton("Live Control", g_useLiveControl)) {
        g_useLiveControl = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("MIDI playback uses UI Wave/Envelope settings\n(Ignores config file)");
    }

    if (ImGui::RadioButton("Config Mode", !g_useLiveControl)) {
        g_useLiveControl = false;
        // When switching to Config Mode, load current instrument config to UI
        LoadInstrumentConfigToUI(g_selectedInstrument);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("MIDI playback uses config file settings\n(Wave/Envelope only affect keyboard play)");
    }

    ImGui::Spacing();

    // Velocity mapping option
    ImGui::Checkbox("Velocity Mapping", &g_enableVelocityMapping);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Map MIDI velocity to 4-level volume\n"
                         "(Enable for dynamic volume control)");
    }

    // Dynamic velocity mapping option (sub-option, indented)
    if (g_enableVelocityMapping) {
        ImGui::Indent(20.0f);
        if (ImGui::Checkbox("Dynamic Mapping", &g_enableDynamicVelocityMapping)) {
            // Re-analyze current MIDI file if loaded
            if (g_enableDynamicVelocityMapping && g_midiPlayer.midiFile.status()) {
                AnalyzeVelocityDistribution();
            }
        }
        if (ImGui::IsItemHovered()) {
            if (g_enableDynamicVelocityMapping) {
                ImGui::SetTooltip("Dynamic velocity mapping (ENABLED):\n"
                                 "Analyzes MIDI file velocity distribution\n"
                                 "Maps most common velocities to -6dB and -12dB\n"
                                 "Maps peak velocities to 0dB\n"
                                 "Maps very low velocities to Mute\n\n"
                                 "Current thresholds:\n"
                                 "  0dB: >= %d\n"
                                 "  -6dB: %d-%d\n"
                                 "  -12dB: %d-%d\n"
                                 "  Mute: < %d",
                                 g_velocityAnalysis.threshold_0dB,
                                 g_velocityAnalysis.threshold_6dB, g_velocityAnalysis.threshold_0dB - 1,
                                 g_velocityAnalysis.threshold_12dB, g_velocityAnalysis.threshold_6dB - 1,
                                 g_velocityAnalysis.threshold_mute);
            } else {
                ImGui::SetTooltip("Fixed velocity mapping:\n"
                                 "  0dB: 113-127\n"
                                 "  -6dB: 64-112\n"
                                 "  -12dB: 1-63\n"
                                 "  Mute: 0");
            }
        }
        ImGui::Unindent(20.0f);
    }

    // Sustain pedal mapping option
    ImGui::Checkbox("Sustain Pedal", &g_enableSustainPedal);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Map sustain pedal (CC64) to envelope:\n"
                         "Pedal Down: Fast, Pedal Up: Decay");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Checkbox("Global Media Keys", &g_enableGlobalMediaKeys)) {
        if (g_enableGlobalMediaKeys) {
            RegisterGlobalMediaKeys();
        } else {
            UnregisterGlobalMediaKeys();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Capture global media keys:\n"
                         "Play/Pause, Next Track, Previous Track\n"
                         "Works even when window is not focused");
    }

    // Auto-skip silence option
    ImGui::Checkbox("Auto-Skip Silence", &g_enableAutoSkipSilence);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically skip silence at the start of MIDI files\n"
                         "Jumps to the first note to avoid waiting");
    }

    // v11: YM2163 chip controls (redesigned)
    ImGui::Separator();
    ImGui::Text("YM2163 Chips");

    // v11: Connect/Disconnect hardware button
    if (g_hardwareConnected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Disconnect Hardware", ImVec2(-1, 0))) {
            g_manualDisconnect = true;
            DisconnectHardware();
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
        if (ImGui::Button("Connect Hardware", ImVec2(-1, 0))) {
            g_manualDisconnect = false;
            ConnectHardware();
        }
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(g_hardwareConnected ?
            "Disconnect from FTDI hardware\nHardware will auto-disconnect if removed" :
            "Connect to FTDI hardware\nHardware will auto-connect when plugged in");
    }

    // v11: Slot0 (1st @1MHz) - always enabled, cannot be unchecked
    {
        bool alwaysOn = true;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Slot0 (1st @1MHz)", &alwaysOn);
        ImGui::EndDisabled();
    }

    // v11: Slot1 (2nd @1MHz)
    if (ImGui::Checkbox("Slot1 (2nd @1MHz)", &g_enableSecondYM2163)) {
        if (g_ftHandle) {
            if (g_enableSecondYM2163) {
                init_single_ym2163(1);
            } else {
                for (int i = 4; i < 8; i++) {
                    if (g_channels[i].active) stop_note(i);
                }
            }
        }
        SaveSlotConfigToINI();
    }

    // v11: Slot2 (3rd @1MHz)
    if (ImGui::Checkbox("Slot2 (3rd @1MHz)", &g_enableThirdYM2163)) {
        if (g_ftHandle) {
            if (g_enableThirdYM2163) {
                init_single_ym2163(2);
            } else {
                for (int i = 8; i < 12; i++) {
                    if (g_channels[i].active) stop_note(i);
                }
            }
        }
        SaveSlotConfigToINI();
    }

    // v11: Slot3 (4th @1MHz) - normal polyphony mode, mutually exclusive with 2MHz
    {
        bool slot3_1MHz = g_enableFourthYM2163 && !g_enableSlot3_2MHz;
        if (ImGui::Checkbox("Slot3 (4th @1MHz)", &slot3_1MHz)) {
            if (slot3_1MHz) {
                g_enableFourthYM2163 = true;
                g_enableSlot3_2MHz = false;
                if (g_ftHandle) {
                    init_single_ym2163(3);
                }
            } else {
                g_enableFourthYM2163 = false;
                for (int i = 12; i < 16; i++) {
                    if (g_channels[i].active) stop_note(i);
                }
            }
            SaveSlotConfigToINI();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enable Slot3 as normal 1MHz chip\nPolyphony: +4 channels");
        }
    }

    // v11: Slot3 (4th @2MHz) - high octave mode, mutually exclusive with 1MHz
    {
        if (ImGui::Checkbox("Slot3 (4th @2MHz)", &g_enableSlot3_2MHz)) {
            if (g_enableSlot3_2MHz) {
                g_enableFourthYM2163 = true;
                if (g_ftHandle) {
                    init_single_ym2163(3);
                }
            } else {
                g_enableFourthYM2163 = false;
                for (int i = 12; i < 16; i++) {
                    if (g_channels[i].active) stop_note(i);
                }
            }
            SaveSlotConfigToINI();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enable Slot3 as 2MHz chip for C8-B8 octave\n"
                             "Dedicated to high-octave notes only");
        }
    }

    // v11: Slot3 Overflow - enable C4-B7 overflow to 2MHz when normal channels full
    {
        bool overflow_enabled = g_enableSlot3Overflow;
        if (ImGui::Checkbox("Slot3 Overflow (C4-B7)", &overflow_enabled)) {
            g_enableSlot3Overflow = overflow_enabled;
            SaveSlotConfigToINI();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled: overflow C4-B7 to Slot3 2MHz when normal channels >=80% full\n"
                             "When disabled: Slot3 2MHz only plays C8-B8 high octave");
        }
    }

    // v11: Slot3 2MHz Range - select which octaves Slot3 2MHz handles
    ImGui::Text("Slot3 2MHz Range");
    {
        const char* rangeOptions[] = { "C8-B8 only", "C7-B8" };
        if (ImGui::Combo("##Slot3_2MHz_Range", &g_slot3_2MHz_Range, rangeOptions, IM_ARRAYSIZE(rangeOptions))) {
            SaveSlotConfigToINI();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("C8-B8 only: Slot3 2MHz plays only C8-B8 high octave\n"
                             "C7-B8: Slot3 2MHz plays both C7-B7 and C8-B8 (default)");
        }
    }

    // v11: All Slots button - enable all slots with Slot3 @2MHz
    if (ImGui::Button("All Slots", ImVec2(-1, 0))) {
        g_enableSecondYM2163 = true;
        g_enableThirdYM2163 = true;
        g_enableFourthYM2163 = true;
        g_enableSlot3_2MHz = true;
        if (g_ftHandle) {
            init_single_ym2163(1);
            init_single_ym2163(2);
            init_single_ym2163(3);
        }
        SaveSlotConfigToINI();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable all 4 slots\nSlot3 uses 2MHz mode for C8-B8");
    }

    ImGui::Separator();

    // Instrument Selector and Config Save
    ImGui::Text("Instrument Editor");

    // Build instrument combo list
    static char instrumentPreview[128];
    if (g_instrumentConfigs.count(g_selectedInstrument) > 0) {
        snprintf(instrumentPreview, sizeof(instrumentPreview), "%d: %s",
                 g_selectedInstrument, g_instrumentConfigs[g_selectedInstrument].name.c_str());
    } else {
        snprintf(instrumentPreview, sizeof(instrumentPreview), "%d: (undefined)", g_selectedInstrument);
    }

    // Set combo box width to full available width
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##InstrumentSelect", instrumentPreview, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < 128; i++) {
            char label[128];
            if (g_instrumentConfigs.count(i) > 0) {
                snprintf(label, sizeof(label), "%d: %s", i, g_instrumentConfigs[i].name.c_str());
            } else {
                snprintf(label, sizeof(label), "%d: (undefined)", i);
            }

            bool isSelected = (g_selectedInstrument == i);
            if (ImGui::Selectable(label, isSelected)) {
                g_selectedInstrument = i;
                LoadInstrumentConfigToUI(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Handle mouse wheel for instrument selection
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f && g_selectedInstrument > 0) {
            g_selectedInstrument--;
            LoadInstrumentConfigToUI(g_selectedInstrument);
        } else if (wheel < 0.0f && g_selectedInstrument < 127) {
            g_selectedInstrument++;
            LoadInstrumentConfigToUI(g_selectedInstrument);
        }
    }

    // Load and Save buttons
    float btnWidth = (ImGui::GetContentRegionAvail().x - 5.0f) / 2.0f;
    if (ImGui::Button("Load Config", ImVec2(btnWidth, 0))) {
        LoadInstrumentConfigToUI(g_selectedInstrument);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Load selected instrument config to UI");
    }

    ImGui::SameLine();

    if (ImGui::Button("Save Config", ImVec2(btnWidth, 0))) {
        SaveInstrumentConfig(g_selectedInstrument);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save current Wave/Envelope to selected instrument");
    }

    ImGui::Spacing();

    // Tuning button (full width)
    if (ImGui::Button("Tuning", ImVec2(-1, 0))) {
        g_showTuningWindow = !g_showTuningWindow;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open frequency tuning window");
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Envelope");
    for (int i = 0; i < 4; i++) {
        if (ImGui::RadioButton(g_envelopeNames[i], g_currentEnvelope == i)) {
            g_currentEnvelope = i;
        }
        if (i % 2 == 0 && i < 3) ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Pedal Mode");

    if (ImGui::RadioButton("Disabled##PedalMode", g_pedalMode == 0)) {
        g_pedalMode = 0;
    }
    if (ImGui::RadioButton("Piano Pedal##PedalMode", g_pedalMode == 1)) {
        g_pedalMode = 1;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pedal Down: Fast envelope\n"
                         "Pedal Up: Decay envelope");
    }
    if (ImGui::RadioButton("Organ Pedal##PedalMode", g_pedalMode == 2)) {
        g_pedalMode = 2;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pedal Down: Slow envelope\n"
                         "Pedal Up: Medium envelope");
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Timbre");
    for (int i = 1; i <= 5; i++) {
        if (ImGui::RadioButton(g_timbreNames[i], g_currentTimbre == i)) {
            g_currentTimbre = i;
        }
        if (i % 2 == 1 && i < 5) ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Drums (Numpad 1-5)");
    for (int i = 0; i < 5; i++) {
        ImGui::PushID(i);
        if (ImGui::Button(g_drumNames[i], ImVec2(45, 40))) {
            play_drum(g_drumBits[i]);
        }
        if (i < 4) ImGui::SameLine();
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::EndChild();
}

void RenderLog() {
    // v10: Log area is collapsible, default collapsed
    static bool g_logExpanded = false;  // Default collapsed

    // ===== Log Section (Collapsible) =====
    if (ImGui::CollapsingHeader("Log", g_logExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        g_logExpanded = true;  // Remember expanded state

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &g_autoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##Log")) {
            g_logBuffer.clear();
            g_logDisplayBuffer[0] = '\0';
            g_lastLogSize = 0;
        }

        size_t copyLen = (g_logBuffer.length() < sizeof(g_logDisplayBuffer) - 1) ?
                         g_logBuffer.length() : sizeof(g_logDisplayBuffer) - 1;
        memcpy(g_logDisplayBuffer, g_logBuffer.c_str(), copyLen);
        g_logDisplayBuffer[copyLen] = '\0';

        bool log_changed = (g_logBuffer.length() != g_lastLogSize);
        g_lastLogSize = g_logBuffer.length();

        if (g_autoScroll && log_changed) {
            g_logScrollToBottom = true;
        }

        float logHeight = 150;  // Fixed height when expanded
        ImGui::BeginChild("LogScrollRegion", ImVec2(0, logHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

        ImVec2 text_size = ImGui::CalcTextSize(g_logDisplayBuffer, NULL, false, -1.0f);
        float line_height = ImGui::GetTextLineHeightWithSpacing();
        float min_visible_height = ImGui::GetContentRegionAvail().y;

        float input_height = (text_size.y > min_visible_height) ? text_size.y + line_height * 2 : min_visible_height;

        ImGui::InputTextMultiline("##LogText",
            g_logDisplayBuffer,
            sizeof(g_logDisplayBuffer),
            ImVec2(-1, input_height),
            ImGuiInputTextFlags_ReadOnly);

        if (g_logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            g_logScrollToBottom = false;
        }

        ImGui::EndChild();
    } else {
        g_logExpanded = false;
    }

    ImGui::Spacing();
}

void RenderMIDIFolderHistory() {
    // ===== MIDI Folder History Section =====
    ImGui::Text("MIDI Folder History");
    ImGui::SameLine();
    if (ImGui::Button("Clear All##History")) {
        ClearMIDIFolderHistory();
    }
    ImGui::Separator();

    // Use remaining height for history
    float historyHeight = ImGui::GetContentRegionAvail().y - 5;
    ImGui::BeginChild("HistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

    if (g_midiFolderHistory.empty()) {
        ImGui::TextDisabled("No MIDI folder history yet...");
        ImGui::TextDisabled("Navigate to folders containing MIDI files to build history.");
    } else {
        for (int i = 0; i < (int)g_midiFolderHistory.size(); i++) {
            const std::string& path = g_midiFolderHistory[i];

            ImGui::PushID(i);

            size_t lastSlash = path.find_last_of("\\/");
            std::string folderName = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

            if (ImGui::Selectable(folderName.c_str(), false)) {
                NavigateToPath(path.c_str());
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", path.c_str());
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove from history")) {
                    RemoveMIDIFolderHistoryEntry(i);
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
    }

    ImGui::EndChild();
}

// ===== Tuning Window =====

void RenderTuningWindow() {
    if (!g_showTuningWindow) return;

    ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Frequency Tuning", &g_showTuningWindow)) {
        ImGui::Text("Adjust YM2163 frequency values (FNUM) for each note");
        ImGui::Text("Range: 0-2047 | Mouse wheel: +/-10 per step");
        ImGui::Separator();
        ImGui::Spacing();

        // Load and Save buttons
        float btnWidth = (ImGui::GetContentRegionAvail().x - 5.0f) / 2.0f;
        if (ImGui::Button("Load All Frequencies", ImVec2(btnWidth, 0))) {
            LoadFrequenciesFromINI();
            log_command("All frequencies loaded from INI");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Load all frequency values from ym2163_midi_config.ini");
        }

        ImGui::SameLine();

        if (ImGui::Button("Save All Frequencies", ImVec2(btnWidth, 0))) {
            SaveFrequenciesToINI();
            log_command("All frequencies saved to INI");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Save all frequency values to ym2163_midi_config.ini");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Base frequencies (C3-C6 octaves, 12 notes)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Base Frequencies (C3-C6 octaves)");
        ImGui::Separator();

        for (int i = 0; i < 12; i++) {
            ImGui::PushID(i);
            ImGui::Text("%s:", g_noteNames[i]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);

            // InputInt with mouse wheel support
            if (ImGui::InputInt("", &g_fnums[i], 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
                if (g_fnums[i] < 0) g_fnums[i] = 0;
                if (g_fnums[i] > 2047) g_fnums[i] = 2047;
                log_command("Base Freq updated: %s = %d", g_noteNames[i], g_fnums[i]);
            }

            // Mouse wheel adjustment
            if (ImGui::IsItemHovered()) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    g_fnums[i] += (int)(wheel * 10);
                    if (g_fnums[i] < 0) g_fnums[i] = 0;
                    if (g_fnums[i] > 2047) g_fnums[i] = 2047;
                    log_command("Base Freq updated: %s = %d", g_noteNames[i], g_fnums[i]);
                }
            }

            if ((i + 1) % 6 != 0) ImGui::SameLine();
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // B2 special frequency (lowest note)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "B2 Frequency (Lowest Note)");
        ImGui::Separator();

        ImGui::PushID(100);
        ImGui::Text("B2:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);

        if (ImGui::InputInt("", &g_fnum_b2, 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
            if (g_fnum_b2 < 0) g_fnum_b2 = 0;
            if (g_fnum_b2 > 2047) g_fnum_b2 = 2047;
            log_command("B2 Freq updated: B2 = %d", g_fnum_b2);
        }

        if (ImGui::IsItemHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                g_fnum_b2 += (int)(wheel * 10);
                if (g_fnum_b2 < 0) g_fnum_b2 = 0;
                if (g_fnum_b2 > 2047) g_fnum_b2 = 2047;
                log_command("B2 Freq updated: B2 = %d", g_fnum_b2);
            }
        }
        ImGui::PopID();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // C7 octave frequencies (C7-B7, 12 notes)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "C7 Octave Frequencies (C7-B7)");
        ImGui::Separator();

        for (int i = 0; i < 12; i++) {
            ImGui::PushID(200 + i);
            ImGui::Text("%s7:", g_noteNames[i]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);

            if (ImGui::InputInt("", &g_fnums_c7[i], 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
                if (g_fnums_c7[i] < 0) g_fnums_c7[i] = 0;
                if (g_fnums_c7[i] > 2047) g_fnums_c7[i] = 2047;
                log_command("C7 Freq updated: %s7 = %d", g_noteNames[i], g_fnums_c7[i]);
            }

            if (ImGui::IsItemHovered()) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    g_fnums_c7[i] += (int)(wheel * 10);
                    if (g_fnums_c7[i] < 0) g_fnums_c7[i] = 0;
                    if (g_fnums_c7[i] > 2047) g_fnums_c7[i] = 2047;
                    log_command("C7 Freq updated: %s7 = %d", g_noteNames[i], g_fnums_c7[i]);
                }
            }

            if ((i + 1) % 6 != 0) ImGui::SameLine();
            ImGui::PopID();
        }

        ImGui::Spacing();
    }
    ImGui::End();
}

// ===== DirectX 11 Setup =====

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (res != S_OK)
        return false;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();

    return true;
}

void CleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void HandleKeyPress(int vk) {
    if (g_keyStates[vk]) return;
    g_keyStates[vk] = true;

    // Don't process keyboard piano input when any input field is active
    if (g_isInputActive) {
        return;
    }

    if (vk == VK_PRIOR && g_currentOctave < 5) {
        stop_all_notes();
        g_currentOctave++;
        return;
    } else if (vk == VK_NEXT && g_currentOctave > 0) {
        stop_all_notes();
        g_currentOctave--;
        return;
    }

    if (vk == VK_UP && g_currentVolume > 0) {
        g_currentVolume--;
        return;
    } else if (vk == VK_DOWN && g_currentVolume < 3) {
        g_currentVolume++;
        return;
    }

    if (vk >= VK_F1 && vk <= VK_F4) {
        g_currentEnvelope = vk - VK_F1;
        return;
    }

    if (vk >= VK_F5 && vk <= VK_F9) {
        g_currentTimbre = vk - VK_F5 + 1;
        return;
    }

    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD5) {
        int drumIdx = vk - VK_NUMPAD1;
        play_drum(g_drumBits[drumIdx]);
        return;
    }

    for (int i = 0; i < g_numKeyMappings; i++) {
        if (g_keyMappings[i].vk == vk) {
            int note = g_keyMappings[i].note;
            int octave = g_currentOctave + g_keyMappings[i].octave_offset;

            bool valid = false;
            if (octave == 0 && note == 11) {
                valid = true;
            } else if (octave >= 1 && octave <= 5) {
                valid = true;
            }

            if (valid) {
                int channel = find_free_channel();
                if (channel >= 0) {
                    play_note(channel, note, octave);

                    int keyIdx = get_key_index(octave, note);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyPressed[keyIdx] = true;
                        g_pianoKeyFromKeyboard[keyIdx] = true;  // Mark as keyboard input
                        g_pianoKeyVelocity[keyIdx] = 96;  // Default keyboard velocity
                    }
                }
            }
            break;
        }
    }
}

void HandleKeyRelease(int vk) {
    g_keyStates[vk] = false;

    for (int i = 0; i < g_numKeyMappings; i++) {
        if (g_keyMappings[i].vk == vk) {
            int note = g_keyMappings[i].note;
            int octave = g_currentOctave + g_keyMappings[i].octave_offset;

            bool valid = false;
            if (octave == 0 && note == 11) {
                valid = true;
            } else if (octave >= 1 && octave <= 5) {
                valid = true;
            }

            if (valid) {
                int channel = find_channel_playing(note, octave);

                if (channel >= 0) {
                    stop_note(channel);

                    int keyIdx = get_key_index(octave, note);
                    if (keyIdx >= 0 && keyIdx < 73) {
                        g_pianoKeyPressed[keyIdx] = false;
                        g_pianoKeyFromKeyboard[keyIdx] = false;  // Clear keyboard flag
                        g_pianoKeyVelocity[keyIdx] = 0;  // Clear velocity
                    }
                }
            }
            break;
        }
    }
}

// ===== Window Procedure =====

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
                return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;

        case WM_ENTERSIZEMOVE:
            // Window is being dragged or resized - start timer to keep MIDI playing
            g_isWindowDragging = true;
            SetTimer(hWnd, TIMER_MIDI_UPDATE, 16, NULL);  // ~60Hz update
            return 0;

        case WM_EXITSIZEMOVE:
            // Window drag/resize finished - stop timer
            g_isWindowDragging = false;
            KillTimer(hWnd, TIMER_MIDI_UPDATE);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_MIDI_UPDATE && g_isWindowDragging) {
                // Continue MIDI playback during window drag
                UpdateMIDIPlayback();
                UpdateDrumStates();
                CleanupStuckChannels();
            }
            return 0;

        case WM_HOTKEY:
            // Handle global media key presses
            if (g_enableGlobalMediaKeys) {
                switch (wParam) {
                    case HK_PLAY_PAUSE:
                        // Toggle play/pause
                        if (g_midiPlayer.isPlaying) {
                            if (g_midiPlayer.isPaused) {
                                PlayMIDI();  // Resume
                            } else {
                                PauseMIDI();  // Pause
                            }
                        } else {
                            PlayMIDI();  // Start playing
                        }
                        break;

                    case HK_NEXT_TRACK:
                        PlayNextMIDI();
                        break;

                    case HK_PREV_TRACK:
                        PlayPreviousMIDI();
                        break;
                }
            }
            return 0;

        case WM_KEYDOWN:
            HandleKeyPress((int)wParam);
            return 0;

        case WM_KEYUP:
            HandleKeyRelease((int)wParam);
            return 0;

        case WM_DESTROY:
            SaveFrequenciesToINI();
            SaveSlotConfigToINI();
            if (g_enableGlobalMediaKeys) {
                UnregisterGlobalMediaKeys();
            }
            DisconnectHardware();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===== Main =====

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Set DPI awareness
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(void*);
        SetProcessDpiAwarenessContext_t func =
            (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (func) {
            func((void*)-4);
        } else {
            HMODULE shcore = LoadLibraryA("shcore.dll");
            if (shcore) {
                typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);
                SetProcessDpiAwareness_t func2 =
                    (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
                if (func2) {
                    func2(2);
                }
                FreeLibrary(shcore);
            }
        }
        FreeLibrary(user32);
    }

    // Initialize INI file paths
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
        snprintf(g_iniFilePath, MAX_PATH, "%sym2163_tuning.ini", exePath);
        snprintf(g_midiConfigPath, MAX_PATH, "%sym2163_midi_config.ini", exePath);
    } else {
        strcpy(g_iniFilePath, "ym2163_tuning.ini");
        strcpy(g_midiConfigPath, "ym2163_midi_config.ini");
    }

    // Create window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"YM2163PianoV11", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"YM2163 Virtual Piano v11 - Quad YM2163",
        WS_OVERLAPPEDWINDOW, 100, 100, 1400, 900, nullptr, nullptr, wc.hInstance, nullptr);

    // Save window handle for global media keys
    g_mainWindow = hwnd;

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_MAXIMIZE);  // Maximize window on startup
    UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.FontGlobalScale = 1.0f;
    io.FontAllowUserScaling = false;

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;

    // First, load a base font with Chinese support
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 20.0f, &fontConfig,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

    if (!font) {
        // Fallback to SimSun
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simsun.ttc", 20.0f, &fontConfig,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }

    if (!font) {
        // Fallback to default
        fontConfig.SizePixels = 20.0f;
        font = io.Fonts->AddFontDefault(&fontConfig);
    }

    // Now merge Korean font (Malgun Gothic) into the same font
    if (font) {
        ImFontConfig mergeConfig;
        mergeConfig.MergeMode = true;  // Merge into previous font
        mergeConfig.OversampleH = 1;
        mergeConfig.OversampleV = 1;
        mergeConfig.PixelSnapH = true;

        // Add Korean glyphs from Malgun Gothic
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 20.0f, &mergeConfig,
            io.Fonts->GetGlyphRangesKorean());

        // Also try to add Japanese glyphs
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msgothic.ttc", 20.0f, &mergeConfig,
            io.Fonts->GetGlyphRangesJapanese());
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // v11: Initialize hardware connection
    LoadFrequenciesFromINI();
    LoadSlotConfigFromINI();
    LoadMIDIConfig();
    InitializeFileBrowser();

    // Load config to UI if starting in Config Mode
    if (!g_useLiveControl) {
        LoadInstrumentConfigToUI(g_selectedInstrument);
    }

    // v11: Auto-connect hardware on startup
    g_lastHardwareCheck = std::chrono::steady_clock::now();
    ConnectHardware();

    // Register global media keys if enabled by default
    if (g_enableGlobalMediaKeys) {
        RegisterGlobalMediaKeys();
    }

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }

        // v11: Auto-detect hardware plug/unplug
        CheckHardwareAutoConnect();

        // Update MIDI playback
        UpdateMIDIPlayback();

        // Update drum states
        UpdateDrumStates();

        // Cleanup stuck channels
        CleanupStuckChannels();

        // v10: Update level meters
        UpdateChannelLevels();
        UpdateDrumLevels();

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render UI
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("YM2163 Piano", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::BeginChild("LeftPane", ImVec2(300, 0), true);
        RenderControls();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("RightPane", ImVec2(0, 0), false);

        // v10: Two-row level meter layout for better space utilization
        // Calculate heights
        float pianoHeight = 150;
        float levelMeterHeight = 200;  // Increased for two rows
        float statusAreaWidth = 560;  // 2 boxes side by side (each box ~270px)
        float topSectionHeight = pianoHeight + levelMeterHeight;  // Piano + Level meters

        // Top section: Piano + Level meters (left) | Channel Status (right)
        ImGui::BeginGroup();

        // Piano keyboard
        ImGui::BeginChild("PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
        RenderPianoKeyboard();
        ImGui::EndChild();

        // Level meters below piano (two rows: melody + rhythm)
        ImGui::BeginChild("LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
        RenderLevelMeters();
        ImGui::EndChild();

        ImGui::EndGroup();

        ImGui::SameLine();

        // Channel status display - v10: 2x2 grid of chip status boxes (aligned to top)
        ImGui::BeginChild("StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
        RenderChannelStatus();
        ImGui::EndChild();

        // Bottom section: split into MIDI Player (left) and Log (right)
        ImGui::BeginChild("BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
        RenderMIDIPlayer();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("BottomRight", ImVec2(0, 0), true);
        RenderLog();
        RenderMIDIFolderHistory();
        ImGui::EndChild();

        ImGui::EndChild();

        ImGui::End();

        // Render tuning window if open
        RenderTuningWindow();

        // Check if any input field is active (disable keyboard piano)
        g_isInputActive = ImGui::IsAnyItemActive();

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.45f, 0.55f, 0.60f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        // Frame rate control: ~60 FPS (16ms per frame)
        // This prevents high CPU usage and ensures stable playback even when other windows are active
        Sleep(16);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
