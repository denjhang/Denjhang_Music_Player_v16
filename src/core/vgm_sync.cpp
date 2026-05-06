// vgm_sync.cpp - Shared VGM playback state and slot mapping

#include "vgm_sync.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <string>
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

static const char* ChipTypeName(int ct) {
    switch (ct) {
        case CHIP_YM2413:  return "YM2413";
        case CHIP_AY8910:  return "AY8910";
        case CHIP_SN76489: return "SN76489";
        default:           return "none";
    }
}

static int StringToChipType(const char* s) {
    if (strcmp(s, "YM2413") == 0)  return CHIP_YM2413;
    if (strcmp(s, "AY8910") == 0)  return CHIP_AY8910;
    if (strcmp(s, "SN76489") == 0) return CHIP_SN76489;
    return CHIP_NONE;
}

// ===== Slot Preset Persistence =====
static char s_presetPath[MAX_PATH] = "";
static char s_lastComboKey[128] = "";

static void InitPresetPath(void) {
    if (s_presetPath[0]) return;
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(wbuf, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    char buf[MAX_PATH];
    wcstombs(buf, wbuf, MAX_PATH);
    snprintf(s_presetPath, MAX_PATH, "%s\\slot_presets.ini", buf);
}

static std::string MakeChipComboKey(const int* slots) {
    std::vector<std::string> names;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i] != CHIP_NONE) names.push_back(ChipTypeName(slots[i]));
    }
    // Sort to normalize: YM2413+AY8910 == AY8910+YM2413
    std::sort(names.begin(), names.end());
    std::string key;
    for (size_t i = 0; i < names.size(); i++) {
        if (i > 0) key += "+";
        key += names[i];
    }
    return key.empty() ? "none" : key;
}

static bool LoadSlotPresetInternal(const char* comboKey) {
    InitPresetPath();
    char buf[16];
    GetPrivateProfileStringA(comboKey, "Slot0", "", buf, sizeof(buf), s_presetPath);
    if (buf[0] == '\0') return false;
    for (int i = 0; i < MAX_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "Slot%d", i);
        GetPrivateProfileStringA(comboKey, key, "none", buf, sizeof(buf), s_presetPath);
        s_slotChip[i] = StringToChipType(buf);
    }
    return true;
}

void SaveSlotPreset(const char* comboKey) {
    if (!comboKey || !comboKey[0]) return;
    InitPresetPath();
    for (int i = 0; i < MAX_SLOTS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "Slot%d", i);
        WritePrivateProfileStringA(comboKey, key, ChipTypeName(s_slotChip[i]), s_presetPath);
    }
}

const char* GetLastComboKey() {
    return s_lastComboKey;
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

    // Scan all 42 chip clock fields
    // Dual chip: clock bit 30 (0x40000000) means 2 instances (libvgm GetChipCount)
    int newSlots[MAX_SLOTS] = { CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE };
    int slotIdx = 0;
    for (int i = 0; i < CHIP_COUNT && slotIdx < MAX_SLOTS; i++) {
        uint32_t off = CHIPCLK_OFS[i];
        if (off + 4 > hdrSize) continue;
        uint32_t clock = ReadMemLE32(hdr + off);
        if (clock == 0) continue;
        ChipType ct = DevIdToChipType(DEV_LIST[i]);
        if (ct == CHIP_NONE) continue;
        int count = (clock & 0x40000000) ? 2 : 1;
        for (int j = 0; j < count && slotIdx < MAX_SLOTS; j++) {
            newSlots[slotIdx++] = ct;
        }
    }

    if (slotIdx == 0) return;

    // Generate combo key and check for saved preset
    std::string comboKey = MakeChipComboKey(newSlots);
    snprintf(s_lastComboKey, sizeof(s_lastComboKey), "%s", comboKey.c_str());

    if (LoadSlotPresetInternal(comboKey.c_str())) return;  // Use saved config

    // No saved config: auto-assign and save
    for (int i = 0; i < MAX_SLOTS; i++) s_slotChip[i] = newSlots[i];
    SaveSlotPreset(comboKey.c_str());
}

void PreviewAssignSlots(const char* firstVgmPath) {
    if (!firstVgmPath || !firstVgmPath[0]) return;
    if (s_sharedPath[0] != '\0') return;
    AutoAssignSlots(firstVgmPath);
}

// ============================================================
// Unified VGM Playback Engine
// ============================================================

// --- Callback storage ---
struct ChipWriter {
    ChipStateUpdateFn stateFn;
    ChipHwWriteFn     hwFn;
    ChipFlushFn       flushFn;
    ChipWriter() : stateFn(nullptr), hwFn(nullptr), flushFn(nullptr) {}
};
static ChipWriter s_writers[4]; // indexed by ChipType
static SnCmdFn    s_snCmdHandler = nullptr;

void RegisterChipWriter(int chipType, ChipStateUpdateFn stateFn, ChipHwWriteFn hwFn, ChipFlushFn flushFn) {
    if (chipType < 0 || chipType >= 4) return;
    s_writers[chipType].stateFn = stateFn;
    s_writers[chipType].hwFn = hwFn;
    s_writers[chipType].flushFn = flushFn;
}

void RegisterSnHandler(SnCmdFn cmdFn) {
    s_snCmdHandler = cmdFn;
}

// --- Shared progress ---
static uint32_t s_currentSamples = 0;
static uint32_t s_totalSamples = 0;

void    SetCurrentSamples(uint32_t s) { s_currentSamples = s; }
uint32_t GetCurrentSamples()          { return s_currentSamples; }
uint32_t GetTotalSamples()            { return s_totalSamples; }
void    SetTotalSamples(uint32_t t)   { s_totalSamples = t; }

// --- Unified playback state ---
static bool     s_uPlaying = false;
static bool     s_uPaused = false;
static bool     s_uThreadRunning = false;
static HANDLE   s_uThread = nullptr;
static int      s_timerMode = 0;
static float    s_fadeoutDuration = 0;
static int      s_maxLoops = 0;

void SetTimerMode(int mode) { s_timerMode = mode; }
int  GetTimerMode()         { return s_timerMode; }
void SetFadeout(float d)    { s_fadeoutDuration = d; }
void SetMaxLoops(int l)     { s_maxLoops = l; }

bool IsUnifiedPlaying() { return s_uPlaying && !s_uPaused; }
bool IsUnifiedPaused()  { return s_uPlaying && s_uPaused; }

// --- VGM file data ---
static std::vector<uint8_t> s_uMemData;
static size_t               s_uMemPos = 0;
static FILE*                s_uFile = nullptr;
static uint32_t             s_uDataOffset = 0;
static uint32_t             s_uLoopOffset = 0;
static uint32_t             s_uLoopSamples = 0;
static uint32_t             s_uLoopCount = 0;
static char                 s_uPath[1024] = "";
static uint32_t             s_uVersion = 0;
static bool                 s_uTrackEnded = false;

// Unified file I/O wrappers
static uint8_t uReadByte(void) {
    if (!s_uMemData.empty()) {
        if (s_uMemPos >= s_uMemData.size()) return 0xFF;
        return s_uMemData[s_uMemPos++];
    }
    uint8_t b;
    if (fread(&b, 1, 1, s_uFile) != 1) return 0xFF;
    return b;
}

static bool uRead(void* buf, size_t len) {
    if (!s_uMemData.empty()) {
        if (s_uMemPos + len > s_uMemData.size()) return false;
        memcpy(buf, s_uMemData.data() + s_uMemPos, len);
        s_uMemPos += len;
        return true;
    }
    return fread(buf, 1, len, s_uFile) == len;
}

static bool uSeek(long offset, int origin) {
    if (!s_uMemData.empty()) {
        if (origin == SEEK_SET) s_uMemPos = offset;
        else if (origin == SEEK_CUR) s_uMemPos += offset;
        return true;
    }
    return fseek(s_uFile, offset, origin) == 0;
}

static long uTell(void) {
    if (!s_uMemData.empty()) return (long)s_uMemPos;
    return ftell(s_uFile);
}

// VGM Command Length Table (same as in all three windows)
static const uint8_t U_CMD_LEN[0x100] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    3,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,2,  2,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    0,3,1,1,0,0,0,0, 0x0C,0,0,0,0,0,0,0,  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,  5,5,6,0x0B,2,5,0,0, 0,0,0,0,0,0,0,0,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,  3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    5,5,5,4,4,4,4,4, 4,4,4,4,4,4,4,4,  4,4,4,5,5,5,5,4, 4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,  5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,
};

// Fadeout state
static bool   s_fadeActive = false;
static float  s_fadeLevel = 1.0f;
static uint32_t s_fadeStartSample = 0;
static uint32_t s_fadeEndSample = 0;

// Unified VGM command processor
static int UProcessCommand(void) {
    uint8_t cmd = uReadByte();

    switch (cmd) {
        case 0x51: { // YM2413
            uint8_t reg = uReadByte(), data = uReadByte();
            if (s_writers[CHIP_YM2413].stateFn)
                data = s_writers[CHIP_YM2413].stateFn(reg, data);
            if (s_writers[CHIP_YM2413].hwFn)
                s_writers[CHIP_YM2413].hwFn(reg, data);
            return 0;
        }
        case 0xA0: { // AY8910
            uint8_t reg = uReadByte(), data = uReadByte();
            if (s_writers[CHIP_AY8910].stateFn)
                data = s_writers[CHIP_AY8910].stateFn(reg, data);
            if (s_writers[CHIP_AY8910].hwFn)
                s_writers[CHIP_AY8910].hwFn(reg, data);
            return 0;
        }
        case 0x50: { // SN76489
            uint8_t data = uReadByte();
            if (s_snCmdHandler) s_snCmdHandler(0x50, data);
            return 0;
        }
        case 0x30: { // SN76489 2nd / T6W28
            uint8_t data = uReadByte();
            if (s_snCmdHandler) s_snCmdHandler(0x30, data);
            return 0;
        }
        case 0x61: {
            uint16_t wait;
            if (!uRead(&wait, 2)) return -1;
            return wait;
        }
        case 0x62: return 735;
        case 0x63: return 882;
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return (cmd & 0x0F) + 1;
        case 0x66: {
            if (s_uLoopOffset > 0 && (s_maxLoops == 0 || s_uLoopCount < s_maxLoops)) {
                // Fadeout on penultimate loop
                if (s_maxLoops > 0 && s_uLoopCount >= s_maxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeActive) {
                    s_fadeActive = true;
                    s_fadeLevel = 1.0f;
                    s_fadeStartSample = s_currentSamples;
                    uint32_t fadeSamples = (uint32_t)(s_fadeoutDuration * 44100.0);
                    if (s_uLoopSamples > 0 && fadeSamples > s_uLoopSamples)
                        fadeSamples = s_uLoopSamples;
                    s_fadeEndSample = s_currentSamples + fadeSamples;
                }
                uSeek(s_uLoopOffset, SEEK_SET);
                s_uLoopCount++;
                return 0;
            }
            return -1;
        }
        case 0x67: {
            uint8_t compat; if (!uRead(&compat, 1)) return -1;
            if (compat != 0x66) return -1;
            uint8_t type; if (!uRead(&type, 1)) return -1;
            uint32_t size; if (!uRead(&size, 4)) return -1;
            uSeek(size, SEEK_CUR);
            return 0;
        }
    }

    // Skip unknown commands
    uint8_t cmdLen = U_CMD_LEN[cmd];
    if (cmdLen <= 1) return 0;
    uSeek(cmdLen - 1, SEEK_CUR);
    return 0;
}

static void FlushAllWriters(void) {
    for (int i = 1; i <= 3; i++) {
        if (s_writers[i].flushFn) s_writers[i].flushFn();
    }
}

static DWORD WINAPI UnifiedVGMThread(LPVOID) {
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double samplesPerTick = 44100.0 / freq.QuadPart;
    double samplesToProcess = 0.0;

    while (s_uThreadRunning && s_uPlaying) {
        if (s_uPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        samplesToProcess += (now.QuadPart - last.QuadPart) * samplesPerTick;
        last = now;

        int run = (int)samplesToProcess;
        if (run > 44100) run = 44100;  // cap to 1 second max per frame
        if (run > 0) {
            int processed = 0;
            while (processed < run && s_uThreadRunning && s_uPlaying && !s_uPaused) {
                int s = UProcessCommand();
                if (s < 0) {
                    s_uTrackEnded = true;
                    s_uPlaying = false;
                    break;
                }
                if (s > 0) processed += s;
            }
            samplesToProcess -= processed;
            s_currentSamples += processed;

            // Fadeout
            if (s_fadeActive && s_fadeoutDuration > 0) {
                uint32_t fadeRange = s_fadeEndSample - s_fadeStartSample;
                if (fadeRange == 0) fadeRange = 1;
                float progress = (float)(s_currentSamples - s_fadeStartSample) / (float)fadeRange;
                if (s_currentSamples >= s_fadeEndSample) {
                    s_fadeLevel = 0.0f;
                } else {
                    s_fadeLevel = 1.0f - progress;
                }
                if (s_fadeLevel <= 0.0f) {
                    s_uTrackEnded = true;
                    s_uPlaying = false;
                }
            }
            FlushAllWriters();
        }
        Sleep(1);
    }
    FlushAllWriters();
    s_uThreadRunning = false;
    return 0;
}

static uint32_t UReadLE32(void) {
    uint8_t b[4];
    uRead(b, 4);
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

bool StartUnifiedPlayback(const char* vgmPath, int timerMode) {
    if (!vgmPath || !vgmPath[0]) return false;

    // Stop any existing playback
    StopUnifiedPlayback();

    // Open VGM file
    FILE* f = Utf8Fopen(vgmPath, "rb");
    if (!f) return false;

    uint8_t magic[2];
    if (fread(magic, 1, 2, f) != 2) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    s_uMemData.clear();
    s_uMemPos = 0;

    if (magic[0] == 0x1F && magic[1] == 0x8B) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> compressed(fsize);
        if ((long)fread(compressed.data(), 1, fsize, f) != fsize) { fclose(f); return false; }
        fclose(f);
        if (!DecompressGzip(compressed.data(), compressed.size(), s_uMemData)) return false;
        if (s_uMemData.size() < 0x40) return false;
        if (memcmp(s_uMemData.data(), "Vgm ", 4) != 0) return false;
        s_uFile = nullptr;
    } else {
        s_uFile = f; // keep open for streaming
    }

    // Parse header
    uSeek(0x08, SEEK_SET);
    s_uVersion = UReadLE32();

    uSeek(0x14, SEEK_SET);
    uint32_t gd3RelOff = UReadLE32();
    s_totalSamples = UReadLE32();
    uint32_t loopRelOff = UReadLE32();
    s_uLoopOffset = loopRelOff ? (loopRelOff + 0x1C) : 0;
    s_uLoopSamples = UReadLE32();

    uint32_t dataOff = 0x40;
    if (s_uVersion >= 0x150) {
        uSeek(0x34, SEEK_SET);
        uint32_t hdrDataOff = UReadLE32();
        if (hdrDataOff > 0) dataOff = hdrDataOff + 0x34;
    }
    s_uDataOffset = dataOff;

    // Seek to data start
    uSeek(s_uDataOffset, SEEK_SET);

    snprintf(s_uPath, sizeof(s_uPath), "%s", vgmPath);
    s_timerMode = timerMode;
    s_currentSamples = 0;
    s_uLoopCount = 0;
    s_uPaused = false;
    s_uTrackEnded = false;
    s_fadeActive = false;
    s_fadeLevel = 1.0f;
    s_uPlaying = true;
    s_uThreadRunning = true;
    s_uThread = CreateThread(NULL, 0, UnifiedVGMThread, NULL, 0, NULL);

    NotifyPlay();
    return true;
}

void StopUnifiedPlayback(void) {
    s_uPlaying = false;
    s_uPaused = false;
    s_uThreadRunning = false;
    s_fadeActive = false;
    s_fadeLevel = 1.0f;
    if (s_uThread) {
        WaitForSingleObject(s_uThread, 2000);
        CloseHandle(s_uThread);
        s_uThread = nullptr;
    }
    if (s_uFile) { fclose(s_uFile); s_uFile = nullptr; }
    s_uMemData.clear();
    s_uMemData.shrink_to_fit();
    NotifyStop();
}

void PauseUnifiedPlayback(void) {
    if (!s_uPlaying) return;
    s_uPaused = !s_uPaused;
    NotifyPause();
}

} // namespace VGMSync
