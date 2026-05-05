// vgm_sync.h - Shared VGM playback state and slot mapping across chip windows

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

// ===== Playback Sync =====
void NotifyFileOpened(const char* path);
void NotifyPlay();
void NotifyStop();
void NotifyPause();

const char* GetSharedFilePath();
bool IsPlaying();
bool IsPaused();

// ===== Slot Mapping (shared across all windows) =====
// slot: 0-3, returns CHIP_NONE/CHIP_YM2413/CHIP_AY8910/CHIP_SN76489
int  GetSlotChip(int slot);
void SetSlotChip(int slot, int chipType);

// Find which slot a chip is assigned to, returns -1 if not assigned
int FindChipSlot(int chipType);

// ===== Auto Slot Assignment =====
// Read VGM header clock fields, auto-assign detected chips to slots
// Call this from LoadVGMFile() after successful load
void AutoAssignSlots(const char* vgmPath);

// Preview: read first VGM in playlist, assign slots (for folder navigation feedback)
void PreviewAssignSlots(const char* firstVgmPath);

} // namespace VGMSync

#endif // VGM_SYNC_H
