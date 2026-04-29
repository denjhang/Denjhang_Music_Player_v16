// config_manager.h - Configuration File Management Module
// Handles loading/saving INI config files for tuning, MIDI instruments, and slots

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <map>
#include <vector>
#include <windows.h>
#include <cstdint>

namespace Config {

// ===== Instrument Config =====
struct InstrumentConfig {
    std::string name;
    int wave;       // 0=none,1=String,2=Organ,3=Clarinet,4=Piano,5=Harpsichord
    int envelope;   // 0=Decay,1=Fast,2=Medium,3=Slow
    int pedalMode;  // 0=global,1=Piano,2=Organ

    InstrumentConfig() : wave(4), envelope(0), pedalMode(0) {}
};

// ===== Drum Config =====
struct DrumConfig {
    std::string name;
    std::vector<uint8_t> drumBits;
};

// ===== Global Config State =====
extern char g_iniFilePath[MAX_PATH];
extern char g_midiConfigPath[MAX_PATH];
extern std::map<int, InstrumentConfig> g_instrumentConfigs;
extern std::map<int, DrumConfig> g_drumConfigs;

// ===== Functions =====
void InitConfigPaths();

void LoadFrequenciesFromINI();
void SaveFrequenciesToINI();

void LoadSlotConfigFromINI();
void SaveSlotConfigToINI();

void LoadMIDIConfig();

} // namespace Config

#endif // CONFIG_MANAGER_H