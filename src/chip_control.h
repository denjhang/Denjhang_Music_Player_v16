// ym2163_control.h - YM2163 Chip Control Module
// Handles FTDI communication and YM2163 chip operations

#ifndef YM2163_CONTROL_H
#define YM2163_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <chrono>
#include <string>

// External FTDI header
extern "C" {
#include "ftdi_driver/ftd2xx.h"
}

namespace YM2163 {

// ===== Constants =====
constexpr int MAX_CHANNELS = 16;
constexpr int MAX_CHIPS = 4;
constexpr int CHANNELS_PER_CHIP = 4;

// ===== Channel State Structure =====
struct ChannelState {
    int note;
    int octave;
    uint16_t fnum;
    bool active;
    int midiChannel;
    int timbre;
    int envelope;
    int volume;
    int chipIndex;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point releaseTime;
    bool hasBeenUsed;
    float currentLevel;
    float previousLevel;  // For smooth transitions when retriggering

    ChannelState() : note(0), octave(0), fnum(0), active(false), midiChannel(-1),
                     timbre(0), envelope(0), volume(0), chipIndex(0),
                     hasBeenUsed(false), currentLevel(0.0f), previousLevel(0.0f) {}
};

// ===== Velocity Analysis Structure =====
struct VelocityAnalysis {
    int velocityHistogram[128];
    int totalNotes;
    int minVelocity;
    int maxVelocity;
    float avgVelocity;
    int peakVelocity;
    int mostCommonVelocity1;
    int mostCommonVelocity2;
    int threshold_0dB;
    int threshold_6dB;
    int threshold_12dB;
    int threshold_mute;

    VelocityAnalysis();
    void reset();
};

// ===== Global State Accessors =====
extern FT_HANDLE g_ftHandle;
extern bool g_hardwareConnected;
extern ChannelState g_channels[MAX_CHANNELS];

// ===== Settings Accessors =====
extern int g_currentTimbre;
extern int g_currentEnvelope;
extern int g_currentVolume;
extern bool g_useLiveControl;
extern bool g_enableVelocityMapping;
extern bool g_enableDynamicVelocityMapping;
extern bool g_enableSustainPedal;
extern bool g_sustainPedalActive;
extern int g_pedalMode;
extern bool g_enableSecondYM2163;
extern bool g_enableThirdYM2163;
extern bool g_enableFourthYM2163;
extern bool g_enableSlot3_2MHz;
extern bool g_enableSlot3Overflow;
extern int g_slot3_2MHz_Range;

// ===== Frequency Tables =====
extern int g_fnums[12];
extern int g_fnum_b2;
extern int g_fnums_c7[12];

// ===== FTDI Communication =====
int ftdi_init(int dev_idx);
bool CheckHardwarePresent();
bool CheckHardwareReady();
void ConnectHardware();
void DisconnectHardware();
void CheckHardwareAutoConnect();
void write_melody_cmd_chip(uint8_t data, int chipIndex);
void write_melody_cmd(uint8_t data);

// ===== YM2163 Initialization =====
void ym2163_init();
void init_single_ym2163(int chipIndex);
void ResetYM2163Chip(int chipIndex);
void ResetAllYM2163Chips();
void InitializeAllChannels();

// ===== Note Control =====
int find_free_channel();
int find_free_channel_slot3();
int find_channel_playing(int note, int octave);
void play_note(int channel, int note, int octave, int timbre = -1, int envelope = -1, int volume = -1);
void stop_note(int channel);
void stop_all_notes();

// ===== Drum Control =====
void play_drum(uint8_t rhythm_bit);
void UpdateDrumStates();

// ===== Level Meter Functions =====
float CalculateEnvelopeLevel(int envelope, bool active,
                             std::chrono::steady_clock::time_point startTime,
                             std::chrono::steady_clock::time_point releaseTime);
void UpdateChannelLevels();
void UpdateDrumLevels();

// ===== Velocity Mapping =====
int map_velocity_to_volume(int velocity);
void AnalyzeVelocityDistribution(void* midiFilePtr);

// ===== Utility Functions =====
int get_key_index(int octave, int note);
bool is_in_valid_range(int note, int octave);
bool isChannelEnabled(int ch);
float getNormalChannelUsage();
void CleanupStuckChannels();
void ResetPianoKeyStates();

// ===== Velocity Analysis Instance =====
extern VelocityAnalysis g_velocityAnalysis;

// ===== Drum State (needed by GUI) =====
extern bool  g_drumActive[MAX_CHIPS][5];
extern float g_drumLevels[MAX_CHIPS][5];
extern const char* g_noteNames[12];
extern const bool g_isBlackNote[12];
extern const char* g_drumNames[5];
extern bool g_manualDisconnect;

// ===== Logging ====
void log_command(const char* format, ...);
const std::string& GetLogBuffer();
void ClearLogBuffer();

} // namespace YM2163

#endif // YM2163_CONTROL_H