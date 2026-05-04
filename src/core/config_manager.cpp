// config_manager.cpp - Configuration File Management Module Implementation
// Handles loading/saving INI config files for tuning, MIDI instruments, and slots

#include "config_manager.h"
#include "chip_control.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

namespace Config {

// ===== Global Variable Definitions =====

char g_iniFilePath[MAX_PATH] = {0};
char g_midiConfigPath[MAX_PATH] = {0};
std::map<int, InstrumentConfig> g_instrumentConfigs;
std::map<int, DrumConfig> g_drumConfigs;

// ===== Path Initialization =====

void InitConfigPaths() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
        snprintf(g_iniFilePath, MAX_PATH, "%sym2163_config.ini", exePath);
        snprintf(g_midiConfigPath, MAX_PATH, "%sym2163_config.ini", exePath);
    } else {
        strcpy(g_iniFilePath, "ym2163_config.ini");
        strcpy(g_midiConfigPath, "ym2163_config.ini");
    }
}

// ===== Frequency Table INI =====

void SaveFrequenciesToINI() {
    char buffer[32];

    sprintf(buffer, "%d", YM2163::g_fnum_b2);
    WritePrivateProfileStringA("Frequencies", "B2", buffer, g_iniFilePath);

    for (int i = 0; i < 12; i++) {
        sprintf(buffer, "%d", YM2163::g_fnums[i]);
        WritePrivateProfileStringA("Frequencies", YM2163::g_noteNames[i], buffer, g_iniFilePath);
    }

    for (int i = 0; i < 12; i++) {
        sprintf(buffer, "%d", YM2163::g_fnums_c7[i]);
        WritePrivateProfileStringA("Frequencies_C7", YM2163::g_noteNames[i], buffer, g_iniFilePath);
    }
}

void LoadFrequenciesFromINI() {
    UINT b2_value = GetPrivateProfileIntA("Frequencies", "B2", 0, g_iniFilePath);
    if (b2_value > 0 && b2_value <= 2047) {
        YM2163::g_fnum_b2 = b2_value;
    }

    for (int i = 0; i < 12; i++) {
        UINT value = GetPrivateProfileIntA("Frequencies", YM2163::g_noteNames[i], 0, g_iniFilePath);
        if (value > 0 && value <= 2047) {
            YM2163::g_fnums[i] = value;
        }
    }

    for (int i = 0; i < 12; i++) {
        UINT value = GetPrivateProfileIntA("Frequencies_C7", YM2163::g_noteNames[i], 0, g_iniFilePath);
        if (value <= 2047) {
            YM2163::g_fnums_c7[i] = value;
        }
    }
}

// ===== Slot Configuration INI =====

void SaveSlotConfigToINI() {
    char buffer[32];
    sprintf(buffer, "%d", YM2163::g_enableSecondYM2163 ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot1", buffer, g_iniFilePath);
    sprintf(buffer, "%d", YM2163::g_enableThirdYM2163 ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot2", buffer, g_iniFilePath);
    sprintf(buffer, "%d", YM2163::g_enableFourthYM2163 ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot3", buffer, g_iniFilePath);
    sprintf(buffer, "%d", YM2163::g_enableSlot3_2MHz ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot3_2MHz", buffer, g_iniFilePath);
    sprintf(buffer, "%d", YM2163::g_enableSlot3Overflow ? 1 : 0);
    WritePrivateProfileStringA("Slots", "Slot3Overflow", buffer, g_iniFilePath);
    sprintf(buffer, "%d", YM2163::g_slot3_2MHz_Range);
    WritePrivateProfileStringA("Slots", "Slot3_2MHz_Range", buffer, g_iniFilePath);
}

void LoadSlotConfigFromINI() {
    YM2163::g_enableSecondYM2163  = GetPrivateProfileIntA("Slots", "Slot1",           1, g_iniFilePath) != 0;
    YM2163::g_enableThirdYM2163   = GetPrivateProfileIntA("Slots", "Slot2",           0, g_iniFilePath) != 0;
    YM2163::g_enableFourthYM2163  = GetPrivateProfileIntA("Slots", "Slot3",           0, g_iniFilePath) != 0;
    YM2163::g_enableSlot3_2MHz    = GetPrivateProfileIntA("Slots", "Slot3_2MHz",      0, g_iniFilePath) != 0;
    YM2163::g_enableSlot3Overflow = GetPrivateProfileIntA("Slots", "Slot3Overflow",   0, g_iniFilePath) != 0;
    YM2163::g_slot3_2MHz_Range    = GetPrivateProfileIntA("Slots", "Slot3_2MHz_Range",1, g_iniFilePath);
}

// ===== MIDI Config Loading =====

void LoadMIDIConfig() {
    YM2163::log_command("=== Loading MIDI Configuration ===");

    // Load global pedal mode
    char pedalModeStr[32];
    GetPrivateProfileStringA("Settings", "PedalMode", "Disabled", pedalModeStr, sizeof(pedalModeStr), g_midiConfigPath);
    if (strcmp(pedalModeStr, "Piano") == 0) YM2163::g_pedalMode = 1;
    else if (strcmp(pedalModeStr, "Organ") == 0) YM2163::g_pedalMode = 2;
    else YM2163::g_pedalMode = 0;

    // Parse instrument configs (0-127)
    for (int i = 0; i < 128; i++) {
        char section[32];
        sprintf(section, "Instrument_%d", i);

        char name[128];
        char envelope[32];
        char wave[32];
        char pmode[32];

        GetPrivateProfileStringA(section, "Name",      "",       name,     sizeof(name),     g_midiConfigPath);
        GetPrivateProfileStringA(section, "Envelope",  "Decay",  envelope, sizeof(envelope), g_midiConfigPath);
        GetPrivateProfileStringA(section, "Wave",      "Piano",  wave,     sizeof(wave),     g_midiConfigPath);
        GetPrivateProfileStringA(section, "PedalMode", "",       pmode,    sizeof(pmode),    g_midiConfigPath);

        InstrumentConfig config;
        config.name = name;

        if      (strcmp(envelope, "Decay")  == 0) config.envelope = 0;
        else if (strcmp(envelope, "Fast")   == 0) config.envelope = 1;
        else if (strcmp(envelope, "Medium") == 0) config.envelope = 2;
        else if (strcmp(envelope, "Slow")   == 0) config.envelope = 3;
        else config.envelope = 0;

        if      (strcmp(wave, "String")      == 0) config.wave = 1;
        else if (strcmp(wave, "Organ")       == 0) config.wave = 2;
        else if (strcmp(wave, "Clarinet")    == 0) config.wave = 3;
        else if (strcmp(wave, "Piano")       == 0) config.wave = 4;
        else if (strcmp(wave, "Harpsichord") == 0) config.wave = 5;
        else config.wave = 4;

        if      (strcmp(pmode, "Piano") == 0) config.pedalMode = 1;
        else if (strcmp(pmode, "Organ") == 0) config.pedalMode = 2;
        else config.pedalMode = 0;

        g_instrumentConfigs[i] = config;
    }

    // Parse drum configs (note 27-63)
    for (int i = 27; i <= 63; i++) {
        char section[32];
        sprintf(section, "Drum_%d", i);

        char name[128];
        char drums[128];
        GetPrivateProfileStringA(section, "Name",  "",    name,  sizeof(name),  g_midiConfigPath);
        GetPrivateProfileStringA(section, "Drums", "SDN", drums, sizeof(drums), g_midiConfigPath);

        DrumConfig config;
        config.name = name;

        char* token = strtok(drums, ",");
        while (token != NULL) {
            while (*token == ' ') token++;
            if      (strcmp(token, "BD")  == 0) config.drumBits.push_back(0x01);
            else if (strcmp(token, "HC")  == 0) config.drumBits.push_back(0x02);
            else if (strcmp(token, "SDN") == 0) config.drumBits.push_back(0x04);
            else if (strcmp(token, "HHO") == 0) config.drumBits.push_back(0x08);
            else if (strcmp(token, "HHD") == 0) config.drumBits.push_back(0x10);
            token = strtok(NULL, ",");
        }

        g_drumConfigs[i] = config;
    }

    YM2163::log_command("MIDI configuration loaded: %d instruments, %d drums, Pedal Mode: %d",
                        (int)g_instrumentConfigs.size(), (int)g_drumConfigs.size(), YM2163::g_pedalMode);
}

} // namespace Config


