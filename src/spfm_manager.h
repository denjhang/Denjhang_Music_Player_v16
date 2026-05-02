#ifndef SPFM_MANAGER_H
#define SPFM_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

extern "C" {
#include "ftdi_driver/ftd2xx.h"
}

namespace SPFMManager {

enum ChipType {
    CHIP_NONE    = 0,
    CHIP_YM2163  = 1,
    CHIP_SN76489 = 2,
};

static constexpr int MAX_SLOTS = 4;

struct SlotConfig {
    ChipType chipType;
    bool     enabled;
    SlotConfig() : chipType(CHIP_NONE), enabled(false) {}
};

struct DeviceState {
    FT_HANDLE ftHandle;
    bool      connected;
    char      description[64];
    char      serialNumber[16];
    DWORD     locationId;
    SlotConfig slots[MAX_SLOTS];
    ChipType  activeChipType;

    DeviceState() : ftHandle(NULL), connected(false), locationId(0), activeChipType(CHIP_NONE) {
        description[0] = '\0';
        serialNumber[0] = '\0';
        for (int i = 0; i < MAX_SLOTS; i++) {
            slots[i].chipType = CHIP_NONE;
            slots[i].enabled = (i == 0);
        }
    }
};

// ===== Lifecycle =====
void Init();
void Shutdown();
void Update();

// ===== Device Management =====
int  EnumerateDevices();
bool ConnectDevice(int idx);
void DisconnectDevice(int idx);

// ===== Unified Write Path =====
// YM2163: {chipIndex, 0x80, data} immediate write
void WriteYM2163(uint8_t chipIndex, uint8_t data);
// SN76489: {slot, 0x20, data} buffered write (delegates to spfm_lite)
void WriteSN76489Data(uint8_t slot, uint8_t data);
void FlushSN76489();
// Generic raw write
void RawWrite(const uint8_t* data, DWORD len);
// SPFM reset
void SendReset();

// ===== Slot Management =====
ChipType GetSlotType(int slot);
void SetAllSlots(ChipType type);

// ===== State Query =====
bool IsConnected();
ChipType GetActiveChipType();

// ===== Chip Switching (called from any window or SPFM tab) =====
void SwitchToChipType(ChipType type);

// ===== Config =====
void LoadConfig();
void SaveConfig();

// ===== Global =====
extern DeviceState g_device;
extern char g_configPath[MAX_PATH];

} // namespace SPFMManager

#endif // SPFM_MANAGER_H
