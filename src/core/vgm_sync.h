// vgm_sync.h - Shared VGM playback state, slot mapping, and unified playback

#ifndef VGM_SYNC_H
#define VGM_SYNC_H

#include <stdint.h>

namespace VGMSync {

enum ChipType {
    CHIP_NONE    = 0,
    CHIP_YM2413  = 1,
    CHIP_AY8910  = 2,
    CHIP_SN76489 = 3,
};

static const int MAX_SLOTS = 4;

// ===== Lifecycle =====
void Init();

// ===== Playback Sync (broadcast notifications) =====
void NotifyFileOpened(const char* path);
void NotifyPlay();
void NotifyStop();
void NotifyPause();

const char* GetSharedFilePath();
bool IsPlaying();
bool IsPaused();

// ===== Shared Playback Progress =====
void    SetCurrentSamples(uint32_t samples);
uint32_t GetCurrentSamples();
uint32_t GetTotalSamples();
void    SetTotalSamples(uint32_t total);

// ===== Slot Mapping (shared across all windows) =====
int  GetSlotChip(int slot);
void SetSlotChip(int slot, int chipType);
int  FindChipSlot(int chipType);

// ===== Auto Slot Assignment =====
void AutoAssignSlots(const char* vgmPath);
void PreviewAssignSlots(const char* firstVgmPath);

// ===== Slot Preset Persistence =====
void SaveSlotPreset(const char* comboKey);
const char* GetLastComboKey();

// ===== Unified VGM Playback =====
// Callback types for chip-specific register writes
typedef uint8_t (*ChipStateUpdateFn)(uint8_t reg, uint8_t data);  // update shadow state, return data
typedef void    (*ChipHwWriteFn)(uint8_t reg, uint8_t data);      // hardware write
typedef void    (*ChipFlushFn)(void);                              // optional flush
typedef void    (*SnCmdFn)(uint8_t cmd, uint8_t data);            // SN76489 command handler

// Register chip handlers (call from each window's Init())
void RegisterChipWriter(int chipType, ChipStateUpdateFn stateFn, ChipHwWriteFn hwFn, ChipFlushFn flushFn);
void RegisterSnHandler(SnCmdFn cmdFn);

// Start/stop the unified playback thread
bool StartUnifiedPlayback(const char* vgmPath, int timerMode);
void StopUnifiedPlayback();
void PauseUnifiedPlayback();

// Query unified playback state
bool IsUnifiedPlaying();
bool IsUnifiedPaused();

// Timer mode (copied from originating window)
void SetTimerMode(int mode);
int  GetTimerMode();

// Fadeout
void SetFadeout(float duration);
void SetMaxLoops(int loops);

} // namespace VGMSync

#endif // VGM_SYNC_H
