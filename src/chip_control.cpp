// ym2163_control.cpp - YM2163 Chip Control Module Implementation
// Handles FTDI communication, YM2163 chip initialization, note control, and drum control

#include "chip_control.h"
#include "chip_window_ym2163.h"
#include "gui_renderer.h"  // For piano key state
#include "spfm_manager.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include <climits>
#include <cmath>

namespace YM2163 {

// ===== Global Variable Definitions =====

FT_HANDLE g_ftHandle = NULL;
bool g_hardwareConnected = false;
ChannelState g_channels[MAX_CHANNELS];

int g_currentTimbre = 4;
int g_currentEnvelope = 1;
int g_currentVolume = 0;
bool g_useLiveControl = false;
bool g_enableVelocityMapping = true;
bool g_enableDynamicVelocityMapping = true;
bool g_enableSustainPedal = true;
bool g_sustainPedalActive = false;
int g_pedalMode = 0;
bool g_enableSecondYM2163 = true;
bool g_enableThirdYM2163 = false;
bool g_enableFourthYM2163 = false;
bool g_enableSlot3_2MHz = false;
bool g_enableSlot3Overflow = false;
int g_slot3_2MHz_Range = 1;

// Frequency tables - YM2163 F-Number values for 4MHz clock
// These values produce correct pitches for the YM2163/YM2413 family
int g_fnums[12] = {
    951, 900, 852, 803, 756, 716, 674, 637, 601, 567, 535, 507
};
int g_fnum_b2 = 1014;
int g_fnums_c7[12] = {
    475, 450, 426, 401, 378, 358, 337, 318, 300, 283, 267, 0
};

// Internal state
static std::string g_logBuffer;
static bool g_expectingData = false;
static uint8_t g_lastRegAddr = 0;
bool g_manualDisconnect = false;
static std::chrono::steady_clock::time_point g_lastHardwareCheck;
static int g_hardwareConnectRetries = 0;
static const int HARDWARE_CHECK_INTERVAL_MS = 500;
static const int HARDWARE_MAX_RETRIES = 6;

// Drum state
static const uint8_t g_drumBits[5] = {0x10, 0x08, 0x04, 0x02, 0x01};
bool g_drumActive[MAX_CHIPS][5] = {};
static std::chrono::steady_clock::time_point g_drumTriggerTime[MAX_CHIPS][5];
static int g_currentDrumChip = 0;
float g_drumLevels[MAX_CHIPS][5] = {};

// Note/drum name tables (shared with GUI)
const char* g_noteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
const bool g_isBlackNote[12] = {
    false, true, false, true, false, false, true, false, true, false, true, false
};
const char* g_drumNames[5] = {"BD", "HC", "SDN", "HHO", "HHD"};

// Channel velocity tracking
static float g_channelLevels[MAX_CHANNELS] = {};

// Velocity analysis
VelocityAnalysis g_velocityAnalysis;

// ===== VelocityAnalysis Implementation =====

VelocityAnalysis::VelocityAnalysis() {
    reset();
}

void VelocityAnalysis::reset() {
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

const std::string& GetLogBuffer() {
    return g_logBuffer;
}

void ClearLogBuffer() {
    g_logBuffer.clear();
}

// ===== FTDI Communication =====

int ftdi_init(int dev_idx) {
    FT_STATUS status;

    status = FT_Open(dev_idx, &g_ftHandle);
    if (status != FT_OK) {
        log_command("FT_Open failed: %d", status);
        return -1;
    }

    status = FT_ResetDevice(g_ftHandle);
    status = FT_SetBaudRate(g_ftHandle, 1500000);  // 使用 1.5Mbps，与 v12 一致
    if (status != FT_OK) {
        log_command("FT_SetBaudRate failed: %d", status);
        FT_Close(g_ftHandle);
        g_ftHandle = NULL;
        return -1;
    }

    status = FT_SetDataCharacteristics(g_ftHandle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    status = FT_SetFlowControl(g_ftHandle, FT_FLOW_NONE, 0, 0);
    status = FT_SetTimeouts(g_ftHandle, 100, 100);
    status = FT_SetLatencyTimer(g_ftHandle, 2);
    FT_Purge(g_ftHandle, FT_PURGE_RX | FT_PURGE_TX);

    log_command("FTDI initialized successfully");
    return 0;
}

bool CheckHardwarePresent() {
    DWORD numDevs = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevs);
    return (status == FT_OK && numDevs > 0);
}

bool CheckHardwareReady() {
    DWORD numDevs = 0;
    FT_STATUS status = FT_CreateDeviceInfoList(&numDevs);
    if (status != FT_OK || numDevs == 0) return false;

    FT_DEVICE_LIST_INFO_NODE devInfo;
    status = FT_GetDeviceInfoDetail(0, &devInfo.Flags, &devInfo.Type,
        &devInfo.ID, &devInfo.LocId, devInfo.SerialNumber,
        devInfo.Description, &devInfo.ftHandle);
    if (status != FT_OK) return false;

    return !(devInfo.Flags & 0x01);
}

void ConnectHardware() {
    if (g_hardwareConnected) return;
    // SPFMManager handles connection now
    g_hardwareConnected = SPFMManager::IsConnected();
    if (g_hardwareConnected) {
        g_manualDisconnect = false;
        ym2163_init();
        log_command("Hardware connected via SPFMManager");
    }
}

void DisconnectHardware() {
    stop_all_notes();
    g_hardwareConnected = false;
    g_manualDisconnect = true;
    g_ftHandle = NULL;
    log_command("Hardware disconnected");
}

void CheckHardwareAutoConnect() {
    // SPFMManager::Update() handles device detection now
    g_hardwareConnected = SPFMManager::IsConnected();
    if (g_hardwareConnected && g_manualDisconnect) {
        g_manualDisconnect = false;
    }
}

void write_melody_cmd_chip(uint8_t data, int chipIndex) {
    SPFMManager::WriteYM2163((uint8_t)chipIndex, data);

    if (!g_expectingData) {
        g_lastRegAddr = data;
        g_expectingData = true;
    } else {
        g_expectingData = false;
    }
}

void write_melody_cmd(uint8_t data) {
    write_melody_cmd_chip(data, 0);
}

// ===== YM2163 Initialization =====

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
    SPFMManager::RawWrite(reset_cmd, 4);
    Sleep(200);

    log_command("=== YM2163 Initialization ===");

    init_single_ym2163(0);
    if (g_enableSecondYM2163)  init_single_ym2163(1);
    if (g_enableThirdYM2163)   init_single_ym2163(2);
    if (g_enableFourthYM2163)  init_single_ym2163(3);

    int totalChannels = 4;
    if (g_enableSecondYM2163) totalChannels += 4;
    if (g_enableThirdYM2163)  totalChannels += 4;
    if (g_enableFourthYM2163) totalChannels += 4;
    log_command("YM2163 mode: %d chips, %d channels", totalChannels / 4, totalChannels);
}

void InitializeAllChannels() {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        g_channels[i] = ChannelState();
        g_channels[i].chipIndex = i / CHANNELS_PER_CHIP;
    }
}

// ===== Channel / Note Control =====

bool isChannelEnabled(int ch) {
    if (ch < 4) return true;
    if (ch < 8)  return g_enableSecondYM2163;
    if (ch < 12) return g_enableThirdYM2163;
    return g_enableFourthYM2163;
}

int find_free_channel() {
    auto now = std::chrono::steady_clock::now();
    // Prefer inactive channels first
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!isChannelEnabled(i)) continue;
        if (g_enableSlot3_2MHz && i >= 12) continue;  // Reserve Slot3 for 2MHz
        if (!g_channels[i].active) return i;
    }
    // Steal oldest active channel
    int oldest = -1;
    auto oldestTime = now;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!isChannelEnabled(i)) continue;
        if (g_enableSlot3_2MHz && i >= 12) continue;
        if (g_channels[i].active && g_channels[i].startTime <= oldestTime) {
            oldestTime = g_channels[i].startTime;
            oldest = i;
        }
    }
    if (oldest >= 0) stop_note(oldest);
    return oldest;
}

int find_free_channel_slot3() {
    for (int i = 12; i < 16; i++) {
        if (!g_channels[i].active) return i;
    }
    // Steal lowest/oldest
    int channelToReplace = -1;
    int lowestOctave = INT_MAX;
    auto oldestStart = std::chrono::steady_clock::now();
    for (int i = 12; i < 16; i++) {
        if (!g_channels[i].active) continue;
        int oct = g_channels[i].octave;
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
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (g_channels[i].active &&
            g_channels[i].note == note &&
            g_channels[i].octave == octave)
            return i;
    }
    return -1;
}

// ===== Helper Functions =====

int get_key_index(int octave, int note) {
    if (octave == 0 && note == 11) return 0;
    if (octave >= 1 && octave <= 5) return (octave - 1) * 12 + note + 1;
    if (octave == 6) return 61 + note;
    return -1;
}

bool is_in_valid_range(int note, int octave) {
    if (octave == 0 && note == 11) return true;
    if (octave >= 1 && octave <= 5) return true;
    if (octave == 6 && g_enableSlot3_2MHz) return true;
    return false;
}

float getNormalChannelUsage() {
    int maxCh = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0);
    int active = 0;
    for (int i = 0; i < maxCh; i++) {
        if (g_channels[i].active) active++;
    }
    return maxCh > 0 ? (float)active / maxCh : 0.0f;
}

void CleanupStuckChannels() {
    auto now = std::chrono::steady_clock::now();
    int maxChannels = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0) + (g_enableFourthYM2163 ? 4 : 0);
    for (int i = 0; i < maxChannels; i++) {
        if (g_channels[i].active) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_channels[i].startTime);
            if (duration.count() > 10000) stop_note(i);
        }
    }
}

void ResetPianoKeyStates() {
    for (int i = 0; i < 73; i++) {
        g_pianoKeyPressed[i] = false;
        g_pianoKeyChipIndex[i] = -1;
        g_pianoKeyLevel[i] = 0.0f;
    }
}

// ===== Note Play/Stop =====

void play_note(int channel, int note, int octave, int timbre, int envelope, int volume) {
    if (channel < 0 || channel >= MAX_CHANNELS) return;

    int chipIndex = g_channels[channel].chipIndex;
    int localChannel = channel % CHANNELS_PER_CHIP;

    uint16_t fnum;
    uint8_t hw_octave;

    if (g_enableSlot3_2MHz && chipIndex == 3) {
        bool isInRange = (g_slot3_2MHz_Range == 0) ? (octave == 6) : (octave >= 5 && octave <= 6);
        if (isInRange) {
            fnum = (octave == 6) ? g_fnums_c7[note] : g_fnums[note];
            hw_octave = 3;
        } else if (octave == 6 || (octave == 5 && g_slot3_2MHz_Range == 1)) {
            return;
        } else if (octave >= 2 && octave <= 5) {
            int remappedOctave = octave - 1;
            fnum = g_fnums[note];
            hw_octave = (remappedOctave - 1) & 0x03;
        } else { return; }
    } else if (g_enableSlot3_2MHz && (octave == 6 || (octave == 5 && g_slot3_2MHz_Range == 1))) {
        return;
    } else if (octave == 0 && note == 11) {
        fnum = g_fnum_b2; hw_octave = 0;
    } else if (octave >= 1 && octave <= 4) {
        fnum = g_fnums[note]; hw_octave = (octave - 1) & 0x03;
    } else if (octave == 5) {
        fnum = g_fnums_c7[note]; hw_octave = 3;
    } else { return; }

    uint8_t fnum_low  = fnum & 0x7F;
    uint8_t fnum_high = (fnum >> 7) & 0x07;

    g_channels[channel].note      = note;
    g_channels[channel].octave    = octave;
    g_channels[channel].fnum      = fnum;
    g_channels[channel].active    = true;
    g_channels[channel].startTime = std::chrono::steady_clock::now();

    int useTimbre   = (timbre   >= 0) ? timbre   : g_currentTimbre;
    int useEnvelope = (envelope >= 0) ? envelope : g_currentEnvelope;
    int useVolume   = (volume   >= 0) ? volume   : g_currentVolume;

    g_channels[channel].timbre   = useTimbre;
    g_channels[channel].envelope = useEnvelope;
    g_channels[channel].volume   = useVolume;

    // Update piano key visual state
    int keyIdx = get_key_index(octave, note);
    if (keyIdx >= 0 && keyIdx < 73) {
        g_pianoKeyPressed[keyIdx] = true;
        g_pianoKeyVelocity[keyIdx] = 96;  // Default velocity
        g_pianoKeyFromKeyboard[keyIdx] = false;
        g_pianoKeyOnSlot3_2MHz[keyIdx] = (g_enableSlot3_2MHz && chipIndex == 3);
        g_pianoKeyChipIndex[keyIdx] = chipIndex;  // Track which chip is playing
    }

    write_melody_cmd_chip(0x88 + localChannel, chipIndex);
    write_melody_cmd_chip((useTimbre & 0x07) | ((useEnvelope & 0x03) << 4), chipIndex);
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
    if (channel < 0 || channel >= MAX_CHANNELS) return;

    int chipIndex    = g_channels[channel].chipIndex;
    int localChannel = channel % CHANNELS_PER_CHIP;
    int note   = g_channels[channel].note;
    int octave = g_channels[channel].octave;
    uint16_t fnum = g_channels[channel].fnum;

    uint8_t hw_octave;
    if (g_enableSlot3_2MHz && chipIndex == 3) {
        bool isInRange = (g_slot3_2MHz_Range == 0) ? (octave == 6) : (octave >= 5 && octave <= 6);
        if (isInRange)                        hw_octave = 3;
        else if (octave >= 2 && octave <= 5)  hw_octave = (uint8_t)(((octave - 2)) & 0x03);
        else return;
    } else if (octave == 0 && note == 11) {
        hw_octave = 0;
    } else if (octave >= 1 && octave <= 4) {
        hw_octave = (octave - 1) & 0x03;
    } else if (octave == 5) {
        hw_octave = 3;
    } else { return; }

    uint8_t fnum_low  = fnum & 0x7F;
    uint8_t fnum_high = (fnum >> 7) & 0x07;

    write_melody_cmd_chip(0x80 + localChannel, chipIndex);
    write_melody_cmd_chip(fnum_low, chipIndex);
    write_melody_cmd_chip(0x84 + localChannel, chipIndex);
    write_melody_cmd_chip((hw_octave << 3) | fnum_high, chipIndex);

    // Don't clear piano key state here - let UpdateChannelLevels
    // handle the release fadeout visual via g_pianoKeyLevel

    g_channels[channel].releaseTime = std::chrono::steady_clock::now();
    g_channels[channel].active      = false;
    g_channels[channel].midiChannel = -1;
}

void stop_all_notes() {
    int maxChannels = 4 + (g_enableSecondYM2163 ? 4 : 0) + (g_enableThirdYM2163 ? 4 : 0) + (g_enableFourthYM2163 ? 4 : 0);
    for (int i = 0; i < maxChannels; i++) {
        if (g_channels[i].active) stop_note(i);
    }
    for (int chip = 0; chip < MAX_CHIPS; chip++) {
        for (int i = 0; i < 5; i++) g_drumActive[chip][i] = false;
    }
}

// ===== Drum Control =====

void play_drum(uint8_t rhythm_bit) {
    int chipIndex = 0;
    if (g_enableSecondYM2163) {
        chipIndex = g_currentDrumChip;
        g_currentDrumChip = 1 - g_currentDrumChip;
        log_command("Drum triggered on Chip %d (next will use Chip %d)", chipIndex, g_currentDrumChip);
    }
    write_melody_cmd_chip(0x90, chipIndex);
    write_melody_cmd_chip(rhythm_bit, chipIndex);
    for (int i = 0; i < 5; i++) {
        if (rhythm_bit & g_drumBits[i]) {
            g_drumActive[chipIndex][i] = true;
            g_drumTriggerTime[chipIndex][i] = std::chrono::steady_clock::now();
        }
    }
}

void UpdateDrumStates() {
    auto now = std::chrono::steady_clock::now();
    for (int chip = 0; chip < MAX_CHIPS; chip++) {
        for (int i = 0; i < 5; i++) {
            if (g_drumActive[chip][i]) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_drumTriggerTime[chip][i]);
                if (elapsed.count() > 100) g_drumActive[chip][i] = false;
            }
        }
    }
}

// ===== Level Meter =====

float CalculateEnvelopeLevel(int envelope, bool active,
                             std::chrono::steady_clock::time_point startTime,
                             std::chrono::steady_clock::time_point releaseTime) {
    auto now = std::chrono::steady_clock::now();
    if (active) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
        float t = elapsed.count() / 1000.0f;
        switch (envelope) {
            case 0: return expf(-t * 1.0f);
            case 1: if (t < 0.05f) return t / 0.05f; return expf(-(t - 0.05f) * 0.5f);
            case 2: if (t < 0.1f)  return t / 0.1f;  return 0.7f + 0.3f * expf(-(t - 0.1f) * 0.3f);
            case 3: if (t < 0.2f)  return t / 0.2f;  return 0.8f + 0.2f * expf(-(t - 0.2f) * 0.1f);
            default: return 1.0f;
        }
    } else {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - releaseTime);
        float t = elapsed.count() / 1000.0f;
        switch (envelope) {
            case 0: return expf(-t * 50.0f);  // Decay: instant release
            case 1: return expf(-t * 25.0f);  // Fast: very fast release
            case 2: return expf(-t * 10.0f);  // Medium: fast release
            case 3: return expf(-t * 4.0f);   // Slow: medium release
            default: return expf(-t * 12.0f);
        }
    }
}

void UpdateChannelLevels() {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        float targetLevel = CalculateEnvelopeLevel(
            g_channels[i].envelope, g_channels[i].active,
            g_channels[i].startTime, g_channels[i].releaseTime);

        // Smooth transition: blend towards target level
        // This prevents flicker when same channel retriggers quickly
        float current = g_channels[i].currentLevel;
        float blendFactor = 0.5f;  // Faster response

        // If channel just became active and previous level was high,
        // start from previous level instead of jumping
        if (g_channels[i].active && g_channels[i].previousLevel > 0.1f) {
            blendFactor = 0.7f;  // Very fast rise for retrigger
        }

        // Smooth blending towards target
        g_channels[i].currentLevel = current + (targetLevel - current) * blendFactor;

        // Store for next frame
        if (!g_channels[i].active) {
            g_channels[i].previousLevel = g_channels[i].currentLevel;
        }

        g_channelLevels[i] = g_channels[i].currentLevel;

        // Update piano key visual level from channel level
        int keyIdx = get_key_index(g_channels[i].octave, g_channels[i].note);
        if (keyIdx >= 0 && keyIdx < 73 && g_channels[i].hasBeenUsed) {
            g_pianoKeyLevel[keyIdx] = g_channels[i].currentLevel;
            // Clear key state when level drops to near zero
            if (g_pianoKeyLevel[keyIdx] < 0.01f && !g_channels[i].active) {
                g_pianoKeyPressed[keyIdx] = false;
                g_pianoKeyChipIndex[keyIdx] = -1;
                g_pianoKeyLevel[keyIdx] = 0.0f;
            }
        }
    }
}

void UpdateDrumLevels() {
    auto now = std::chrono::steady_clock::now();
    for (int chip = 0; chip < MAX_CHIPS; chip++) {
        for (int i = 0; i < 5; i++) {
            if (g_drumActive[chip][i]) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_drumTriggerTime[chip][i]);
                float t = elapsed.count() / 100.0f;
                g_drumLevels[chip][i] = expf(-t * 5.0f);  // Fast drum decay
            } else {
                g_drumLevels[chip][i] = 0.0f;
            }
        }
    }
}

// ===== Velocity Mapping =====

int map_velocity_to_volume(int velocity) {
    if (!g_enableVelocityMapping) return g_currentVolume;
    if (g_enableDynamicVelocityMapping) {
        if (velocity < g_velocityAnalysis.threshold_mute)  return 3; // mute
        if (velocity < g_velocityAnalysis.threshold_12dB)  return 2;
        if (velocity < g_velocityAnalysis.threshold_6dB)   return 1;
        return 0;
    }
    if (velocity < 32)  return 3;
    if (velocity < 64)  return 2;
    if (velocity < 96)  return 1;
    return 0;
}

void AnalyzeVelocityDistribution(void* midiFilePtr) {
    if (!midiFilePtr) return;
    // Analysis performed by MIDI player module and stored in g_velocityAnalysis
}

void ResetYM2163Chip(int chipIndex) {
    if (!SPFMManager::IsConnected()) return;
    log_command("Resetting YM2163 Chip %d...", chipIndex);
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x88 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);
    }
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x8C + ch, chipIndex);
        write_melody_cmd_chip(0x03, chipIndex);
    }
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x84 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);
    }
    for (int ch = 0; ch < 4; ch++) {
        write_melody_cmd_chip(0x80 + ch, chipIndex);
        write_melody_cmd_chip(0x00, chipIndex);
    }
    write_melody_cmd_chip(0x90, chipIndex);
    write_melody_cmd_chip(0x00, chipIndex);
    log_command("YM2163 Chip %d reset complete", chipIndex);
}

void ResetAllYM2163Chips() {
    log_command("=== Resetting all YM2163 chips ===");
    ResetYM2163Chip(0);
    if (g_enableSecondYM2163) ResetYM2163Chip(1);
    if (g_enableThirdYM2163)  ResetYM2163Chip(2);
    if (g_enableFourthYM2163 || g_enableSlot3_2MHz) ResetYM2163Chip(3);
    Sleep(50);
}

} // namespace YM2163