// gigatron_window.cpp - Gigatron Tracker Tab for Denjhang's Music Player v16
// Layout mirrors VGM window: LeftPane(controls) | PianoArea + LevelMeter/Scope + StatusArea | FileBrowser + Log

#include "gigatron_window.h"
#include "gigatron/gigatron_emu.h"
#include "gigatron/winmm.h"
#include "gigatron/audio_output.h"
#include "gigatron/fnum_table.h"
#include "midi_player.h"  // for UTF8ToWide, SplitPath

#include "imgui/imgui.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace GigatronWindow {

// ============ Constants ============
static const int GT_SAMPLE_RATE = 44100;
static const int GT_BUFFER_SAMPLES = 1024;
static const int GT_BUFFER_SIZE = GT_BUFFER_SAMPLES * 2 * sizeof(int16_t);
static const int GT_FRAME_RATE_INT = 5998;

// Channel colors (4 Gigatron channels)
static ImU32 kChColors[4] = {
    IM_COL32(80, 220, 80, 255),   // Ch0: green
    IM_COL32(80, 140, 255, 255),  // Ch1: blue
    IM_COL32(255, 80, 80, 255),   // Ch2: red
    IM_COL32(255, 180, 60, 255)   // Ch3: orange
};
static const char* kChNames[4] = { "Ch0", "Ch1", "Ch2", "Ch3" };

// ============ Data structures (from Tracker main.cpp) ============
typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} DynamicByteArray;

typedef struct {
    size_t* data;
    size_t size;
    size_t capacity;
} DynamicSizeTArray;

typedef struct {
    DynamicByteArray all_music_bytes;
    DynamicSizeTArray segment_start_indices;
    unsigned int actual_segment_count;
    unsigned long total_delay_frames;
} ParsedMusicData;

typedef struct {
    unsigned long frame;
    uint16_t reg;
    uint8_t val;
} FrameEvent;

typedef struct {
    FrameEvent* events;
    size_t size;
    size_t capacity;
    size_t play_pos;
} EventTimeline;

// ============ Module state ============
static GigatronState s_gtState;
static int16_t s_audioBuffer[GT_BUFFER_SAMPLES * 2];
static const audio_output_t* s_audioOutput = &audio_output_winmm;

static char s_musicDir[MAX_PATH] = "";
static std::vector<MidiPlayer::FileEntry> s_fileList;
static int s_selectedFileIdx = -1;
static std::string s_selectedFilePath;

static bool s_musicPlaying = false;
static bool s_musicPaused = false;
static unsigned long s_currentFrame = 0;
static unsigned long s_totalEstimatedFrames = 0;
static bool s_playerBarOpen = true;
static unsigned int s_targetSegmentCount = 0;
static float s_playbackSpeed = 1.0f;

static EventTimeline s_timeline;
static ParsedMusicData s_parsedMusicData;

static LARGE_INTEGER s_perfFrequency;
static LARGE_INTEGER s_lastPerfCounter;
static double s_samplesDue = 0.0;

static RATIO_CNTR s_musicEventRC;
static bool s_rcRatioNeedsUpdate = false;

static int s_audioBitDepth = 4;
static bool s_dcOffsetRemoval = false;
static float s_volumeScale = 0.5f;
static bool s_skipInitialSilence = false;

static bool s_showScope = false;
static float s_scopeHeight = 200.0f;

static char s_configPath[MAX_PATH] = "";

static std::string s_log;
static char s_logDisplay[65536] = "";
static bool s_logAutoScroll = true;
static bool s_logScrollToBottom = false;
static size_t s_logLastSize = 0;

static std::vector<std::string> s_folderHistory;
static std::vector<std::string> s_navHistory;
static int s_navPos = -1;
static bool s_navigating = false;

static bool s_pathEditMode = false;
static char s_pathInput[MAX_PATH] = "";
static bool s_pathEditModeJustActivated = false;

static int s_histSortMode = 0;
static char s_histFilter[128] = "";
static bool s_historyCollapsed = false;

// Scope per-channel settings
static float s_scopeWidth[4]  = {220.0f, 220.0f, 220.0f, 220.0f};
static float s_scopeAmp[4]   = {1.0f, 1.0f, 1.0f, 1.0f};

// Scope global settings (shared across all 4 channels)
static int s_scopeSamples = 512;
static int s_scopeSearchWindow = 256;
static int s_scopeOffset = 0;
static bool s_scopeEdgeAlign = true;
static int s_scopeAcMode = 1;   // 0=off, 1=center, 2=bottom (shared)
static bool s_showScopeSettingsWindow = false;

// Scope correlation state (per-channel persistent offset + previous frame buffer)
static int s_scopePersistOffset[4] = {0, 0, 0, 0};
static int16_t s_scopePrevBuf[4][4096];

// Level meter peak decay state
static float s_levelMeter[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static float s_levelPeak[4]  = {0.0f, 0.0f, 0.0f, 0.0f};

// Wave editor state
static bool s_showWaveEditorWindow = false;
static bool s_useCustomWaveTable = false;
static int s_waveTableBits = 6;
static int s_waveEditType = 0; // 0=Noise, 1=Triangle, 2=Pulse, 3=Sawtooth
static uint16_t s_waveEditBuf[64]; // working copy for drawing (64 points per wave type)
static bool s_waveEditBufDirty = false;
static bool s_waveDrawing = false;

// ============ Forward declarations ============
static void LoadConfig();
static void SaveConfig();
static void InitParsedMusicData(ParsedMusicData* d);
static void FreeParsedMusicData(ParsedMusicData* d);
static void InitEventTimeline(EventTimeline* tl);
static void FreeEventTimeline(EventTimeline* tl);
static void TlAdd(EventTimeline* tl, unsigned long frame, uint16_t reg, uint8_t val);
static char* ReadFileContent(const char* filename);
static ParsedMusicData ParseGbasCFile(const char* content, unsigned int maxSegs);
static std::vector<std::string> GetMusicFilesInDir(const std::string& path);
static void BuildEventTimeline(const ParsedMusicData* md, EventTimeline* tl, bool skipInitialSilence);
static void AdvanceTimeline(GigatronState* st, EventTimeline* tl, unsigned long frame);
static void PlaySelected();
static void PlayFileByIndex(int idx);
static void StopPlayback();
static void SeekToFrame(unsigned long targetFrame);
static void RefreshFileList();
static void NavigateTo(const char* path);
static void NavBack();
static void NavForward();
static void NavToParent();
static void AddToFolderHistory(const char* path);
static void RenderControls();
static void RenderPianoArea();
static void RenderLevelMeterArea();
static void RenderScopeArea();
static void RenderStatusArea();
static void RenderFileBrowser();
static void RenderLogPanel();
static void RenderScopeSettingsWindow();
static void RenderWaveEditorWindow();
static void GtLog(const char* fmt, ...);
static int ReverseLookupFnum(uint16_t fnum_div8);
static void GetExeDir(char* buf, int bufSize);

// ============ Helper: Get exe directory ============
static void GetExeDir(char* buf, int bufSize) {
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(wbuf, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    std::string s = MidiPlayer::WideToUTF8(std::wstring(wbuf));
    snprintf(buf, bufSize, "%s", s.c_str());
}

// ============ GtLog ============
static void GtLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_log += buf;
    // Keep log size reasonable
    if (s_log.size() > 256000) {
        s_log = s_log.substr(s_log.size() - 128000);
    }
}

// ============ Dynamic Array Helpers ============
static void InitDynamicByteArray(DynamicByteArray* arr) {
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

static void AddByteToArray(DynamicByteArray* arr, uint8_t byte) {
    if (arr->size >= arr->capacity) {
        size_t nc = (arr->capacity == 0) ? 16 : arr->capacity * 2;
        uint8_t* nd = (uint8_t*)realloc(arr->data, nc * sizeof(uint8_t));
        if (!nd) { GtLog("[GT] OOM in AddByteToArray\n"); return; }
        arr->data = nd;
        arr->capacity = nc;
    }
    arr->data[arr->size++] = byte;
}

static void FreeDynamicByteArray(DynamicByteArray* arr) {
    free(arr->data);
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

static void InitDynamicSizeTArray(DynamicSizeTArray* arr) {
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

static void AddSizeTToArray(DynamicSizeTArray* arr, size_t value) {
    if (arr->size >= arr->capacity) {
        size_t nc = (arr->capacity == 0) ? 16 : arr->capacity * 2;
        size_t* nd = (size_t*)realloc(arr->data, nc * sizeof(size_t));
        if (!nd) { GtLog("[GT] OOM in AddSizeTToArray\n"); return; }
        arr->data = nd;
        arr->capacity = nc;
    }
    arr->data[arr->size++] = value;
}

static void FreeDynamicSizeTArray(DynamicSizeTArray* arr) {
    free(arr->data);
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

static void InitParsedMusicData(ParsedMusicData* d) {
    InitDynamicByteArray(&d->all_music_bytes);
    InitDynamicSizeTArray(&d->segment_start_indices);
    d->actual_segment_count = 0;
    d->total_delay_frames = 0;
}

static void FreeParsedMusicData(ParsedMusicData* d) {
    FreeDynamicByteArray(&d->all_music_bytes);
    FreeDynamicSizeTArray(&d->segment_start_indices);
    d->actual_segment_count = 0;
    d->total_delay_frames = 0;
}

static void InitEventTimeline(EventTimeline* tl) {
    tl->events = NULL;
    tl->size = 0;
    tl->capacity = 0;
    tl->play_pos = 0;
}

static void FreeEventTimeline(EventTimeline* tl) {
    free(tl->events);
    tl->events = NULL;
    tl->size = 0;
    tl->capacity = 0;
    tl->play_pos = 0;
}

static void TlAdd(EventTimeline* tl, unsigned long frame, uint16_t reg, uint8_t val) {
    if (tl->size >= tl->capacity) {
        size_t nc = (tl->capacity == 0) ? 4096 : tl->capacity * 2;
        FrameEvent* nd = (FrameEvent*)realloc(tl->events, nc * sizeof(FrameEvent));
        if (!nd) { GtLog("[GT] OOM in TlAdd\n"); return; }
        tl->events = nd;
        tl->capacity = nc;
    }
    tl->events[tl->size++] = (FrameEvent){frame, reg, val};
}

// ============ ReadFileContent ============
static char* ReadFileContent(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        GtLog("[GT] Could not open file: %s\n", filename);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* content = (char*)malloc(length + 1);
    if (!content) {
        GtLog("[GT] Memory allocation failed for file content\n");
        fclose(file);
        return NULL;
    }
    fread(content, 1, length, file);
    content[length] = '\0';
    fclose(file);
    return content;
}

// ============ ParseGbasCFile ============
static ParsedMusicData ParseGbasCFile(const char* file_content, unsigned int max_segments) {
    ParsedMusicData result;
    InitParsedMusicData(&result);

    if (!file_content) return result;

    const char* current_pos = file_content;
    const char* start_marker = "nohop static const byte ";
    const char* array_start_marker = "[] = {";
    const char* array_end_marker = "};";

    while ((current_pos = strstr(current_pos, start_marker)) != NULL) {
        if (max_segments > 0 && result.actual_segment_count >= max_segments) {
            GtLog("[GT] Reached max_segments (%u). Stopping parsing.\n", max_segments);
            break;
        }
        AddSizeTToArray(&result.segment_start_indices, result.all_music_bytes.size);
        result.actual_segment_count++;

        current_pos += strlen(start_marker);

        const char* name_end = strstr(current_pos, array_start_marker);
        if (!name_end) {
            GtLog("[GT] Malformed .gbas.c file (missing '[] = {').\n");
            break;
        }
        const char* data_start = name_end + strlen(array_start_marker);
        const char* data_end = strstr(data_start, array_end_marker);
        if (!data_end) {
            GtLog("[GT] Malformed .gbas.c file (missing '};').\n");
            break;
        }

        char* seg_copy = (char*)malloc(data_end - data_start + 1);
        if (!seg_copy) { GtLog("[GT] OOM in ParseGbasCFile\n"); break; }
        strncpy(seg_copy, data_start, data_end - data_start);
        seg_copy[data_end - data_start] = '\0';

        // Trim leading whitespace
        size_t first_char = strspn(seg_copy, " \t\n\r");
        char* trimmed = seg_copy + first_char;
        // Trim trailing whitespace
        size_t last_char = strlen(trimmed);
        while (last_char > 0 && strchr(" \t\n\r", trimmed[last_char - 1]) != NULL) last_char--;
        trimmed[last_char] = '\0';

        char* ptr = trimmed;
        while (*ptr != '\0') {
            ptr += strspn(ptr, ", \t\n\r");
            if (*ptr == '\0') break;

            // Check for '0' byte indicating end of segment
            if (ptr[0] == '0' && (ptr[1] == '\0' || ptr[1] == ',' || isspace((unsigned char)ptr[1]))) {
                AddByteToArray(&result.all_music_bytes, 0x00);
                ptr++;
                continue;
            }

            char cmd_type = ptr[0];
            char* open_paren = strchr(ptr, '(');
            char* close_paren = strchr(ptr, ')');

            if (!open_paren || !close_paren || open_paren > close_paren) {
                if (close_paren) ptr = close_paren + 1;
                else ptr += strlen(ptr);
                continue;
            }

            size_t args_len = close_paren - (open_paren + 1);
            char* args_str = (char*)malloc(args_len + 1);
            if (!args_str) { ptr = close_paren + 1; continue; }
            strncpy(args_str, open_paren + 1, args_len);
            args_str[args_len] = '\0';

            char* args_copy = strdup(args_str);
            if (!args_copy) { free(args_str); ptr = close_paren + 1; continue; }

            if (cmd_type == 'D') {
                unsigned long delay_val = strtoul(args_copy, NULL, 0);
                AddByteToArray(&result.all_music_bytes, (uint8_t)delay_val);
                result.total_delay_frames += delay_val;
            } else if (cmd_type == 'X') {
                unsigned long ch_val = strtoul(args_copy, NULL, 0);
                AddByteToArray(&result.all_music_bytes, (uint8_t)(127 + ch_val));
            } else if (cmd_type == 'N') {
                char* tok = strtok(args_copy, ",");
                unsigned long ch = strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
                unsigned long note = strtoul(tok, NULL, 0);
                AddByteToArray(&result.all_music_bytes, (uint8_t)(143 + ch));
                AddByteToArray(&result.all_music_bytes, (uint8_t)note);
            } else if (cmd_type == 'M') {
                char* tok = strtok(args_copy, ",");
                unsigned long ch = strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
                unsigned long note = strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
                unsigned long wavA = strtoul(tok, NULL, 0);
                AddByteToArray(&result.all_music_bytes, (uint8_t)(159 + ch));
                AddByteToArray(&result.all_music_bytes, (uint8_t)note);
                AddByteToArray(&result.all_music_bytes, (uint8_t)wavA);
            } else if (cmd_type == 'W') {
                char* tok = strtok(args_copy, ",");
                unsigned long ch = strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
                unsigned long note = strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
                unsigned long wavA = strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
                unsigned long wavX = strtoul(tok, NULL, 0);
                AddByteToArray(&result.all_music_bytes, (uint8_t)(175 + ch));
                AddByteToArray(&result.all_music_bytes, (uint8_t)note);
                AddByteToArray(&result.all_music_bytes, (uint8_t)wavA);
                AddByteToArray(&result.all_music_bytes, (uint8_t)wavX);
            }

            free(args_str);
            free(args_copy);
            ptr = close_paren + 1;
        }

        free(seg_copy);
        current_pos = data_end + strlen(array_end_marker);
    }

    GtLog("[GT] Parsed %u segments, %u bytes, %lu total delay frames\n",
          result.actual_segment_count, (unsigned)result.all_music_bytes.size, result.total_delay_frames);
    return result;
}

// ============ BuildEventTimeline ============
static void BuildEventTimeline(const ParsedMusicData* md, EventTimeline* tl, bool skipInitialSilence) {
    InitEventTimeline(tl);
    uint8_t channelMask = 0;
    unsigned long abs_frame = 0;

    unsigned int seg_idx = 0;
    unsigned int byte_off = 0;

    struct ByteCursor {
        const ParsedMusicData* md;
        unsigned int seg;
        unsigned int off;
    };
    ByteCursor cur;
    cur.md = md;
    cur.seg = 0;
    cur.off = 0;

    // Skip initial silence: find first segment with non-delay, non-zero commands
    if (skipInitialSilence) {
        unsigned int skipToSeg = 0;
        for (unsigned int s = 0; s < md->actual_segment_count; s++) {
            size_t start = md->segment_start_indices.data[s];
            bool hasMusic = false;
            for (unsigned int b = 0; b + start < md->all_music_bytes.size; b++) {
                uint8_t c = md->all_music_bytes.data[start + b];
                if (c == 0) break; // end of segment
                if (c >= 0x80) { hasMusic = true; break; } // non-delay command = music
            }
            if (hasMusic) { skipToSeg = s; break; }
        }
        if (skipToSeg > 0) {
            GtLog("[GT] Skipping %u initial silence segments\n", skipToSeg);
            cur.seg = skipToSeg;
            cur.off = 0;
        }
    }

    while (cur.seg < md->actual_segment_count) {
        uint8_t cmd;
        {
            if (cur.seg >= md->actual_segment_count) break;
            size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
            if (abs_idx >= md->all_music_bytes.size) break;
            cmd = md->all_music_bytes.data[abs_idx];
            cur.off++;
        }

        if (cmd == 0) {
            cur.seg++;
            cur.off = 0;
            continue;
        }

        if (cmd < 0x80) {
            // D(x): delay x frames
            abs_frame += cmd;
        } else if (cmd >= 0x80 && cmd < 0x90) {
            // X(c): channel off
            uint8_t ch = (cmd - 127) - 1;
            uint16_t base = (uint16_t)(ch + 1) << 8;
            TlAdd(tl, abs_frame, base | adrFnumL, 0);
            TlAdd(tl, abs_frame, base | adrFnumH, 0);
        } else if (cmd >= 0x90 && cmd < 0xA0) {
            // N(c,n): note on
            uint8_t ch = (cmd - 143) - 1;
            uint8_t note;
            {
                size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
                if (abs_idx >= md->all_music_bytes.size) break;
                note = md->all_music_bytes.data[abs_idx];
                cur.off++;
            }
            uint16_t fnum = FNUM_TABLE[note] / 8;
            uint16_t base = (uint16_t)(ch + 1) << 8;
            TlAdd(tl, abs_frame, base | adrFnumL, fnum & 0x7F);
            TlAdd(tl, abs_frame, base | adrFnumH, (fnum >> 7) & 0x7F);
            channelMask |= (1 << ch);
            TlAdd(tl, abs_frame, 0x21, channelMask);
        } else if (cmd >= 0xA0 && cmd < 0xB0) {
            // M(c,n,v): note on + wavA
            uint8_t ch = (cmd - 159) - 1;
            uint8_t note;
            {
                size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
                if (abs_idx >= md->all_music_bytes.size) break;
                note = md->all_music_bytes.data[abs_idx];
                cur.off++;
            }
            uint8_t wavA;
            {
                size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
                if (abs_idx >= md->all_music_bytes.size) break;
                wavA = md->all_music_bytes.data[abs_idx];
                cur.off++;
            }
            uint16_t fnum = FNUM_TABLE[note] / 8;
            uint16_t base = (uint16_t)(ch + 1) << 8;
            TlAdd(tl, abs_frame, base | adrFnumL, fnum & 0x7F);
            TlAdd(tl, abs_frame, base | adrFnumH, (fnum >> 7) & 0x7F);
            TlAdd(tl, abs_frame, base | adrWavA, wavA);
            channelMask |= (1 << ch);
            TlAdd(tl, abs_frame, 0x21, channelMask);
        } else if (cmd >= 0xB0) {
            // W(c,n,v,w): note on + wavA + wavX
            uint8_t ch = (cmd - 175) - 1;
            uint8_t note;
            {
                size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
                if (abs_idx >= md->all_music_bytes.size) break;
                note = md->all_music_bytes.data[abs_idx];
                cur.off++;
            }
            uint8_t wavA;
            {
                size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
                if (abs_idx >= md->all_music_bytes.size) break;
                wavA = md->all_music_bytes.data[abs_idx];
                cur.off++;
            }
            uint8_t wavX;
            {
                size_t abs_idx = md->segment_start_indices.data[cur.seg] + cur.off;
                if (abs_idx >= md->all_music_bytes.size) break;
                wavX = md->all_music_bytes.data[abs_idx];
                cur.off++;
            }
            uint16_t fnum = FNUM_TABLE[note] / 8;
            uint16_t base = (uint16_t)(ch + 1) << 8;
            TlAdd(tl, abs_frame, base | adrFnumL, fnum & 0x7F);
            TlAdd(tl, abs_frame, base | adrFnumH, (fnum >> 7) & 0x7F);
            TlAdd(tl, abs_frame, base | adrWavA, wavA);
            TlAdd(tl, abs_frame, base | adrWavX, wavX);
            channelMask |= (1 << ch);
            TlAdd(tl, abs_frame, 0x21, channelMask);
        }
    }

    GtLog("[GT] Timeline built: %u events, channelMask=0x%02X\n",
          (unsigned)tl->size, channelMask);
}

// ============ AdvanceTimeline ============
static void AdvanceTimeline(GigatronState* gt_state, EventTimeline* tl, unsigned long current_frame) {
    while (tl->play_pos < tl->size && tl->events[tl->play_pos].frame <= current_frame) {
        const FrameEvent* e = &tl->events[tl->play_pos];
        gigatron_emu_write_register(gt_state, e->reg, e->val);
        tl->play_pos++;
    }
}

// ============ GetMusicFilesInDir ============
static std::vector<std::string> GetMusicFilesInDir(const std::string& path) {
    std::vector<std::string> files;
    std::wstring wPath = MidiPlayer::UTF8ToWide(path);
    std::wstring wSearch = wPath + L"\\*.c";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wSearch.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = MidiPlayer::WideToUTF8(std::wstring(fd.cFileName));
        if (name.length() > 2 && name.substr(name.length() - 2) == ".c") {
            files.push_back(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
}

// ============ Reverse FNUM Lookup ============
// FNUM_TABLE has 96 entries (8 octaves x 12 notes). note_index 0 = C1.
// gc.key stores fnum * 4 (due to adrFnumH writing key *= 4).
// Timeline writes fnum = FNUM_TABLE[note] / 8, so gc.key = FNUM_TABLE[note] / 8 * 4 = FNUM_TABLE[note] / 2.
// Returns note_index (0-95) or -1 if not found.
static int ReverseLookupFnum(uint16_t key_val) {
    for (int i = 0; i < (int)FNUM_TABLE_SIZE; i++) {
        if (FNUM_TABLE[i] == 0) continue;
        if ((FNUM_TABLE[i] / 2) == key_val) return i;
    }
    // Fallback: find closest match
    int best = -1;
    int bestDiff = 0xFFFF;
    for (int i = 0; i < (int)FNUM_TABLE_SIZE; i++) {
        if (FNUM_TABLE[i] == 0) continue;
        int diff = abs((int)(FNUM_TABLE[i] / 2) - (int)key_val);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

// ============ Config ============
static void LoadConfig() {
    char exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    snprintf(s_configPath, MAX_PATH, "%s\\gigatron_config.ini", exeDir);

    // [Settings]
    s_audioBitDepth = GetPrivateProfileIntA("Settings", "AudioBitDepth", 4, s_configPath);
    s_dcOffsetRemoval = GetPrivateProfileIntA("Settings", "DCOffsetRemoval", 0, s_configPath) != 0;
    s_playbackSpeed = (float)GetPrivateProfileIntA("Settings", "PlaybackSpeed", 10, s_configPath) / 10.0f;
    s_targetSegmentCount = (unsigned int)GetPrivateProfileIntA("Settings", "TargetSegments", 0, s_configPath);
    s_showScope = GetPrivateProfileIntA("Settings", "ShowScope", 0, s_configPath) != 0;
    s_scopeHeight = (float)GetPrivateProfileIntA("Settings", "ScopeHeight", 200, s_configPath);
    s_volumeScale = (float)GetPrivateProfileIntA("Settings", "VolumeScale", 5, s_configPath) / 10.0f;
    s_skipInitialSilence = GetPrivateProfileIntA("Settings", "SkipInitialSilence", 0, s_configPath) != 0;

    char buf[MAX_PATH] = "";
    GetPrivateProfileStringA("Settings", "MusicDir", "", buf, MAX_PATH, s_configPath);
    if (buf[0]) snprintf(s_musicDir, MAX_PATH, "%s", buf);

    // Scope settings
    for (int i = 0; i < 4; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ScopeWidth%d", i);
        s_scopeWidth[i] = (float)GetPrivateProfileIntA("ScopeSettings", key, 220, s_configPath);
        snprintf(key, sizeof(key), "ScopeAmp%d", i);
        s_scopeAmp[i] = (float)GetPrivateProfileIntA("ScopeSettings", key, 10, s_configPath) / 10.0f;
    }
    s_scopeSamples = GetPrivateProfileIntA("ScopeSettings", "Samples", 512, s_configPath);
    s_scopeSearchWindow = GetPrivateProfileIntA("ScopeSettings", "SearchWindow", 256, s_configPath);
    s_scopeOffset = GetPrivateProfileIntA("ScopeSettings", "Offset", 0, s_configPath);
    s_scopeEdgeAlign = GetPrivateProfileIntA("ScopeSettings", "EdgeAlign", 1, s_configPath) != 0;
    s_scopeAcMode = GetPrivateProfileIntA("ScopeSettings", "ACMode", 1, s_configPath);

    // Wave editor settings
    s_useCustomWaveTable = GetPrivateProfileIntA("WaveEditor", "UseCustom", 0, s_configPath) != 0;
    s_waveTableBits = GetPrivateProfileIntA("WaveEditor", "Bits", 6, s_configPath);
    if (s_waveTableBits != 6 && s_waveTableBits != 8 && s_waveTableBits != 12 && s_waveTableBits != 16)
        s_waveTableBits = 6;

    // [GtFolderHistory]
    s_folderHistory.clear();
    for (int i = 0; i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        char val[MAX_PATH] = "";
        GetPrivateProfileStringA("GtFolderHistory", key, "", val, MAX_PATH, s_configPath);
        if (val[0] == '\0') break;
        s_folderHistory.push_back(std::string(val));
    }

    GtLog("[GT] Config loaded from %s\n", s_configPath);
}

static void SaveConfig() {
    WritePrivateProfileStringA("Settings", "AudioBitDepth",
        std::to_string(s_audioBitDepth).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "DCOffsetRemoval",
        s_dcOffsetRemoval ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "PlaybackSpeed",
        std::to_string((int)(s_playbackSpeed * 10)).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "TargetSegments",
        std::to_string(s_targetSegmentCount).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "ShowScope",
        s_showScope ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "ScopeHeight",
        std::to_string((int)s_scopeHeight).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "VolumeScale",
        std::to_string((int)(s_volumeScale * 10)).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "SkipInitialSilence",
        s_skipInitialSilence ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "MusicDir", s_musicDir, s_configPath);

    // Scope settings
    for (int i = 0; i < 4; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ScopeWidth%d", i);
        WritePrivateProfileStringA("ScopeSettings", key,
            std::to_string((int)s_scopeWidth[i]).c_str(), s_configPath);
        snprintf(key, sizeof(key), "ScopeAmp%d", i);
        WritePrivateProfileStringA("ScopeSettings", key,
            std::to_string((int)(s_scopeAmp[i] * 10)).c_str(), s_configPath);
    }
    WritePrivateProfileStringA("ScopeSettings", "Samples",
        std::to_string(s_scopeSamples).c_str(), s_configPath);
    WritePrivateProfileStringA("ScopeSettings", "SearchWindow",
        std::to_string(s_scopeSearchWindow).c_str(), s_configPath);
    WritePrivateProfileStringA("ScopeSettings", "Offset",
        std::to_string(s_scopeOffset).c_str(), s_configPath);
    WritePrivateProfileStringA("ScopeSettings", "EdgeAlign",
        s_scopeEdgeAlign ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("ScopeSettings", "ACMode",
        std::to_string(s_scopeAcMode).c_str(), s_configPath);

    // Wave editor settings
    WritePrivateProfileStringA("WaveEditor", "UseCustom",
        s_useCustomWaveTable ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("WaveEditor", "Bits",
        std::to_string(s_waveTableBits).c_str(), s_configPath);

    // Folder history
    // Clear existing entries first
    WritePrivateProfileStringA("GtFolderHistory", NULL, NULL, s_configPath);
    for (int i = 0; i < (int)s_folderHistory.size() && i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        WritePrivateProfileStringA("GtFolderHistory", key,
            s_folderHistory[i].c_str(), s_configPath);
    }
}

// ============ Navigation ============
static void AddToFolderHistory(const char* path) {
    // Remove duplicate if exists
    for (int i = (int)s_folderHistory.size() - 1; i >= 0; i--) {
        if (s_folderHistory[i] == std::string(path)) {
            s_folderHistory.erase(s_folderHistory.begin() + i);
            break;
        }
    }
    s_folderHistory.insert(s_folderHistory.begin(), std::string(path));
    if (s_folderHistory.size() > 50) s_folderHistory.resize(50);
}

static void RefreshFileList() {
    s_fileList.clear();
    s_selectedFileIdx = -1;

    // Add parent entry if not at root
    if (strlen(s_musicDir) > 3) {
        MidiPlayer::FileEntry parent;
        parent.name = "..";
        parent.fullPath = "";
        parent.isDirectory = true;
        s_fileList.push_back(parent);
    }

    std::wstring wCurrentPath = MidiPlayer::UTF8ToWide(s_musicDir);
    std::wstring wSearchPath = wCurrentPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<MidiPlayer::FileEntry> dirs, files;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        MidiPlayer::FileEntry entry;
        entry.name = MidiPlayer::WideToUTF8(std::wstring(fd.cFileName));
        std::wstring wFullPath = wCurrentPath + L"\\" + fd.cFileName;
        entry.fullPath = MidiPlayer::WideToUTF8(wFullPath);
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (entry.isDirectory) {
            dirs.push_back(entry);
        } else {
            // Accept .c files (especially .gbas.c)
            std::string name = entry.name;
            if (name.length() > 2 && name.substr(name.length() - 2) == ".c") {
                files.push_back(entry);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(dirs.begin(), dirs.end(),
        [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b) {
            return a.name < b.name;
        });
    std::sort(files.begin(), files.end(),
        [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b) {
            return a.name < b.name;
        });
    for (auto& d : dirs) s_fileList.push_back(d);
    for (auto& f : files) s_fileList.push_back(f);
}

static void NavigateTo(const char* rawPath) {
    std::wstring wRaw = MidiPlayer::UTF8ToWide(rawPath);
    wchar_t wCanon[MAX_PATH];
    if (_wfullpath(wCanon, wRaw.c_str(), MAX_PATH) == nullptr)
        wcsncpy(wCanon, wRaw.c_str(), MAX_PATH);
    std::string canon = MidiPlayer::WideToUTF8(std::wstring(wCanon));
    snprintf(s_musicDir, MAX_PATH, "%s", canon.c_str());

    if (!s_navigating) {
        if (s_navPos < (int)s_navHistory.size() - 1)
            s_navHistory.erase(s_navHistory.begin() + s_navPos + 1, s_navHistory.end());
        s_navHistory.push_back(canon);
        s_navPos++;
    }
    s_navigating = false;

    RefreshFileList();
    AddToFolderHistory(s_musicDir);
    SaveConfig();
}

static void NavBack() {
    if (s_navPos > 0) {
        s_navPos--;
        s_navigating = true;
        NavigateTo(s_navHistory[s_navPos].c_str());
    }
}

static void NavForward() {
    if (s_navPos < (int)s_navHistory.size() - 1) {
        s_navPos++;
        s_navigating = true;
        NavigateTo(s_navHistory[s_navPos].c_str());
    }
}

static void NavToParent() {
    char parentPath[MAX_PATH];
    strncpy(parentPath, s_musicDir, MAX_PATH);
    int len = (int)strlen(parentPath);
    while (len > 0 && parentPath[len - 1] == '\\') { parentPath[--len] = '\0'; }
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        *lastSlash = '\0';
        NavigateTo(parentPath);
    }
}

// ============ Playback ============
static void StopPlayback() {
    if (s_musicPlaying) {
        s_audioOutput->close();
        s_musicPlaying = false;
    }
    s_musicPaused = false;
    FreeParsedMusicData(&s_parsedMusicData);
    InitParsedMusicData(&s_parsedMusicData);
    FreeEventTimeline(&s_timeline);
    InitEventTimeline(&s_timeline);
    s_currentFrame = 0;
    s_totalEstimatedFrames = 0;
    s_samplesDue = 0.0;
    s_rcRatioNeedsUpdate = false;
    GtLog("[GT] Playback stopped\n");
}

static void PlaySelected() {
    if (s_selectedFileIdx < 0 || s_selectedFileIdx >= (int)s_fileList.size()) {
        GtLog("[GT] No file selected\n");
        return;
    }
    PlayFileByIndex(s_selectedFileIdx);
}

static void PlayFileByIndex(int idx) {
    // Stop any current playback
    StopPlayback();

    if (idx < 0 || idx >= (int)s_fileList.size()) {
        GtLog("[GT] Invalid file index\n");
        return;
    }
    const MidiPlayer::FileEntry& entry = s_fileList[idx];
    if (entry.isDirectory) {
        GtLog("[GT] Cannot play a directory\n");
        return;
    }

    s_selectedFileIdx = idx;
    s_selectedFilePath = entry.fullPath;
    GtLog("[GT] Loading: %s\n", entry.fullPath.c_str());

    // Read file
    char* content = ReadFileContent(entry.fullPath.c_str());
    if (!content) return;

    // Parse
    FreeParsedMusicData(&s_parsedMusicData);
    s_parsedMusicData = ParseGbasCFile(content, s_targetSegmentCount);
    free(content);

    if (s_parsedMusicData.actual_segment_count == 0) {
        GtLog("[GT] No segments parsed\n");
        return;
    }

    // Build timeline
    BuildEventTimeline(&s_parsedMusicData, &s_timeline, s_skipInitialSilence);

    if (s_timeline.size == 0) {
        GtLog("[GT] Timeline has no events\n");
        return;
    }

    // Init audio
    if (s_audioOutput->init(GT_SAMPLE_RATE) == 0) {
        GtLog("[GT] Audio init failed (%s)\n", s_audioOutput->name);
        return;
    }

    // Reset emulator
    gigatron_emu_init(&s_gtState);
    s_gtState.audioSampleRate = GT_SAMPLE_RATE;
    s_gtState.audio_bit_depth = (uint8_t)s_audioBitDepth;
    s_gtState.dc_offset_removal_enabled = s_dcOffsetRemoval;
    s_gtState.dc_bias = 0.0;
    s_gtState.dc_alpha = 0.99;
    s_gtState.volume_scale = s_volumeScale;
    s_gtState.channelMask = 0;

    // Init RC ratio
    RC_SET_RATIO(&s_musicEventRC,
        (unsigned int)(GT_FRAME_RATE_INT * s_playbackSpeed),
        GT_SAMPLE_RATE * 100);
    s_rcRatioNeedsUpdate = false;

    // Reset frame counter
    s_currentFrame = 0;
    s_musicPaused = false;

    // Total estimated frames from parsed data
    s_totalEstimatedFrames = s_parsedMusicData.total_delay_frames;
    if (s_totalEstimatedFrames == 0) s_totalEstimatedFrames = 1;

    // Reset perf counter
    QueryPerformanceCounter(&s_lastPerfCounter);
    s_samplesDue = 0.0;

    s_musicPlaying = true;
    GtLog("[GT] Playing: %s (%u segments, %u events, ~%lu frames)\n",
          entry.name.c_str(),
          s_parsedMusicData.actual_segment_count,
          (unsigned)s_timeline.size,
          s_parsedMusicData.total_delay_frames);
}

static void SeekToFrame(unsigned long targetFrame) {
    // Save current state before rebuild
    bool wasPaused = s_musicPaused;

    // Close audio
    if (s_musicPlaying) {
        s_audioOutput->close();
        s_musicPlaying = false;
    }

    // Reset emulator
    gigatron_emu_init(&s_gtState);
    s_gtState.audioSampleRate = GT_SAMPLE_RATE;
    s_gtState.audio_bit_depth = (uint8_t)s_audioBitDepth;
    s_gtState.dc_offset_removal_enabled = s_dcOffsetRemoval;
    s_gtState.dc_bias = 0.0;
    s_gtState.dc_alpha = 0.99;
    s_gtState.volume_scale = s_volumeScale;
    s_gtState.channelMask = 0;

    // Re-init audio
    if (s_audioOutput->init(GT_SAMPLE_RATE) == 0) {
        GtLog("[GT] Audio re-init failed after seek\n");
        return;
    }

    // Reset timeline position to beginning
    s_timeline.play_pos = 0;
    s_currentFrame = 0;

    // Init RC ratio
    RC_SET_RATIO(&s_musicEventRC,
        (unsigned int)(GT_FRAME_RATE_INT * s_playbackSpeed),
        GT_SAMPLE_RATE * 100);
    s_rcRatioNeedsUpdate = false;

    // Fast-forward: advance timeline up to targetFrame without audio output
    for (unsigned long f = 0; f < targetFrame && s_timeline.play_pos < s_timeline.size; f++) {
        AdvanceTimeline(&s_gtState, &s_timeline, f);
    }
    s_currentFrame = targetFrame;

    // Reset perf counter
    QueryPerformanceCounter(&s_lastPerfCounter);
    s_samplesDue = 0.0;

    s_musicPaused = wasPaused;
    s_musicPlaying = true;

    GtLog("[GT] Seeked to frame %lu / %lu\n", targetFrame, s_totalEstimatedFrames);
}

// ============ Update ============
void Update() {
    if (!s_musicPlaying || s_musicPaused) {
        QueryPerformanceCounter(&s_lastPerfCounter);
        s_samplesDue = 0.0;
        return;
    }

    // Update RC ratio if needed (speed changed)
    if (s_rcRatioNeedsUpdate) {
        RC_SET_RATIO(&s_musicEventRC,
            (unsigned int)(GT_FRAME_RATE_INT * s_playbackSpeed),
            GT_SAMPLE_RATE * 100);
        s_rcRatioNeedsUpdate = false;
    }

    // Calculate elapsed time
    LARGE_INTEGER currentPerf;
    QueryPerformanceCounter(&currentPerf);
    double elapsed = (double)(currentPerf.QuadPart - s_lastPerfCounter.QuadPart) / (double)s_perfFrequency.QuadPart;
    s_lastPerfCounter = currentPerf;
    s_samplesDue += elapsed * GT_SAMPLE_RATE;

    // Push audio buffers
    while (s_samplesDue >= GT_BUFFER_SAMPLES) {
        gigatron_emu_update(&s_gtState, s_audioBuffer, GT_BUFFER_SAMPLES);
        s_audioOutput->play((char*)s_audioBuffer, GT_BUFFER_SIZE);
        s_samplesDue -= GT_BUFFER_SAMPLES;

        RC_STEPS(&s_musicEventRC, GT_BUFFER_SAMPLES);

        while (RC_GET_VAL(&s_musicEventRC) >= 1) {
            RC_VAL_SUB(&s_musicEventRC, 1);
            s_currentFrame++;
            AdvanceTimeline(&s_gtState, &s_timeline, s_currentFrame);

            if (s_timeline.play_pos >= s_timeline.size) {
                s_musicPlaying = false;
                s_samplesDue = 0.0;
                s_audioOutput->close();
                GtLog("[GT] Playback complete (%lu frames)\n", s_currentFrame);
                FreeParsedMusicData(&s_parsedMusicData);
                InitParsedMusicData(&s_parsedMusicData);
                FreeEventTimeline(&s_timeline);
                InitEventTimeline(&s_timeline);
                return;
            }
        }
    }
}

// ============ Render ============
void Render() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Gigatron")) { ImGui::End(); return; }

    ImGui::BeginChild("GT_LeftPane", ImVec2(300, 0), true);
    RenderControls();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("GT_RightPane", ImVec2(0, 0), false);

    static float s_levelMeterHeight = 200.0f;
    float pianoHeight = 150;
    float statusAreaWidth = 560;

    float topSectionHeight;
    if (s_showScope) {
        topSectionHeight = pianoHeight + s_scopeHeight + 4;
    } else {
        topSectionHeight = pianoHeight + s_levelMeterHeight;
    }

    ImGui::BeginGroup();
    ImGui::BeginChild("GT_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoArea();
    ImGui::EndChild();

    if (s_showScope) {
        ImGui::BeginChild("GT_ScopeBackground", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, s_scopeHeight), false);
        RenderScopeArea();
        ImGui::EndChild();

        // Resizable splitter
        ImGui::Button("##GTScopeSplitter", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, 4));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float delta = ImGui::GetIO().MouseDelta.y;
            s_scopeHeight += delta;
            if (s_scopeHeight < 30.0f) s_scopeHeight = 30.0f;
            if (s_scopeHeight > 500.0f) s_scopeHeight = 500.0f;
            SaveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    } else {
        ImGui::BeginChild("GT_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, s_levelMeterHeight), false);
        RenderLevelMeterArea();
        ImGui::EndChild();
    }

    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginChild("GT_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderStatusArea();
    ImGui::EndChild();

    ImGui::BeginChild("GT_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderFileBrowser();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("GT_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();

    ImGui::EndChild(); // GT_RightPane
    RenderScopeSettingsWindow();
    RenderWaveEditorWindow();
    ImGui::End();
}

// ============ RenderControls ============
static void RenderControls() {
    if (!ImGui::CollapsingHeader("Gigatron Controls##controls", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // File info
    if (!s_selectedFilePath.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Gigatron Tracker");
        ImGui::TextDisabled("File:"); ImGui::SameLine();
        size_t lastSlash = s_selectedFilePath.find_last_of("\\/");
        std::string fname = (lastSlash != std::string::npos)
            ? s_selectedFilePath.substr(lastSlash + 1) : s_selectedFilePath;
        if (fname.length() > 35) fname = "..." + fname.substr(fname.length() - 32);
        ImGui::Text("%s", fname.c_str());

        if (s_parsedMusicData.actual_segment_count > 0) {
            ImGui::TextDisabled("Segments:"); ImGui::SameLine();
            ImGui::Text("%u", s_parsedMusicData.actual_segment_count);
            ImGui::TextDisabled("Bytes:"); ImGui::SameLine();
            ImGui::Text("%u", (unsigned)s_parsedMusicData.all_music_bytes.size);

            double durSec = (double)s_parsedMusicData.total_delay_frames / 59.98;
            int durMin = (int)durSec / 60;
            int durSecInt = (int)durSec % 60;
            ImGui::TextDisabled("Duration:"); ImGui::SameLine();
            ImGui::Text("%02d:%02d", durMin, durSecInt);
        }
    } else {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Gigatron Tracker");
        ImGui::TextDisabled("[No file selected]");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Playback Settings =====
    ImGui::TextDisabled("-- Playback --");

    // Speed slider
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Speed##gtspeed", &s_playbackSpeed, 0.1f, 4.0f, "%.1fx")) {
        s_rcRatioNeedsUpdate = true;
        SaveConfig();
    }

    // Target segments
    ImGui::SetNextItemWidth(100.0f);
    int segInt = (int)s_targetSegmentCount;
    if (ImGui::InputInt("Segments (0=all)##gtseg", &segInt)) {
        s_targetSegmentCount = (segInt < 0) ? 0 : (unsigned int)segInt;
        SaveConfig();
    }

    // Skip initial silence
    if (ImGui::Checkbox("Skip Initial Silence", &s_skipInitialSilence)) {
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Skip leading empty segments (delays only) when loading a file.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Audio Settings =====
    ImGui::TextDisabled("-- Audio --");

    // Volume slider
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Volume##gtvol", &s_volumeScale, 0.0f, 2.0f, "%.2f")) {
        s_gtState.volume_scale = s_volumeScale;
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Output volume scale. 0.5 = default (matches original Tracker).\nHigher values may cause clipping.");
    }

    // Bit depth (radio buttons, wrapped to fit narrow sidebar)
    ImGui::TextDisabled("Bit Depth:");
    int bd = s_audioBitDepth;
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 labelSize = ImGui::CalcTextSize("16-bit", NULL, true);
    float itemW = labelSize.x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
    int cols = (int)(avail / itemW);
    if (cols < 1) cols = 1;
    int row = 0;
    for (int i = 0; i < 5; i++) {
        int bits[] = {4, 6, 8, 12, 16};
        if (i > 0 && i % cols != 0) ImGui::SameLine();
        if (ImGui::RadioButton(("##bd" + std::to_string(bits[i])).c_str(), bd == bits[i])) { bd = bits[i]; }
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled("%d-bit", bits[i]);
    }
    if (bd != s_audioBitDepth) {
        s_audioBitDepth = bd;
        s_gtState.audio_bit_depth = (uint8_t)s_audioBitDepth;
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("DAC quantization: 4-bit = original Gigatron hardware.\n"
            "Higher = more dynamic range, less harsh quantization noise.");
    }

    // DC offset removal
    if (ImGui::Checkbox("DC Offset Removal##gtdc", &s_dcOffsetRemoval)) {
        s_gtState.dc_offset_removal_enabled = s_dcOffsetRemoval;
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable DC offset removal filter (IIR high-pass).\nOriginal Tracker default: OFF.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Scope Settings =====
    ImGui::TextDisabled("-- Scope --");

    ImGui::PushStyleColor(ImGuiCol_Button,
        s_showScope ? ImVec4(0.2f, 0.7f, 0.3f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    if (ImGui::SmallButton("Scope##gttoggle")) {
        s_showScope = !s_showScope;
        SaveConfig();
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle oscilloscope / level meters");

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,
        s_showScopeSettingsWindow ? ImVec4(0.2f, 0.7f, 0.3f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    if (ImGui::SmallButton("Scope Settings...##gtbtn")) {
        s_showScopeSettingsWindow = !s_showScopeSettingsWindow;
    }
    ImGui::PopStyleColor();

    if (s_showScope) {
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("Scope Height##gth", &s_scopeHeight, 30.0f, 500.0f, "%.0fpx")) {
            SaveConfig();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Wave Editor =====
    ImGui::TextDisabled("-- Wave Editor --");

    ImGui::PushStyleColor(ImGuiCol_Button,
        s_showWaveEditorWindow ? ImVec4(0.2f, 0.7f, 0.3f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    if (ImGui::SmallButton("Wave Editor...##gtwedit")) {
        s_showWaveEditorWindow = !s_showWaveEditorWindow;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open custom waveform editor with mouse drawing");

    if (s_useCustomWaveTable) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Custom: ON");
    } else {
        ImGui::TextDisabled("Custom: OFF");
    }
}

// ============ RenderPianoArea ============
static void RenderPianoArea() {
    // Track active notes from Gigatron channel state
    bool noteActive[128] = {};
    ImU32 noteColor[128] = {};
    float noteLevel[128] = {};

    for (int ch = 0; ch < 4; ch++) {
        const GigatronChannel& gc = s_gtState.ch[ch];
        if (gc.key > 0) {
            // Channel is active - find MIDI note from key (fnum)
            // key = fnum value set by the emulator. We reverse-lookup.
            uint16_t fnum_div8 = gc.key;
            int noteIdx = ReverseLookupFnum(fnum_div8);
            if (noteIdx >= 0 && noteIdx < (int)FNUM_TABLE_SIZE) {
                // noteIdx 0 = C1 (MIDI 12), 95 = C8 (MIDI 107)
                int midiNote = noteIdx;
                if (midiNote >= 0 && midiNote < 128) {
                    noteActive[midiNote] = true;
                    float level = (gc.wavA != 0) ? fabsf((float)gc.wavA) / 32.0f : 0.3f;
                    if (level > 1.0f) level = 1.0f;
                    if (level > noteLevel[midiNote] || !noteActive[midiNote]) {
                        noteLevel[midiNote] = level;
                        noteColor[midiNote] = kChColors[ch];
                    }
                }
            }
        }
    }

    // Blend function
    auto blendKey = [](ImU32 col, float lv, bool isBlack) -> ImU32 {
        float blendLv = 0.55f + lv * 0.45f;
        int r = (col >> IM_COL32_R_SHIFT) & 0xFF;
        int g = (col >> IM_COL32_G_SHIFT) & 0xFF;
        int b = (col >> IM_COL32_B_SHIFT) & 0xFF;
        int br = isBlack ? 20 : 255;
        int bg = isBlack ? 20 : 255;
        int bb = isBlack ? 20 : 255;
        int fr = br + (int)((r - br) * blendLv);
        int fg = bg + (int)((g - bg) * blendLv);
        int fb = bb + (int)((b - bb) * blendLv);
        return IM_COL32(fr, fg, fb, 255);
    };

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float whiteKeyHeight = ImGui::GetContentRegionAvail().y - 4;
    if (whiteKeyHeight < 40.0f) whiteKeyHeight = 40.0f;
    float blackKeyHeight = whiteKeyHeight * 0.62f;

    const int kMinNote = 24;
    const int kMaxNote = 107;
    static const bool kIsBlack[] = {
        false, true, false, true, false, false, true, false, true, false, true, false
    };

    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!kIsBlack[n % 12]) numWhiteKeys++;
    }

    float whiteKeyWidth = availW / (float)numWhiteKeys;
    if (whiteKeyWidth < 6.0f) whiteKeyWidth = 6.0f;
    float blackKeyWidth = whiteKeyWidth * 0.65f;

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (kIsBlack[n % 12]) continue;
        float x = p.x + wkIdx * whiteKeyWidth;
        ImU32 fillCol = noteActive[n]
            ? blendKey(noteColor[n], noteLevel[n], false)
            : IM_COL32(255, 255, 255, 255);
        draw_list->AddRectFilled(ImVec2(x, p.y), ImVec2(x + whiteKeyWidth - 1, p.y + whiteKeyHeight), fillCol);
        draw_list->AddRect(ImVec2(x, p.y), ImVec2(x + whiteKeyWidth, p.y + whiteKeyHeight), IM_COL32(80, 80, 80, 255));
        if (n % 12 == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "C%d", (n / 12) - 1);
            ImVec2 tsz = ImGui::CalcTextSize(buf);
            float noteNameY = p.y + whiteKeyHeight - tsz.y - 2;
            draw_list->AddText(ImVec2(x + 2, noteNameY), IM_COL32(0, 0, 0, 180), buf);
        }
        // Channel label on active white keys
        if (noteActive[n]) {
            // Find which channel owns this note
            for (int ch = 0; ch < 4; ch++) {
                if (noteColor[n] == kChColors[ch]) {
                    ImFont* lblFont = ImGui::GetFont();
                    float lblY = p.y + 2;
                    float fs = ImGui::GetFontSize() * 0.65f;
                    draw_list->AddText(lblFont, fs,
                        ImVec2(x + 1, lblY), IM_COL32(0, 0, 0, 200), kChNames[ch]);
                    break;
                }
            }
        }
        wkIdx++;
    }

    // Pass 2: black keys
    wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!kIsBlack[n % 12]) { wkIdx++; continue; }
        float x = p.x + (wkIdx - 1) * whiteKeyWidth + whiteKeyWidth - blackKeyWidth * 0.5f;
        ImU32 fillCol = noteActive[n]
            ? blendKey(noteColor[n], noteLevel[n], true)
            : IM_COL32(20, 20, 20, 255);
        draw_list->AddRectFilled(ImVec2(x, p.y), ImVec2(x + blackKeyWidth, p.y + blackKeyHeight), fillCol);
        draw_list->AddRect(ImVec2(x, p.y), ImVec2(x + blackKeyWidth, p.y + blackKeyHeight), IM_COL32(0, 0, 0, 255));
        // Channel label on active black keys
        if (noteActive[n]) {
            for (int ch = 0; ch < 4; ch++) {
                if (noteColor[n] == kChColors[ch]) {
                    ImFont* lblFont = ImGui::GetFont();
                    float lblY = p.y + 2;
                    float fs = ImGui::GetFontSize() * 0.65f;
                    draw_list->AddText(lblFont, fs,
                        ImVec2(x + 1, lblY), IM_COL32(255, 255, 255, 200), kChNames[ch]);
                    break;
                }
            }
        }
    }

    ImGui::Dummy(ImVec2((float)numWhiteKeys * whiteKeyWidth, whiteKeyHeight));
}

// ============ RenderLevelMeterArea ============
static void RenderLevelMeterArea() {
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y - 4;
    if (availH < 40.0f) availH = 40.0f;

    bool hasData = s_musicPlaying || s_parsedMusicData.actual_segment_count > 0;
    if (!hasData) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(no file)");
        ImGui::Dummy(ImVec2(availW, availH));
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float kChLabelH = 18.0f;
    const float kBarGap = 6.0f;
    const float kBarH = availH - kChLabelH - 4.0f;
    if (kBarH < 10.0f) return;

    float meterW = 28.0f;
    float totalBarsW = 4 * meterW + 3 * kBarGap;
    float startX = (availW - totalBarsW) * 0.5f;
    if (startX < 2.0f) startX = 2.0f;

    ImVec2 basePos = ImGui::GetCursorScreenPos();

    for (int ch = 0; ch < 4; ch++) {
        const GigatronChannel& gc = s_gtState.ch[ch];

        // Update level meter with decay
        float targetLevel = 0.0f;
        if (gc.key > 0) {
            targetLevel = fabsf((float)gc.wavA) / 32.0f;
            if (targetLevel > 1.0f) targetLevel = 1.0f;
        }
        if (targetLevel > s_levelMeter[ch]) {
            s_levelMeter[ch] += (targetLevel - s_levelMeter[ch]) * 0.3f;
        } else {
            s_levelMeter[ch] *= 0.95f;
        }
        if (s_levelMeter[ch] > s_levelPeak[ch]) {
            s_levelPeak[ch] = s_levelMeter[ch];
        } else {
            s_levelPeak[ch] *= 0.998f;
        }

        float level = s_levelMeter[ch];
        float mx = basePos.x + startX + ch * (meterW + kBarGap);
        float barTop = basePos.y + kChLabelH;

        // Channel label above bar
        dl->AddText(ImVec2(mx, basePos.y), kChColors[ch], kChNames[ch]);

        // Background
        dl->AddRectFilled(ImVec2(mx, barTop),
            ImVec2(mx + meterW, barTop + kBarH),
            IM_COL32(25, 25, 25, 255));

        // Vertical level bar (bottom-up)
        if (level > 0.0f) {
            float barFillH = kBarH * level;
            float fillTop = barTop + kBarH - barFillH;

            bool keyOn = (gc.key > 0);
            float dimF = keyOn ? (0.4f + level * 0.6f) : (0.25f + level * 0.2f);
            int r = (int)(((kChColors[ch] >> IM_COL32_R_SHIFT) & 0xFF) * dimF);
            int g = (int)(((kChColors[ch] >> IM_COL32_G_SHIFT) & 0xFF) * dimF);
            int b = (int)(((kChColors[ch] >> IM_COL32_B_SHIFT) & 0xFF) * dimF);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            dl->AddRectFilled(ImVec2(mx + 1, fillTop),
                ImVec2(mx + meterW - 1, fillTop + barFillH),
                IM_COL32(r, g, b, 255));
        }

        // Peak indicator (horizontal line)
        if (s_levelPeak[ch] > 0.01f) {
            float peakY = barTop + kBarH * (1.0f - s_levelPeak[ch]);
            dl->AddLine(ImVec2(mx, peakY), ImVec2(mx + meterW, peakY),
                IM_COL32(255, 255, 255, 180));
        }

        // Border
        dl->AddRect(ImVec2(mx, barTop), ImVec2(mx + meterW, barTop + kBarH),
            IM_COL32(60, 60, 60, 255));

        // dB label below
        float db = (level > 0.001f) ? 20.0f * log10f(level) : -60.0f;
        char dbStr[16];
        snprintf(dbStr, sizeof(dbStr), "%.0f", db);
        dl->AddText(ImVec2(mx, barTop + kBarH + 1), IM_COL32(160, 160, 160, 255), dbStr);
    }

    ImGui::Dummy(ImVec2(availW, availH));
}

// ============ RenderScopeArea ============
static void RenderScopeArea() {
    if (!s_showScope) return;

    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;
    if (availH < 20.0f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 basePos = ImGui::GetCursorScreenPos();

    // Layout: 4 channels side by side
    float totalWidth = 0;
    for (int ch = 0; ch < 4; ch++) totalWidth += s_scopeWidth[ch];
    float gap = 4.0f;
    totalWidth += 3.0f * gap;
    float scale = (availW > totalWidth) ? 1.0f : availW / totalWidth;

    int samples = s_scopeSamples;
    int searchWin = s_scopeSearchWindow;
    bool edgeAlign = s_scopeEdgeAlign;
    int acMode = s_scopeAcMode;
    int scopePos = s_gtState.scope_pos;

    float xOffset = 0;
    for (int ch = 0; ch < 4; ch++) {
        float w = s_scopeWidth[ch] * scale;
        float h = availH;
        if (h < 20.0f) h = 20.0f;

        // Channel label
        ImVec2 chLabelPos(basePos.x + xOffset + 2, basePos.y);
        dl->AddText(chLabelPos, kChColors[ch], kChNames[ch]);

        // Background
        ImVec2 chPos(basePos.x + xOffset, basePos.y + 14);
        float waveH = h - 14;
        dl->AddRectFilled(chPos, ImVec2(chPos.x + w, chPos.y + waveH),
            IM_COL32(10, 10, 15, 255));

        // Center line
        float centerY = chPos.y + waveH * 0.5f;
        dl->AddLine(ImVec2(chPos.x, centerY), ImVec2(chPos.x + w, centerY),
            IM_COL32(40, 40, 50, 255));

        // ===== Cross-correlation trigger =====
        int drawStart = scopePos - samples;
        if (searchWin > 0 && samples > 4) {
            int best_offset = s_scopePersistOffset[ch];

            // Clamp samples for correlation
            int maxSamples = GT_SCOPE_BUF_SIZE - 64;
            if (maxSamples < 4) maxSamples = 4;
            if (samples > maxSamples) samples = maxSamples;

            // Validate persistent offset
            if (best_offset < 0 || best_offset >= GT_SCOPE_BUF_SIZE) {
                best_offset = scopePos - samples;
            }

            static const int CORR_STEP = 4;
            long maxCorr = -1;

            int searchLo = best_offset - searchWin / 2;
            int searchHi = best_offset + searchWin / 2;

            // Bidirectional search from center outward
            int ofsRight = best_offset;
            int ofsLeft = best_offset - CORR_STEP;
            bool rightDone = false, leftDone = false;

            for (;;) {
                if (!rightDone && ofsRight <= searchHi) {
                    long corr = 0;
                    for (int i = 0; i < samples; i += CORR_STEP) {
                        int idx = ((ofsRight + i) % GT_SCOPE_BUF_SIZE + GT_SCOPE_BUF_SIZE) % GT_SCOPE_BUF_SIZE;
                        corr += (long)s_gtState.scope_buf[ch][idx] * (long)s_scopePrevBuf[ch][i];
                    }
                    if (corr > maxCorr) { maxCorr = corr; best_offset = ofsRight; }
                    ofsRight += CORR_STEP;
                } else rightDone = true;

                if (!leftDone && ofsLeft >= searchLo) {
                    long corr = 0;
                    for (int i = 0; i < samples; i += CORR_STEP) {
                        int idx = ((ofsLeft + i) % GT_SCOPE_BUF_SIZE + GT_SCOPE_BUF_SIZE) % GT_SCOPE_BUF_SIZE;
                        corr += (long)s_gtState.scope_buf[ch][idx] * (long)s_scopePrevBuf[ch][i];
                    }
                    if (corr > maxCorr) { maxCorr = corr; best_offset = ofsLeft; }
                    ofsLeft -= CORR_STEP;
                } else leftDone = true;

                if (rightDone && leftDone) break;
            }

            // Edge-align refinement: snap to nearest rising edge
            if (edgeAlign) {
                int refineLo = best_offset - 32;
                int refineHi = best_offset + 32;
                int bestEdge = best_offset;
                int bestDist = 999;
                for (int pos = refineLo; pos <= refineHi; pos++) {
                    int idxCur = ((pos) % GT_SCOPE_BUF_SIZE + GT_SCOPE_BUF_SIZE) % GT_SCOPE_BUF_SIZE;
                    int idxPrev = ((pos - 1) % GT_SCOPE_BUF_SIZE + GT_SCOPE_BUF_SIZE) % GT_SCOPE_BUF_SIZE;
                    int cur = s_gtState.scope_buf[ch][idxCur];
                    int prev = s_gtState.scope_buf[ch][idxPrev];
                    if (prev < 0 && cur >= 0) {
                        int dist = (pos >= best_offset) ? (pos - best_offset) : (best_offset - pos);
                        if (dist < bestDist) { bestDist = dist; bestEdge = pos; }
                    }
                }
                best_offset = bestEdge;
            }

            s_scopePersistOffset[ch] = best_offset;
            drawStart = best_offset + s_scopeOffset;
        }

        // ===== Read waveform into linear buffer =====
        int16_t drawBuf[4096];
        int drawSamples = samples;
        if (drawSamples > 4096) drawSamples = 4096;
        for (int i = 0; i < drawSamples; i++) {
            int idx = ((drawStart + i) % GT_SCOPE_BUF_SIZE + GT_SCOPE_BUF_SIZE) % GT_SCOPE_BUF_SIZE;
            drawBuf[i] = s_gtState.scope_buf[ch][idx];
        }

        // ===== AC coupling =====
        int16_t acBuf[4096];
        const int16_t* drawSrc = drawBuf;
        if (acMode >= 1) {
            bool unipolar = true;
            int minVal = 32767, maxVal = -32768;
            for (int i = 0; i < drawSamples; i++) {
                int16_t v = drawBuf[i];
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
                if (v < -2) { unipolar = false; break; }
            }
            if (unipolar && (maxVal - minVal) > 4) {
                if (acMode == 1) {
                    // Center: subtract DC offset
                    int dc = (minVal + maxVal) / 2;
                    for (int i = 0; i < drawSamples; i++) {
                        int v = (int)drawBuf[i] - dc;
                        if (v > 32767) v = 32767;
                        if (v < -32768) v = -32768;
                        acBuf[i] = (int16_t)v;
                    }
                } else {
                    // Bottom: map min..max to full range
                    int range = maxVal - minVal;
                    if (range < 1) range = 1;
                    for (int i = 0; i < drawSamples; i++) {
                        int v = (int)(drawBuf[i] - minVal) * 65535 / range - 32768;
                        if (v > 32767) v = 32767;
                        if (v < -32768) v = -32768;
                        acBuf[i] = (int16_t)v;
                    }
                }
                drawSrc = acBuf;
            }
        }

        // ===== Draw waveform =====
        float ampScale = s_scopeAmp[ch] * waveH * 0.4f / 32768.0f;
        ImU32 waveColor = kChColors[ch];
        for (int i = 1; i < drawSamples; i++) {
            float val0 = (float)drawSrc[i - 1] * ampScale;
            float val1 = (float)drawSrc[i] * ampScale;

            float x0 = chPos.x + (float)(i - 1) / (float)drawSamples * w;
            float x1 = chPos.x + (float)i / (float)drawSamples * w;
            float y0 = centerY - val0;
            float y1 = centerY - val1;

            // Clamp to channel rect
            if (y0 < chPos.y) y0 = chPos.y;
            if (y0 > chPos.y + waveH) y0 = chPos.y + waveH;
            if (y1 < chPos.y) y1 = chPos.y;
            if (y1 > chPos.y + waveH) y1 = chPos.y + waveH;

            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), waveColor);
        }

        // Store for next frame correlation
        int storeSamples = drawSamples;
        if (storeSamples > 4096) storeSamples = 4096;
        for (int i = 0; i < storeSamples; i++) {
            s_scopePrevBuf[ch][i] = drawSrc[i];
        }

        // Border
        dl->AddRect(chPos, ImVec2(chPos.x + w, chPos.y + waveH),
            IM_COL32(60, 60, 70, 255));

        xOffset += w + gap;
    }

    ImGui::Dummy(ImVec2(availW, availH));
}

// ============ RenderStatusArea ============
static void RenderStatusArea() {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.8f, 1.0f), "Gigatron Channel State");

    static const char* kNoteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char* kWaveNames[] = {"Noise", "Triangle", "Pulse", "Sawtooth"};

    // Per-channel register table
    if (ImGui::BeginTable("##gtreginfo", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Ch",   ImGuiTableColumnFlags_WidthFixed, 32.f);
        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 48.f);
        ImGui::TableSetupColumn("OSC",  ImGuiTableColumnFlags_WidthFixed, 52.f);
        ImGui::TableSetupColumn("KEY",  ImGuiTableColumnFlags_WidthFixed, 52.f);
        ImGui::TableSetupColumn("WAVX", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("WAVA", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableHeadersRow();

        for (int ch = 0; ch < 4; ch++) {
            const GigatronChannel& gc = s_gtState.ch[ch];
            ImVec4 chCol = ImGui::ColorConvertU32ToFloat4(kChColors[ch]);
            bool isActive = (gc.key > 0);

            // Status color: bright when active, dim when inactive
            ImVec4 infoCol = isActive ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.45f, 0.45f, 1);

            ImGui::TableNextRow();

            // Channel name
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(chCol, "%s", kChNames[ch]);

            // Status
            ImGui::TableSetColumnIndex(1);
            if (isActive) {
                ImGui::TextColored(chCol, "ON");
            } else {
                ImGui::TextDisabled("off");
            }

            // Note name
            ImGui::TableSetColumnIndex(2);
            if (isActive) {
                uint16_t fnum_div8 = gc.key;
                int noteIdx = ReverseLookupFnum(fnum_div8);
                if (noteIdx >= 0 && noteIdx < (int)FNUM_TABLE_SIZE) {
                    int octave = noteIdx / 12 + 1;
                    int noteInOctave = noteIdx % 12;
                    ImGui::TextColored(infoCol, "%s%d", kNoteNames[noteInOctave], octave);
                } else {
                    ImGui::TextColored(infoCol, "?");
                }
            } else {
                ImGui::TextDisabled("--");
            }

            // OSC
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(infoCol, "0x%04X", gc.osc);

            // KEY
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(infoCol, "0x%04X", gc.key);

            // WAVX (show waveform name)
            ImGui::TableSetColumnIndex(5);
            int waveIdx = (int)gc.wavX & 3;
            ImGui::TextColored(infoCol, "%s(%u)", kWaveNames[waveIdx], gc.wavX);

            // WAVA
            ImGui::TableSetColumnIndex(6);
            ImGui::TextColored(infoCol, "%d", gc.wavA);
        }
        ImGui::EndTable();
    }

    // Channel mask
    ImGui::Spacing();
    ImGui::TextDisabled("ChannelMask: 0x%02X", s_gtState.channelMask);
    for (int ch = 0; ch < 4; ch++) {
        ImGui::SameLine();
        bool active = (s_gtState.channelMask & (1 << ch)) != 0;
        ImVec4 col = active ? ImGui::ColorConvertU32ToFloat4(kChColors[ch])
                            : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(col, "[%s]", active ? kChNames[ch] : "  ");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Frame counter
    if (s_musicPlaying) {
        const char* stateStr = s_musicPaused ? "(Paused)" : "";
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Frame: %lu / %lu %s",
            s_currentFrame, s_totalEstimatedFrames, stateStr);

        // Timeline progress
        ImGui::TextDisabled("Events: %u / %u",
            (unsigned)s_timeline.play_pos, (unsigned)s_timeline.size);
    } else if (s_parsedMusicData.actual_segment_count > 0) {
        ImGui::TextDisabled("Stopped at frame %lu", s_currentFrame);
    } else {
        ImGui::TextDisabled("(idle)");
    }

    // Audio info
    ImGui::Spacing();
    ImGui::TextDisabled("Audio: %dHz %d-bit %s", GT_SAMPLE_RATE, s_audioBitDepth,
        s_audioOutput ? s_audioOutput->name : "none");
    ImGui::TextDisabled("Speed: %.1fx  DC remove: %s",
        s_playbackSpeed, s_dcOffsetRemoval ? "on" : "off");
}

// ============ RenderFileBrowser ============
static void RenderFileBrowser() {
    // ===== Gigatron Player transport bar (mimics VGM RenderInline) =====
    bool hasFile = !s_selectedFilePath.empty() && s_parsedMusicData.actual_segment_count > 0;
    bool isPlaying = s_musicPlaying && !s_musicPaused;
    bool isPaused = s_musicPlaying && s_musicPaused;

    ImGui::SetNextItemOpen(s_playerBarOpen, ImGuiCond_Once);
    bool playerOpen = ImGui::CollapsingHeader("Gigatron Player##gtplayer",
        s_playerBarOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);

    // Semi-transparent progress bar behind collapsed header
    if (!playerOpen && hasFile) {
        float progress = 0.0f;
        if (s_totalEstimatedFrames > 0) {
            progress = (float)s_currentFrame / (float)s_totalEstimatedFrames;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
        }
        ImVec2 rectMin = ImGui::GetItemRectMin();
        ImVec2 rectMax = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(rectMin, rectMax, true);
        float fillW = (rectMax.x - rectMin.x) * progress;
        dl->AddRectFilled(rectMin,
            ImVec2(rectMin.x + fillW, rectMax.y),
            IM_COL32(100, 180, 255, 50));
        dl->PopClipRect();
    }
    s_playerBarOpen = playerOpen;

    // ===== Title bar mini controls (always visible) =====
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("<<##gtprev")) {
        // Prev file in list
        if (!s_fileList.empty()) {
            int prevIdx = -1;
            // Find current file in list, go to previous non-dir file
            for (int i = s_selectedFileIdx - 1; i >= 0; i--) {
                if (!s_fileList[i].isDirectory) { prevIdx = i; break; }
            }
            if (prevIdx >= 0) PlayFileByIndex(prevIdx);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous file");

    ImGui::SameLine();
    if (isPlaying) {
        if (ImGui::SmallButton("||##gtpause")) {
            Pause();
        }
    } else if (isPaused) {
        if (ImGui::SmallButton(">##gtpause")) {
            Resume();
        }
    } else if (hasFile) {
        if (ImGui::SmallButton(">##gtpause")) {
            PlaySelected();
        }
    } else {
        ImGui::SmallButton(">##gtpause");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play / Pause");

    ImGui::SameLine();
    if (ImGui::SmallButton(">>##gtnext")) {
        // Next file in list
        if (!s_fileList.empty()) {
            int nextIdx = -1;
            for (int i = s_selectedFileIdx + 1; i < (int)s_fileList.size(); i++) {
                if (!s_fileList[i].isDirectory) { nextIdx = i; break; }
            }
            if (nextIdx >= 0) PlayFileByIndex(nextIdx);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next file");

    // Time display on title bar
    if (hasFile) {
        ImGui::SameLine();
        double curSec = (double)s_currentFrame / 59.98;
        double totSec = (double)s_totalEstimatedFrames / 59.98;
        int curMin = (int)curSec / 60; int curSecI = (int)curSec % 60;
        int totMin = (int)totSec / 60; int totSecI = (int)totSec % 60;
        char timeStr[64];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d", curMin, curSecI, totMin, totSecI);
        ImGui::TextDisabled("%s", timeStr);
    }

    // ===== Expanded player controls =====
    if (playerOpen) {
        // File info
        if (hasFile) {
            size_t lastSlash = s_selectedFilePath.find_last_of("\\/");
            std::string fname = (lastSlash != std::string::npos)
                ? s_selectedFilePath.substr(lastSlash + 1) : s_selectedFilePath;
            if (fname.length() > 40) fname = "..." + fname.substr(fname.length() - 37);
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", fname.c_str());
        } else {
            ImGui::TextDisabled("[No file loaded]");
        }

        ImGui::Spacing();

        // Transport buttons: Play / Pause / Stop
        float btnWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
        if (isPaused) {
            if (ImGui::Button("Resume", ImVec2(btnWidth, 30))) {
                Resume();
            }
        } else if (hasFile && !s_musicPlaying) {
            if (ImGui::Button("Play", ImVec2(btnWidth, 30))) {
                PlaySelected();
            }
        } else {
            ImGui::Button("Play", ImVec2(btnWidth, 30));
        }
        ImGui::SameLine();
        if (isPlaying) {
            if (ImGui::Button("Pause", ImVec2(btnWidth, 30))) {
                Pause();
            }
        } else {
            ImGui::Button("Pause", ImVec2(btnWidth, 30));
        }
        ImGui::SameLine();
        if (s_musicPlaying) {
            if (ImGui::Button("Stop", ImVec2(btnWidth, 30))) {
                StopPlayback();
            }
        } else {
            ImGui::Button("Stop", ImVec2(btnWidth, 30));
        }

        ImGui::Spacing();

        // Prev / Next
        float navWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
        if (ImGui::Button("<< Prev", ImVec2(navWidth, 25))) {
            if (!s_fileList.empty()) {
                int prevIdx = -1;
                for (int i = s_selectedFileIdx - 1; i >= 0; i--) {
                    if (!s_fileList[i].isDirectory) { prevIdx = i; break; }
                }
                if (prevIdx >= 0) PlayFileByIndex(prevIdx);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>", ImVec2(navWidth, 25))) {
            if (!s_fileList.empty()) {
                int nextIdx = -1;
                for (int i = s_selectedFileIdx + 1; i < (int)s_fileList.size(); i++) {
                    if (!s_fileList[i].isDirectory) { nextIdx = i; break; }
                }
                if (nextIdx >= 0) PlayFileByIndex(nextIdx);
            }
        }

        ImGui::Spacing();

        // Speed slider (inline)
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("Speed##gtplayer", &s_playbackSpeed, 0.1f, 4.0f, "%.1fx")) {
            s_rcRatioNeedsUpdate = true;
            SaveConfig();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Segments: %u", s_parsedMusicData.actual_segment_count);

        // Progress bar with seek
        if (hasFile) {
            float progress = 0.0f;
            if (s_totalEstimatedFrames > 0) {
                progress = (float)s_currentFrame / (float)s_totalEstimatedFrames;
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
            }

            double curSec = (double)s_currentFrame / 59.98;
            double totSec = (double)s_totalEstimatedFrames / 59.98;
            int curMin = (int)curSec / 60; int curSecI = (int)curSec % 60;
            int totMin = (int)totSec / 60; int totSecI = (int)totSec % 60;
            char curStr[32], totStr[32];
            snprintf(curStr, sizeof(curStr), "%02d:%02d", curMin, curSecI);
            snprintf(totStr, sizeof(totStr), "%02d:%02d", totMin, totSecI);
            ImGui::Text("%s / %s", curStr, totStr);

            // Slider for seeking — only seek on mouse release
            static float seek_progress = 0.0f;
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
            ImGui::SliderFloat("##GTProgress", &seek_progress, 0.0f, 1.0f, "");
            ImGui::PopStyleColor();
            if (ImGui::IsItemActive()) {
                // Dragging: show preview time but don't seek
                double previewSec = seek_progress * (double)s_totalEstimatedFrames / 59.98;
                int pMin = (int)previewSec / 60; int pSec = (int)previewSec % 60;
                ImGui::SetTooltip("Jump to %02d:%02d", pMin, pSec);
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
                // Mouse released: perform actual seek
                if (s_totalEstimatedFrames > 0 && s_musicPlaying) {
                    unsigned long targetFrame = (unsigned long)(seek_progress * s_totalEstimatedFrames);
                    SeekToFrame(targetFrame);
                }
            } else {
                // Idle: sync slider to current position
                seek_progress = progress;
            }
        } else {
            ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
        }

        ImGui::Spacing();
        ImGui::Separator();
    }

    // ===== File browser navigation buttons =====
    // Navigation buttons
    if (ImGui::Button("<", ImVec2(25, 0))) NavBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(25, 0))) NavForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^", ImVec2(25, 0))) NavToParent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");
    ImGui::SameLine();

    // Breadcrumb path bar
    if (!s_pathEditMode) {
        float availWidth = ImGui::GetContentRegionAvail().x;
        std::vector<std::string> segments = MidiPlayer::SplitPath(s_musicDir);
        std::vector<float> buttonWidths;
        std::vector<std::string> accumulatedPaths;
        std::string accumulatedPath;
        ImGuiStyle& style = ImGui::GetStyle();
        float framePaddingX = style.FramePadding.x;
        float itemSpacingX = style.ItemSpacing.x;
        float buttonBorderSize = style.FrameBorderSize;
        for (size_t i = 0; i < segments.size(); i++) {
            if (i == 0) accumulatedPath = segments[i];
            else {
                if (accumulatedPath.back() != '\\') accumulatedPath += "\\";
                accumulatedPath += segments[i];
            }
            accumulatedPaths.push_back(accumulatedPath);
            ImVec2 textSize = ImGui::CalcTextSize(segments[i].c_str());
            float bw = textSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
            buttonWidths.push_back(bw);
        }
        ImVec2 separatorTextSize = ImGui::CalcTextSize(">");
        float separatorWidth = separatorTextSize.x + itemSpacingX * 2.0f;
        ImVec2 ellipsisTextSize = ImGui::CalcTextSize("...");
        float ellipsisButtonWidth = ellipsisTextSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
        float ellipsisWidth = ellipsisButtonWidth + separatorWidth;
        float safeAvailWidth = availWidth - 10.0f;
        int firstVisibleSegment = (int)segments.size() - 1;
        float usedWidth = (segments.size() > 0) ? buttonWidths.back() : 0.0f;
        for (int i = (int)segments.size() - 2; i >= 0; i--) {
            float segmentWidth = buttonWidths[i] + separatorWidth;
            float neededEllipsis = (i > 0) ? ellipsisWidth : 0.0f;
            if (usedWidth + segmentWidth + neededEllipsis > safeAvailWidth) break;
            else { usedWidth += segmentWidth; firstVisibleSegment = i; }
        }
        ImVec2 barStartPos = ImGui::GetCursorScreenPos();
        float barHeight = ImGui::GetFrameHeight();
        ImGui::BeginGroup();
        if (firstVisibleSegment > 0) {
            if (ImGui::Button("...##gtellipsis")) {
                s_pathEditMode = true;
                s_pathEditModeJustActivated = true;
                strncpy(s_pathInput, s_musicDir, MAX_PATH);
            }
            ImGui::SameLine();
            ImGui::Text(">"); ImGui::SameLine();
        }
        for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
            std::string btnId = segments[i] + "##gtseg" + std::to_string(i);
            if (ImGui::Button(btnId.c_str())) {
                NavigateTo(accumulatedPaths[i].c_str());
            }
            if (i < (int)segments.size() - 1) {
                ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine();
            }
        }
        ImGui::EndGroup();
        ImVec2 barEndPos = ImGui::GetItemRectMax();
        float emptySpaceWidth = barStartPos.x + availWidth - barEndPos.x;
        if (emptySpaceWidth > 0) {
            ImGui::SameLine();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(barEndPos.x, barStartPos.y),
                ImVec2(barEndPos.x + emptySpaceWidth, barEndPos.y),
                ImGui::GetColorU32(ImGuiCol_FrameBg));
            ImGui::InvisibleButton("##gtPathEmpty", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) {
                s_pathEditMode = true;
                s_pathEditModeJustActivated = true;
                strncpy(s_pathInput, s_musicDir, MAX_PATH);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##PathInputGt", s_pathInput, MAX_PATH,
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            NavigateTo(s_pathInput);
            s_pathEditMode = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_pathEditMode = false;
            s_pathEditModeJustActivated = false;
            strncpy(s_pathInput, s_musicDir, MAX_PATH);
        } else if (!s_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            s_pathEditMode = false;
            strncpy(s_pathInput, s_musicDir, MAX_PATH);
        }
        if (s_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            s_pathEditModeJustActivated = false;
        }
    }

    // Folder history dropdown
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##gtHist", "GT History", ImGuiComboFlags_HeightLarge)) {
        if (s_folderHistory.empty()) {
            ImGui::TextDisabled("(no history)");
        } else {
            for (int i = 0; i < (int)s_folderHistory.size(); i++) {
                size_t lastSlash = s_folderHistory[i].find_last_of("\\/");
                std::string folderName = (lastSlash != std::string::npos)
                    ? s_folderHistory[i].substr(lastSlash + 1) : s_folderHistory[i];
                if (ImGui::Selectable(folderName.c_str(), false))
                    NavigateTo(s_folderHistory[i].c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", s_folderHistory[i].c_str());
            }
        }
        ImGui::EndCombo();
    }

    // File list
    ImGui::BeginChild("GtFileList", ImVec2(-1, 0), true);

    for (int i = 0; i < (int)s_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = s_fileList[i];
        bool isSelected = (s_selectedFileIdx == i);
        bool isPlaying = (entry.fullPath == s_selectedFilePath && s_musicPlaying);

        std::string label;
        if (entry.name == "..") {
            label = "[UP] " + entry.name;
        } else if (entry.isDirectory) {
            label = "[DIR] " + entry.name;
        } else {
            label = entry.name;
        }

        // Highlight .gbas.c files in green
        bool isGbasC = false;
        if (!entry.isDirectory && entry.name.length() > 7 &&
            entry.name.substr(entry.name.length() - 7) == ".gbas.c") {
            isGbasC = true;
        }

        if (isPlaying) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
        else if (isGbasC) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.4f, 1.0f));

        bool selected = false;
        if (ImGui::Selectable(label.c_str(), isSelected)) {
            s_selectedFileIdx = i;
            if (entry.name == "..") {
                NavToParent();
            } else if (entry.isDirectory) {
                NavigateTo(entry.fullPath.c_str());
            } else {
                // Load and play the selected file
                s_selectedFileIdx = i;
                PlaySelected();
            }
        }

        // Double-click: enter directory or play
        if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsItemHovered()) {
            if (entry.isDirectory && entry.name != "..") {
                NavigateTo(entry.fullPath.c_str());
            } else if (!entry.isDirectory) {
                s_selectedFileIdx = i;
                PlaySelected();
            }
        }

        if (isPlaying || isGbasC) ImGui::PopStyleColor();

        // Tooltip with full path
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", entry.fullPath.c_str());
        }
    }

    ImGui::EndChild();
}

// ============ RenderLogPanel ============
static void RenderLogPanel() {
    // ===== GT Debug Log =====
    if (ImGui::CollapsingHeader("GT Debug Log##gtlog", nullptr, 0)) {
        ImGui::Checkbox("Auto-scroll##gtlog", &s_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##gtlog")) {
            s_log.clear();
            s_logDisplay[0] = '\0';
            s_logLastSize = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All##gtlog")) {
            ImGui::SetClipboardText(s_log.c_str());
        }
        size_t copyLen = s_log.size() < sizeof(s_logDisplay) - 1
            ? s_log.size() : sizeof(s_logDisplay) - 1;
        memcpy(s_logDisplay, s_log.c_str(), copyLen);
        s_logDisplay[copyLen] = '\0';
        bool changed = (s_log.size() != s_logLastSize);
        s_logLastSize = s_log.size();
        if (s_logAutoScroll && changed) s_logScrollToBottom = true;
        float logH = 150;
        ImGui::BeginChild("GtDebugLogRegion", ImVec2(0, logH), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 tsz = ImGui::CalcTextSize(s_logDisplay, NULL, false, -1.0f);
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float minH = ImGui::GetContentRegionAvail().y;
        float inH = (tsz.y > minH) ? tsz.y + lineH * 2 : minH;
        ImGui::InputTextMultiline("##GtLogText", s_logDisplay, sizeof(s_logDisplay),
            ImVec2(-1, inH), ImGuiInputTextFlags_ReadOnly);
        if (s_logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            s_logScrollToBottom = false;
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // ===== GT Folder History =====
    bool prevHistoryCollapsed = s_historyCollapsed;
    ImGui::SetNextItemAllowOverlap();
    bool historyOpen = ImGui::TreeNodeEx("GT Folder History##gthist",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_historyCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
    if (historyOpen) s_historyCollapsed = false;
    else s_historyCollapsed = true;
    if (s_historyCollapsed != prevHistoryCollapsed) SaveConfig();

    // Title bar mini controls
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Clear##GtHistClear")) {
        s_folderHistory.clear();
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All");
    ImGui::SameLine();
    if (s_histSortMode == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Time##gtHistSortTime")) { s_histSortMode = 0; }
    if (s_histSortMode == 0) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by time");
    ImGui::SameLine();
    if (s_histSortMode == 1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Freq##gtHistSortFreq")) { s_histSortMode = 1; }
    if (s_histSortMode == 1) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##gtHistFilter", "Filter...", s_histFilter, sizeof(s_histFilter));

    if (historyOpen) {
        // Build sorted/filtered/deduplicated list
        struct HistEntry {
            std::string name;
            int idx;
            int fileCount;
        };
        std::vector<HistEntry> entries;
        std::vector<std::string> seen;

        for (int i = 0; i < (int)s_folderHistory.size(); i++) {
            size_t lastSlash = s_folderHistory[i].find_last_of("\\/");
            std::string folderName = (lastSlash != std::string::npos)
                ? s_folderHistory[i].substr(lastSlash + 1) : s_folderHistory[i];

            // Apply filter
            if (s_histFilter[0] != '\0') {
                std::string lowerName = folderName;
                std::string lowerFilter = s_histFilter;
                for (size_t c = 0; c < lowerName.size(); c++) lowerName[c] = tolower(lowerName[c]);
                for (size_t c = 0; c < lowerFilter.size(); c++) lowerFilter[c] = tolower(lowerFilter[c]);
                if (lowerName.find(lowerFilter) == std::string::npos) continue;
            }

            // Deduplicate
            bool already = false;
            for (size_t s = 0; s < seen.size(); s++) {
                if (seen[s] == folderName) { already = true; break; }
            }
            if (already) continue;
            seen.push_back(folderName);

            // Count .c files in directory
            int fileCount = 0;
            std::wstring wSearchPath = MidiPlayer::UTF8ToWide(s_folderHistory[i]) + L"\\*.c";
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    fileCount++;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            entries.push_back({folderName, i, fileCount});
        }

        // Sort by file count if selected
        if (s_histSortMode == 1) {
            std::sort(entries.begin(), entries.end(), [](const HistEntry& a, const HistEntry& b) {
                return a.fileCount > b.fileCount;
            });
        }

        ImGui::Separator();
        float historyHeight = ImGui::GetContentRegionAvail().y - 5;
        ImGui::BeginChild("GtHistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

        if (entries.empty()) {
            ImGui::TextDisabled("No matching folders...");
        } else {
            for (size_t ei = 0; ei < entries.size(); ei++) {
                const HistEntry& e = entries[ei];
                const std::string& path = s_folderHistory[e.idx];
                ImGui::PushID(e.idx);
                char label[512];
                if (e.fileCount > 0)
                    snprintf(label, sizeof(label), "[%d] %s", e.fileCount, e.name.c_str());
                else
                    snprintf(label, sizeof(label), "[DIR] %s", e.name.c_str());
                if (ImGui::Selectable(label, false))
                    NavigateTo(path.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.c_str());
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Remove from history")) {
                        s_folderHistory.erase(s_folderHistory.begin() + e.idx);
                        SaveConfig();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }

        ImGui::EndChild();
    }
}

// ============ RenderScopeSettingsWindow ============
static void RenderScopeSettingsWindow() {
    if (!s_showScopeSettingsWindow) return;

    ImGui::SetNextWindowSize(ImVec2(540, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GT Scope Settings", &s_showScopeSettingsWindow)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Gigatron Scope Settings");
    ImGui::SameLine();
    ImGui::TextDisabled("(4 channels shared)");

    ImGui::Spacing();

    // ===== Global trigger / waveform settings =====
    if (ImGui::BeginTable("##gtscopetrigger", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Samples
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Samples");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(220);
        int maxSamples = GT_SCOPE_BUF_SIZE - s_scopeSearchWindow - 64;
        if (maxSamples < 16) maxSamples = 16;
        if (s_scopeSamples > maxSamples) s_scopeSamples = maxSamples;
        if (ImGui::SliderInt("##gtsamples", &s_scopeSamples, 16, maxSamples, "%d")) {
            SaveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of waveform samples to display.");

        // Offset
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Offset");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("##gtoffset", &s_scopeOffset, 0, GT_SCOPE_BUF_SIZE - 1, "%d")) {
            SaveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Manual buffer read offset. Shifts the waveform position\nafter correlation. Use to fine-tune the displayed phase.");

        // Search Window
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Search Window");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("##gtsearchwin", &s_scopeSearchWindow, 0, GT_SCOPE_BUF_SIZE / 2, "%d")) {
            SaveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cross-correlation search range. 0 = disable correlation (raw buffer).");

        // Edge Align
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Edge Align");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Checkbox("Enable##gtedgealign", &s_scopeEdgeAlign)) {
            SaveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap to nearest rising edge after correlation.\nStabilizes waveform display.");

        // AC Mode (shared, radio-style)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("AC Coupling");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::RadioButton("Off##ac0", s_scopeAcMode == 0)) { s_scopeAcMode = 0; SaveConfig(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Center##ac1", s_scopeAcMode == 1)) { s_scopeAcMode = 1; SaveConfig(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Bottom##ac2", s_scopeAcMode == 2)) { s_scopeAcMode = 2; SaveConfig(); }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("AC coupling mode (applied to all 4 channels):\n"
                "Off = raw waveform\nCenter = subtract DC (unipolar only)\n"
                "Bottom = map min..max to full range (unipolar only)");
        }

        // Scope Height
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Scope Height");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("##scopeh", &s_scopeHeight, 30.0f, 500.0f, "%.0f px")) {
            SaveConfig();
        }

        // Show Scope
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Show Scope");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Checkbox("Enable##gtshowscope", &s_showScope)) {
            SaveConfig();
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Per-channel settings =====
    if (ImGui::BeginTable("##gtscopesettings", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int ch = 0; ch < 4; ch++) {
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(kChColors[ch]);

            // Width
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(col, "Ch%d Width", ch);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderFloat(("##scopew" + std::to_string(ch)).c_str(),
                    &s_scopeWidth[ch], 40.0f, 600.0f, "%.0f px")) {
                SaveConfig();
            }

            // Amplitude
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(col, "Ch%d Amplitude", ch);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderFloat(("##scopea" + std::to_string(ch)).c_str(),
                    &s_scopeAmp[ch], 0.1f, 10.0f, "%.1f")) {
                SaveConfig();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Vertical amplitude multiplier. Higher = taller waveform.");
            }

            // Color
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(col, "Ch%d Color", ch);
            ImGui::TableSetColumnIndex(1);
            float c[4] = {
                ((kChColors[ch] >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                ((kChColors[ch] >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                ((kChColors[ch] >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                ((kChColors[ch] >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f
            };
            if (ImGui::ColorEdit4(("##scopec" + std::to_string(ch)).c_str(), c,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                kChColors[ch] = IM_COL32(
                    (int)(c[0] * 255), (int)(c[1] * 255),
                    (int)(c[2] * 255), (int)(c[3] * 255));
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();

    // Reset
    if (ImGui::Button("Reset All to Defaults##gtreset")) {
        for (int ch = 0; ch < 4; ch++) {
            s_scopeWidth[ch] = 220.0f;
            s_scopeAmp[ch] = 1.0f;
            s_scopePersistOffset[ch] = 0;
        }
        s_scopeSamples = 512;
        s_scopeSearchWindow = 256;
        s_scopeOffset = 0;
        s_scopeEdgeAlign = true;
        s_scopeAcMode = 1;
        s_scopeHeight = 200.0f;
        s_showScope = false;
        kChColors[0] = IM_COL32(80, 220, 80, 255);
        kChColors[1] = IM_COL32(80, 140, 255, 255);
        kChColors[2] = IM_COL32(255, 80, 80, 255);
        kChColors[3] = IM_COL32(255, 180, 60, 255);
        SaveConfig();
    }

    ImGui::End();
}

// ============ Wave Editor Window ============
static void SaveWaveTableFile() {
    char wtabPath[MAX_PATH];
    GetExeDir(wtabPath, MAX_PATH);
    snprintf(wtabPath + strlen(wtabPath), MAX_PATH - strlen(wtabPath), "\\gigatron_custom_wave.wtab");
    FILE* f = fopen(wtabPath, "wb");
    if (f) {
        fwrite(&s_gtState.waveTableBits, 1, 1, f);
        fwrite(s_gtState.customWaveTable, sizeof(uint16_t), 256, f);
        fclose(f);
        GtLog("[GT] Custom wave table saved (bits=%d)\n", s_gtState.waveTableBits);
    }
}

static void CopyWaveFromEditor() {
    // Copy working buffer back to customWaveTable at current wave type
    for (int i = 0; i < 64; i++) {
        s_gtState.customWaveTable[i * 4 + s_waveEditType] = s_waveEditBuf[i];
    }
    s_waveEditBufDirty = false;
    SaveWaveTableFile();
}

static void CopyWaveToEditor() {
    // Load current wave type from customWaveTable into working buffer
    for (int i = 0; i < 64; i++) {
        s_waveEditBuf[i] = s_gtState.customWaveTable[i * 4 + s_waveEditType];
    }
    s_waveEditBufDirty = false;
}

static void RenderWaveEditorWindow() {
    if (!s_showWaveEditorWindow) return;

    ImGui::SetNextWindowSize(ImVec2(520, 580), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Gigatron Wave Editor##gtwaveedit", &s_showWaveEditorWindow)) {
        ImGui::End();
        return;
    }

    // ===== Enable toggle =====
    if (ImGui::Checkbox("Enable Custom Wave Table##gtcustom", &s_useCustomWaveTable)) {
        s_gtState.useCustomWaveTable = s_useCustomWaveTable;
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, the emulator uses the custom high-precision wave table\n"
                          "instead of the original 6-bit soundTable.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Wave table bit depth =====
    ImGui::TextDisabled("Wave Table Bit Depth:");
    int bits[] = {6, 8, 12, 16};
    const char* bitsLabels[] = {"6-bit", "8-bit", "12-bit", "16-bit"};
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(bitsLabels[i], s_waveTableBits == bits[i])) {
            s_waveTableBits = bits[i];
            s_gtState.waveTableBits = (uint8_t)bits[i];
            gigatron_emu_reset_custom_wave_table(&s_gtState, (uint8_t)bits[i]);
            CopyWaveToEditor();
            SaveWaveTableFile();
            SaveConfig();
            GtLog("[GT] Wave table bit depth changed to %d-bit\n", bits[i]);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Higher bit depth = more waveform precision = better audio quality.\n"
                          "16-bit gives near-CD quality waveform data.");
    }

    // Show current max value
    uint16_t maxVal = (s_waveTableBits == 6) ? 63 : (s_waveTableBits == 8) ? 255 :
                      (s_waveTableBits == 12) ? 4095 : 65535;
    ImGui::SameLine();
    ImGui::TextDisabled("(max: %u)", maxVal);

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Wave type selection =====
    ImGui::TextDisabled("Edit Wave Type:");
    const char* waveNames[] = {"Noise", "Triangle", "Pulse", "Sawtooth"};
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,
            (s_waveEditType == i) ? ImVec4(0.2f, 0.5f, 0.9f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        if (ImGui::SmallButton(waveNames[i])) {
            if (s_waveEditBufDirty) CopyWaveFromEditor();
            s_waveEditType = i;
            CopyWaveToEditor();
        }
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // ===== Waveform canvas (64 points, mouse drawable) =====
    if (s_waveEditBufDirty) CopyWaveFromEditor();

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize(480, 200);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                      IM_COL32(20, 20, 30, 255));

    // Grid lines
    for (int i = 0; i <= 4; i++) {
        float y = canvasPos.y + (float)i / 4.0f * canvasSize.y;
        dl->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y),
                     IM_COL32(50, 50, 60, 255));
    }
    for (int i = 0; i <= 16; i++) {
        float x = canvasPos.x + (float)i / 16.0f * canvasSize.x;
        dl->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y),
                     IM_COL32(50, 50, 60, 255));
    }

    // Draw waveform
    for (int i = 0; i < 64; i++) {
        float x1 = canvasPos.x + (float)i / 63.0f * canvasSize.x;
        float x2 = canvasPos.x + (float)(i + 1) / 63.0f * canvasSize.x;
        float y1 = canvasPos.y + (1.0f - (float)s_waveEditBuf[i] / (float)maxVal) * canvasSize.y;
        float y2 = canvasPos.y + (1.0f - (float)s_waveEditBuf[(i + 1) % 64] / (float)maxVal) * canvasSize.y;
        dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(0, 200, 255, 255), 2.0f);
        // Dot at each point
        dl->AddCircleFilled(ImVec2(x1, y1), 3.0f, IM_COL32(255, 220, 80, 255));
    }

    // Invisible button for mouse interaction
    ImGui::InvisibleButton("##wavecanvas", canvasSize);
    bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            s_waveDrawing = true;
        }
        if (s_waveDrawing && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            float relX = (mousePos.x - canvasPos.x) / canvasSize.x;
            float relY = (mousePos.y - canvasPos.y) / canvasSize.y;
            if (relX >= 0.0f && relX <= 1.0f && relY >= 0.0f && relY <= 1.0f) {
                int idx = (int)(relX * 63.0f + 0.5f);
                if (idx < 0) idx = 0;
                if (idx > 63) idx = 63;
                uint16_t val = (uint16_t)((1.0f - relY) * maxVal + 0.5f);
                if (val > maxVal) val = maxVal;
                s_waveEditBuf[idx] = val;
                s_waveEditBufDirty = true;
            }
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (s_waveDrawing) {
            s_waveDrawing = false;
            CopyWaveFromEditor();
        }
    }

    // Border
    dl->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                IM_COL32(100, 100, 120, 255));

    ImGui::Spacing();

    // ===== Value display =====
    ImGui::TextDisabled("Current wave: %s | Bits: %d | Drag mouse to draw",
                        waveNames[s_waveEditType], s_waveTableBits);

    ImGui::Spacing();
    ImGui::Separator();

    // ===== Preset buttons =====
    ImGui::TextDisabled("Presets:");
    float btnW = 110.0f;
    if (ImGui::Button("Reset Default##gtpreset1", ImVec2(btnW, 0))) {
        gigatron_emu_reset_custom_wave_table(&s_gtState, (uint8_t)s_waveTableBits);
        CopyWaveToEditor();
        SaveWaveTableFile();
    }
    ImGui::SameLine();
    if (ImGui::Button("Flat (Zero)##gtpreset2", ImVec2(btnW, 0))) {
        memset(s_waveEditBuf, 0, sizeof(s_waveEditBuf));
        CopyWaveFromEditor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Full Level##gtpreset3", ImVec2(btnW, 0))) {
        for (int i = 0; i < 64; i++) s_waveEditBuf[i] = maxVal;
        CopyWaveFromEditor();
    }
    if (ImGui::Button("Sine Wave##gtpreset4", ImVec2(btnW, 0))) {
        for (int i = 0; i < 64; i++)
            s_waveEditBuf[i] = (uint16_t)((sin((double)i / 64.0 * 2.0 * 3.14159265) * 0.5 + 0.5) * maxVal);
        CopyWaveFromEditor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Square 50%##gtpreset5", ImVec2(btnW, 0))) {
        for (int i = 0; i < 64; i++)
            s_waveEditBuf[i] = (i < 32) ? 0 : maxVal;
        CopyWaveFromEditor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Sawtooth##gtpreset6", ImVec2(btnW, 0))) {
        for (int i = 0; i < 64; i++)
            s_waveEditBuf[i] = (uint16_t)((float)i / 63.0f * maxVal);
        CopyWaveFromEditor();
    }
    if (ImGui::Button("Random##gtpreset7", ImVec2(btnW, 0))) {
        srand((unsigned int)GetTickCount());
        for (int i = 0; i < 64; i++)
            s_waveEditBuf[i] = (uint16_t)(rand() % (maxVal + 1));
        CopyWaveFromEditor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Triangle##gtpreset8", ImVec2(btnW, 0))) {
        for (int i = 0; i < 32; i++)
            s_waveEditBuf[i] = (uint16_t)((float)i / 31.0f * maxVal);
        for (int i = 32; i < 64; i++)
            s_waveEditBuf[i] = (uint16_t)((float)(63 - i) / 31.0f * maxVal);
        CopyWaveFromEditor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Inverse Saw##gtpreset9", ImVec2(btnW, 0))) {
        for (int i = 0; i < 64; i++)
            s_waveEditBuf[i] = (uint16_t)((1.0f - (float)i / 63.0f) * maxVal);
        CopyWaveFromEditor();
    }

    ImGui::Spacing();

    // ===== Apply All Types =====
    if (ImGui::Button("Apply Current to All Types##gtapplyall")) {
        for (int t = 0; t < 4; t++) {
            for (int i = 0; i < 64; i++)
                s_gtState.customWaveTable[i * 4 + t] = s_waveEditBuf[i];
        }
        SaveWaveTableFile();
        GtLog("[GT] Current wave applied to all 4 types\n");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy the current wave shape to all 4 wave types (Noise/Tri/Pulse/Saw).");
    }

    ImGui::End();
}

// ============ Public API ============
void Init() {
    LoadConfig();
    QueryPerformanceFrequency(&s_perfFrequency);
    QueryPerformanceCounter(&s_lastPerfCounter);

    // Init emulator state
    gigatron_emu_init(&s_gtState);
    s_gtState.audioSampleRate = GT_SAMPLE_RATE;
    s_gtState.audio_bit_depth = (uint8_t)s_audioBitDepth;
    s_gtState.dc_offset_removal_enabled = s_dcOffsetRemoval;
    s_gtState.dc_bias = 0.0;
    s_gtState.dc_alpha = 0.99;
    s_gtState.volume_scale = s_volumeScale;

    // Wave editor: load custom wave table from file or generate default
    s_gtState.useCustomWaveTable = s_useCustomWaveTable;
    s_gtState.waveTableBits = (uint8_t)s_waveTableBits;
    char wtabPath[MAX_PATH];
    GetExeDir(wtabPath, MAX_PATH);
    snprintf(wtabPath + strlen(wtabPath), MAX_PATH - strlen(wtabPath), "\\gigatron_custom_wave.wtab");
    FILE* f = fopen(wtabPath, "rb");
    if (f) {
        uint8_t fileBits;
        if (fread(&fileBits, 1, 1, f) == 1 && (fileBits == 6 || fileBits == 8 || fileBits == 12 || fileBits == 16)) {
            s_waveTableBits = fileBits;
            s_gtState.waveTableBits = fileBits;
            fread(s_gtState.customWaveTable, sizeof(uint16_t), 256, f);
        }
        fclose(f);
        GtLog("[GT] Custom wave table loaded from file (bits=%d)\n", s_waveTableBits);
    } else {
        gigatron_emu_reset_custom_wave_table(&s_gtState, (uint8_t)s_waveTableBits);
    }
    // Initialize working buffer to current wave type
    for (int i = 0; i < 64; i++)
        s_waveEditBuf[i] = s_gtState.customWaveTable[i * 4 + s_waveEditType];

    // Init timeline
    InitEventTimeline(&s_timeline);
    InitParsedMusicData(&s_parsedMusicData);

    // Set initial directory
    if (s_musicDir[0] != '\0') {
        RefreshFileList();
    } else {
        // Default to exe directory
        GetExeDir(s_musicDir, MAX_PATH);
        RefreshFileList();
    }

    // Seed nav history with initial directory
    if (s_navHistory.empty()) {
        s_navHistory.push_back(std::string(s_musicDir));
        s_navPos = 0;
    }

    GtLog("[GT] Initialized. Sample rate: %d, Buffer: %d samples\n",
          GT_SAMPLE_RATE, GT_BUFFER_SAMPLES);
}

void Shutdown() {
    StopPlayback();
    SaveConfig();
    FreeEventTimeline(&s_timeline);
    FreeParsedMusicData(&s_parsedMusicData);
    s_folderHistory.clear();
    s_navHistory.clear();
    s_fileList.clear();
    s_log.clear();
    GtLog("[GT] Shutdown complete\n");
}

bool IsPlaying() {
    return s_musicPlaying;
}

void Pause() {
    if (s_musicPlaying && !s_musicPaused) {
        s_musicPaused = true;
        s_audioOutput->close();
    }
}

void Resume() {
    if (s_musicPlaying && s_musicPaused) {
        s_musicPaused = false;
        if (s_audioOutput->init(GT_SAMPLE_RATE) == 0) {
            QueryPerformanceCounter(&s_lastPerfCounter);
            s_samplesDue = 0.0;
            s_rcRatioNeedsUpdate = true;
        }
    }
}

bool WantsKeyboardCapture() {
    return s_pathEditMode;
}

}  // namespace GigatronWindow
