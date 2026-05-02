// spfm_manager.cpp - Unified FTDI/SPFM Hardware Manager

#include "spfm_manager.h"
#include "sn76489/spfm.h"
#include "chip_control.h"
#include "sn76489_window.h"
#include "ym2413_window.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace SPFMManager {

DeviceState g_device;
char g_configPath[MAX_PATH] = "";

// ===== Internal: SN76489 buffer (moved from spfm_lite.c conceptually) =====
// We use spfm_lite.c's buffer via spfm_set_handle(), but keep a local state
// to know if we need to flush before YM2163 writes.
static bool s_sn76489BufDirty = false;

static void GetExeDir(char* buf, int bufSize) {
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    WideCharToMultiByte(CP_ACP, 0, wbuf, -1, buf, bufSize, NULL, NULL);
    char* slash = strrchr(buf, '\\');
    if (slash) *(slash + 1) = '\0';
}

// ===== Lifecycle =====

void Init() {
    GetExeDir(g_configPath, MAX_PATH);
    strcat(g_configPath, "spfm_config.ini");
    LoadConfig();
    EnumerateDevices();
    // Auto-connect to device 0
    ConnectDevice(0);
}

void Shutdown() {
    DisconnectDevice(0);
}

void Update() {
    // Device removal detection (every ~2s)
    static int frameCount = 0;
    if (++frameCount < 120) return;
    frameCount = 0;

    if (!g_device.connected) return;

    DWORD numDevs = 0;
    if (FT_CreateDeviceInfoList(&numDevs) != FT_OK || numDevs == 0) {
        // Device removed
        DisconnectDevice(0);
    }
}

// ===== Device Management =====

int EnumerateDevices() {
    DWORD numDevs = 0;
    FT_STATUS st = FT_CreateDeviceInfoList(&numDevs);
    if (st != FT_OK) return 0;

    if (numDevs > 0) {
        FT_DEVICE_LIST_INFO_NODE info;
        st = FT_GetDeviceInfoDetail(0, &info.Flags, &info.Type,
            &info.ID, &info.LocId, info.SerialNumber,
            info.Description, &info.ftHandle);
        if (st == FT_OK) {
            strncpy(g_device.description, info.Description, sizeof(g_device.description) - 1);
            strncpy(g_device.serialNumber, info.SerialNumber, sizeof(g_device.serialNumber) - 1);
            g_device.locationId = info.LocId;
        }
    }
    return (int)numDevs;
}

bool ConnectDevice(int idx) {
    if (g_device.connected) return true;
    if (idx != 0) return false;  // Phase 1: single device

    FT_STATUS st;
    st = FT_Open(idx, &g_device.ftHandle);
    if (st != FT_OK) return false;

    // Configure FTDI (same params as YM2163 ftdi_init + SPFM spfm_init)
    FT_ResetDevice(g_device.ftHandle);
    FT_SetBaudRate(g_device.ftHandle, 1500000);
    FT_SetDataCharacteristics(g_device.ftHandle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(g_device.ftHandle, FT_FLOW_NONE, 0, 0);
    FT_SetTimeouts(g_device.ftHandle, 100, 100);
    FT_SetLatencyTimer(g_device.ftHandle, 2);
    FT_SetUSBParameters(g_device.ftHandle, 65536, 65536);
    FT_Purge(g_device.ftHandle, FT_PURGE_RX | FT_PURGE_TX);

    // DTR/RTS reset sequence (from spfm_init)
    FT_SetDtr(g_device.ftHandle); FT_SetRts(g_device.ftHandle);
    Sleep(1);
    FT_ClrDtr(g_device.ftHandle); FT_ClrRts(g_device.ftHandle);
    Sleep(1);
    FT_SetRts(g_device.ftHandle);
    Sleep(1);
    FT_ClrDtr(g_device.ftHandle); FT_ClrRts(g_device.ftHandle);
    Sleep(10);

    // Send SPFM reset
    uint8_t reset = 0xFE;
    DWORD written;
    FT_Write(g_device.ftHandle, &reset, 1, &written);
    Sleep(100);
    FT_Purge(g_device.ftHandle, FT_PURGE_RX | FT_PURGE_TX);

    g_device.connected = true;

    // Inject handle into spfm_lite for SN76489 writes
    ::spfm_set_handle(g_device.ftHandle);
    s_sn76489BufDirty = false;

    // Initialize hardware based on active chip type
    if (g_device.activeChipType != CHIP_NONE) {
        // Chip-specific init happens via the respective window modules
        // after they see IsConnected() = true
    }

    return true;
}

void DisconnectDevice(int idx) {
    if (!g_device.connected || idx != 0) return;

    // Full chip mute before disconnecting
    if (g_device.activeChipType == CHIP_YM2163) {
        YM2163::stop_all_notes();
        YM2163::ResetAllYM2163Chips();
    }
    if (g_device.activeChipType == CHIP_SN76489) {
        SN76489Window::MuteAll();
    }
    if (g_device.activeChipType == CHIP_YM2413) {
        YM2413Window::MuteAll();
    }

    // Flush any pending SN76489 buffer and wait for hardware
    ::spfm_flush();
    Sleep(100);

    // Send reset before closing
    uint8_t reset = 0xFE;
    DWORD written;
    FT_Write(g_device.ftHandle, &reset, 1, &written);
    Sleep(50);

    FT_Close(g_device.ftHandle);
    g_device.ftHandle = NULL;
    g_device.connected = false;
    s_sn76489BufDirty = false;

    // Clear spfm_lite handle
    ::spfm_set_handle(NULL);
}

// ===== Unified Write Path =====

void WriteYM2163(uint8_t chipIndex, uint8_t data) {
    if (!g_device.ftHandle) return;

    // Flush SN76489 buffer first if dirty (ensures command ordering)
    if (s_sn76489BufDirty) {
        ::spfm_flush();
        s_sn76489BufDirty = false;
    }

    uint8_t cmd[3] = {chipIndex, 0x80, data};
    DWORD written;
    FT_Write(g_device.ftHandle, cmd, 3, &written);
    FT_Purge(g_device.ftHandle, FT_PURGE_TX);
}

void WriteSN76489Data(uint8_t slot, uint8_t data) {
    if (!g_device.connected) return;
    s_sn76489BufDirty = true;
    ::spfm_write_data(slot, data);
}

void FlushSN76489() {
    if (!g_device.connected) return;
    ::spfm_flush();
    s_sn76489BufDirty = false;
}

void WriteYM2413(uint8_t reg, uint8_t data) {
    if (!g_device.connected) return;
    s_sn76489BufDirty = true;
    ::spfm_write_reg(0, 0, reg, data);
}

void RawWrite(const uint8_t* data, DWORD len) {
    if (!g_device.ftHandle) return;

    if (s_sn76489BufDirty) {
        ::spfm_flush();
        s_sn76489BufDirty = false;
    }

    DWORD written;
    FT_Write(g_device.ftHandle, (void*)data, len, &written);
    FT_Purge(g_device.ftHandle, FT_PURGE_TX);
}

void SendReset() {
    if (!g_device.connected) return;
    FlushSN76489();
    uint8_t reset = 0xFE;
    DWORD written;
    FT_Write(g_device.ftHandle, &reset, 1, &written);
    Sleep(50);
    FT_Purge(g_device.ftHandle, FT_PURGE_RX | FT_PURGE_TX);
}

// ===== Slot Management =====

ChipType GetSlotType(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return CHIP_NONE;
    return g_device.slots[slot].chipType;
}

void SetAllSlots(ChipType type) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        g_device.slots[i].chipType = type;
        g_device.slots[i].enabled = (i == 0 || type != CHIP_NONE);
    }
    g_device.activeChipType = type;
    SaveConfig();
}

// ===== Chip Switching =====

void SwitchToChipType(ChipType type) {
    if (g_device.activeChipType == type) return;

    if (g_device.connected) {
        // Full mute before switching away from current chip
        if (g_device.activeChipType == CHIP_YM2163) {
            YM2163::stop_all_notes();
            YM2163::ResetAllYM2163Chips();
        }
        if (g_device.activeChipType == CHIP_SN76489) {
            SN76489Window::MuteAll();
        }
        if (g_device.activeChipType == CHIP_YM2413) {
            YM2413Window::MuteAll();
        }
        FlushSN76489();
        Sleep(100);
    }

    SetAllSlots(type);

    if (g_device.connected) {
        SendReset();
        Sleep(200);

        if (type == CHIP_YM2163) {
            YM2163::g_hardwareConnected = true;
            YM2163::g_manualDisconnect = false;
            YM2163::ym2163_init();
        }
        // SN76489 auto-picks via SyncConnectionState in SN76489Window::Update()
        // CHIP_NONE: reset sent above, YM2163/SN76489 modules will sync on next Update
    }
}

// ===== State Query =====

bool IsConnected() {
    return g_device.connected;
}

ChipType GetActiveChipType() {
    return g_device.activeChipType;
}

// ===== Config =====

static const char* ChipTypeToString(ChipType t) {
    switch (t) {
        case CHIP_YM2163:  return "YM2163";
        case CHIP_SN76489: return "SN76489";
        case CHIP_YM2413:  return "YM2413";
        default:           return "NONE";
    }
}

static ChipType StringToChipType(const char* s) {
    if (strcmp(s, "YM2163") == 0)  return CHIP_YM2163;
    if (strcmp(s, "SN76489") == 0) return CHIP_SN76489;
    if (strcmp(s, "YM2413") == 0)  return CHIP_YM2413;
    return CHIP_NONE;
}

void LoadConfig() {
    char buf[64];
    GetPrivateProfileStringA("Device", "ChipType", "YM2163", buf, sizeof(buf), g_configPath);
    ChipType type = StringToChipType(buf);

    int autoConnect = GetPrivateProfileIntA("Device", "AutoConnect", 1, g_configPath);

    g_device.activeChipType = type;
    SetAllSlots(type);

    (void)autoConnect;
}

void SaveConfig() {
    WritePrivateProfileStringA("Device", "ChipType",
        ChipTypeToString(g_device.activeChipType), g_configPath);
    WritePrivateProfileStringA("Device", "AutoConnect", "1", g_configPath);
}

} // namespace SPFMManager
