// vgm_sync.cpp - Shared VGM playback state and slot mapping

#include "vgm_sync.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <zlib.h>
#include <windows.h>

namespace VGMSync {

// UTF-8 fopen for non-ASCII paths (Windows)
static FILE* Utf8Fopen(const char* path, const char* mode) {
    int wLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    int mLen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (wLen <= 0 || mLen <= 0) return NULL;
    wchar_t wPath[1024], wMode[16];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, wLen);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wMode, mLen);
    return _wfopen(wPath, wMode);
}

static char s_sharedPath[1024] = "";
static bool s_playing = false;
static bool s_paused = false;
static int  s_slotChip[MAX_SLOTS] = { CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE };

void Init() {
    s_sharedPath[0] = '\0';
    s_playing = false;
    s_paused = false;
    for (int i = 0; i < MAX_SLOTS; i++) s_slotChip[i] = CHIP_NONE;
}

void NotifyFileOpened(const char* path) {
    snprintf(s_sharedPath, sizeof(s_sharedPath), "%s", path);
}

void NotifyPlay() {
    s_playing = true;
    s_paused = false;
}

void NotifyStop() {
    s_playing = false;
    s_paused = false;
}

void NotifyPause() {
    if (s_playing) s_paused = !s_paused;
}

const char* GetSharedFilePath() {
    return s_sharedPath;
}

bool IsPlaying() {
    return s_playing && !s_paused;
}

bool IsPaused() {
    return s_playing && s_paused;
}

int GetSlotChip(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return CHIP_NONE;
    return s_slotChip[slot];
}

void SetSlotChip(int slot, int chipType) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    if (chipType != CHIP_NONE) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (s_slotChip[i] == chipType) s_slotChip[i] = CHIP_NONE;
        }
    }
    s_slotChip[slot] = chipType;
}

int FindChipSlot(int chipType) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (s_slotChip[i] == chipType) return i;
    }
    return -1;
}

// ===== VGM Header Chip Detection =====
// Data copied from libvgm-modizer/player/vgmplayer.cpp:
//   _DEV_LIST (line 54-62): VGM chip index → DEVID
//   _CHIPCLK_OFS (line 64-72): VGM header offset for each chip's clock
//   DEVID values from libvgm-modizer/emu/SoundDevs.h
//   _CHIP_COUNT = 42 (0x2a) from vgmplayer.hpp:292

// DEVID constants (from libvgm-modizer/emu/SoundDevs.h)
#define DEVID_SN76496   0x00
#define DEVID_YM2413    0x01
#define DEVID_YM2612    0x02
#define DEVID_YM2151    0x03
#define DEVID_SEGAPCM   0x04
#define DEVID_RF5C68    0x05
#define DEVID_YM2203    0x06
#define DEVID_YM2608    0x07
#define DEVID_YM2610    0x08
#define DEVID_YM3812    0x09
#define DEVID_YM3526    0x0A
#define DEVID_Y8950     0x0B
#define DEVID_YMF262    0x0C
#define DEVID_YMF278B   0x0D
#define DEVID_YMF271    0x0E
#define DEVID_YMZ280B   0x0F
#define DEVID_32X_PWM   0x11
#define DEVID_AY8910    0x12
#define DEVID_GB_DMG    0x13
#define DEVID_NES_APU   0x14
#define DEVID_YMW258    0x15
#define DEVID_uPD7759   0x16
#define DEVID_OKIM6258  0x17
#define DEVID_OKIM6295  0x18
#define DEVID_K051649   0x19
#define DEVID_K054539   0x1A
#define DEVID_C6280     0x1B
#define DEVID_C140      0x1C
#define DEVID_K053260   0x1D
#define DEVID_POKEY     0x1E
#define DEVID_QSOUND    0x1F
#define DEVID_SCSP      0x20
#define DEVID_WSWAN     0x21
#define DEVID_VBOY_VSU  0x22
#define DEVID_SAA1099   0x23
#define DEVID_ES5503    0x24
#define DEVID_ES5506    0x25
#define DEVID_X1_010    0x26
#define DEVID_C352      0x27
#define DEVID_GA20      0x28
#define DEVID_MIKEY     0x29

static const int CHIP_COUNT = 42;

// VGM chip index → DEVID (from vgmplayer.cpp:54-62 _DEV_LIST)
static const uint8_t DEV_LIST[CHIP_COUNT] = {
    DEVID_SN76496, DEVID_YM2413,  DEVID_YM2612,  DEVID_YM2151,  DEVID_SEGAPCM,  DEVID_RF5C68,  DEVID_YM2203,  DEVID_YM2608,
    DEVID_YM2610,  DEVID_YM3812,  DEVID_YM3526,  DEVID_Y8950,   DEVID_YMF262,  DEVID_YMF278B, DEVID_YMF271,  DEVID_YMZ280B,
    DEVID_RF5C68,  DEVID_32X_PWM, DEVID_AY8910,  DEVID_GB_DMG,  DEVID_NES_APU, DEVID_YMW258,  DEVID_uPD7759, DEVID_OKIM6258,
    DEVID_OKIM6295, DEVID_K051649, DEVID_K054539, DEVID_C6280,   DEVID_C140,    DEVID_K053260, DEVID_POKEY,   DEVID_QSOUND,
    DEVID_SCSP,    DEVID_WSWAN,   DEVID_VBOY_VSU, DEVID_SAA1099, DEVID_ES5503,  DEVID_ES5506,  DEVID_X1_010,  DEVID_C352,
    DEVID_GA20,    DEVID_MIKEY,
};

// VGM header clock offsets (from vgmplayer.cpp:64-72 _CHIPCLK_OFS)
static const uint32_t CHIPCLK_OFS[CHIP_COUNT] = {
    0x0C, 0x10, 0x2C, 0x30, 0x38, 0x40, 0x44, 0x48,
    0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68,
    0x6C, 0x70, 0x74, 0x80, 0x84, 0x88, 0x8C, 0x90,
    0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4,
    0xB8, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD8, 0xDC,
    0xE0, 0xE4,
};

static uint32_t ReadMemLE32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static bool DecompressGzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    z_stream strm = {};
    strm.avail_in = (uInt)size;
    strm.next_in = (Bytef*)data;
    if (inflateInit2(&strm, 15 + 32) != Z_OK) return false;
    uint8_t chunk[16384];
    int ret;
    do {
        strm.avail_out = sizeof(chunk);
        strm.next_out = chunk;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm); return false;
        }
        size_t have = sizeof(chunk) - strm.avail_out;
        out.insert(out.end(), chunk, chunk + have);
    } while (ret != Z_STREAM_END);
    inflateEnd(&strm);
    return !out.empty();
}

// Map DEVID to our ChipType enum (only chips we support with hardware)
static ChipType DevIdToChipType(uint8_t devid) {
    switch (devid) {
        case DEVID_SN76496: return CHIP_SN76489;
        case DEVID_YM2413:  return CHIP_YM2413;
        case DEVID_AY8910:  return CHIP_AY8910;
        default:            return CHIP_NONE;
    }
}

void AutoAssignSlots(const char* vgmPath) {
    if (!vgmPath || !vgmPath[0]) return;

    // Open and read VGM header
    FILE* f = Utf8Fopen(vgmPath, "rb");
    if (!f) return;

    uint8_t magic[2];
    if (fread(magic, 1, 2, f) != 2) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    const uint8_t* hdr;
    size_t hdrSize;
    std::vector<uint8_t> decBuf;

    if (magic[0] == 0x1F && magic[1] == 0x8B) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> compressed(fsize);
        if ((long)fread(compressed.data(), 1, fsize, f) != fsize) { fclose(f); return; }
        fclose(f);
        if (!DecompressGzip(compressed.data(), compressed.size(), decBuf)) return;
        if (decBuf.size() < 0x40) return;
        if (memcmp(decBuf.data(), "Vgm ", 4) != 0) return;
        hdr = decBuf.data();
        hdrSize = decBuf.size();
    } else {
        static const size_t HDR_READ = 0xE8;
        static uint8_t hdrBuf[HDR_READ];
        hdrSize = fread(hdrBuf, 1, HDR_READ, f);
        fclose(f);
        if (hdrSize < 0x40) return;
        if (memcmp(hdrBuf, "Vgm ", 4) != 0) return;
        hdr = hdrBuf;
    }

    // Scan all 42 chip clock fields (from libvgm _CHIPCLK_OFS + _DEV_LIST)
    int newSlots[MAX_SLOTS] = { CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE };
    bool seenChip[4] = {};
    int slotIdx = 0;
    for (int i = 0; i < CHIP_COUNT && slotIdx < MAX_SLOTS; i++) {
        uint32_t off = CHIPCLK_OFS[i];
        if (off + 4 > hdrSize) continue;
        uint32_t clock = ReadMemLE32(hdr + off);
        if (clock == 0) continue;
        ChipType ct = DevIdToChipType(DEV_LIST[i]);
        if (ct == CHIP_NONE) continue;
        if (seenChip[ct]) continue;
        seenChip[ct] = true;
        newSlots[slotIdx++] = ct;
    }

    // Only update if we actually detected something
    if (slotIdx > 0) {
        for (int i = 0; i < MAX_SLOTS; i++) s_slotChip[i] = newSlots[i];
    }
}

void PreviewAssignSlots(const char* firstVgmPath) {
    if (!firstVgmPath || !firstVgmPath[0]) return;
    if (s_sharedPath[0] != '\0') return;
    AutoAssignSlots(firstVgmPath);
}

} // namespace VGMSync
