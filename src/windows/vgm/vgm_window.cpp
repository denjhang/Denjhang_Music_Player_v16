// vgm_window.cpp - libvgm Simulation Playback Window
// Independent file browser and folder history (separate from MIDI player).
// Supports all libvgm-supported formats: VGM/VGZ, S98, GYM, DRO.

#include "vgm_window.h"
#include "vgm_parser.h"
#include "core/gui_renderer.h"
#include "midi/midi_player.h"
#include "core/modizer_viz.h"
#include "libvgm-modizer/emu/cores/ModizerVoicesData.h"

#include "imgui/imgui.h"

#include "libvgm-modizer/emu/SoundDevs.h"
#include "libvgm-modizer/emu/EmuCores.h"
#include "libvgm-modizer/chip_params.h"
#include "libvgm-modizer/player/vgmplayer.hpp"
#include "libvgm-modizer/player/s98player.hpp"
#include "libvgm-modizer/player/gymplayer.hpp"
#include "libvgm-modizer/player/droplayer.hpp"
#include "libvgm-modizer/player/playera.hpp"
#include "libvgm-modizer/audio/AudioStream.h"
#include "libvgm-modizer/audio/AudioStream_SpcDrvFuns.h"
#include "libvgm-modizer/player/playerbase.hpp"
#include "libvgm-modizer/utils/DataLoader.h"
#include "libvgm-modizer/utils/FileLoader.h"

#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <map>
#include <set>
#include <unordered_map>

#define AUDDRV_DSOUND 1

namespace VgmWindow {

// ===== Public state =====
bool g_windowOpen  = false;
bool g_initialized = false;

// ===== Oscilloscope =====
static ModizerViz s_scope;
static bool s_showScope = false;
static bool s_showScopeSettingsWindow = false;
static float s_scopeHeight = 60.0f; // Oscilloscope height in pixels, unified
static float s_scopeBackgroundHeight = 200.0f; // Height of oscilloscope background area (splitter bar position)

// ===== Scope rendering: per-chip settings in ScopeChipSettings =====

struct ScopeChipSettings {
    int samples = 441;
    int offset = 0;
    int search_window = 735;
    bool edge_align = true;
    int ac_mode = 1;  // 0=off, 1=center, 2=bottom
    float width = 90.0f;     // per-chip scope width
    float amplitude = 3.0f;  // amplitude multiplier
    bool legacy_mode = false;  // true = clear waveform when no new data (for PCM chips)
    ImU32 channel_colors[32] = {};  // per-channel color override; 0=use default shiftHue
};
static std::map<UINT8, ScopeChipSettings> s_scopeChipSettings; // key = devType

// ===== Internal VGM debug log =====
static std::string s_vgmLog;
static char  s_vgmLogDisplay[32768] = {};
static size_t s_vgmLogLastSize = 0;
static bool  s_vgmLogAutoScroll = true;
static bool  s_vgmLogScrollToBottom = false;
static bool  s_vgmHistoryCollapsed = false;
static bool  s_vgmPlayerCollapsed = false;
static bool  s_vgmBrowserCollapsed = false;
static bool  s_vgmInlinePlayerCollapsed = false;
static void VgmLog(const char* fmt, ...) {
    char buf[256];
    va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    s_vgmLog += buf;
    if (s_vgmLog.size() > sizeof(s_vgmLogDisplay) - 1)
        s_vgmLog.erase(0, s_vgmLog.size() - (sizeof(s_vgmLogDisplay) - 1));
    s_vgmLogScrollToBottom = true;
}


// ===== Persistence filenames =====
static const char* k_configFile = "vgm_config.ini";

// User-defined chip abbreviation overrides: chipLabel -> custom abbrev
static std::map<std::string, std::string> s_chipAliases;

static void GetExeDir(char* out, int maxLen) {
    GetModuleFileNameA(NULL, out, maxLen);
    char* slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
}

// ===== Internal audio state =====
static PlayerA     s_player;
static VGMFile     s_vgmFile;
static UINT32      s_audDrvId   = (UINT32)-1;
static void*       s_audDrv     = nullptr;
static std::atomic<UINT8> s_playState{0};
static int         s_loopCount  = 2;
static UINT32      s_sampleRate = 44100;
static std::string s_loadedPath;
static bool        s_fileLoaded = false;

#define VGM_PLAY  0x01
#define VGM_PAUSE 0x02

// ===== Independent file browser state =====
static char  s_currentPath[MAX_PATH] = ".";
static char  s_pathInput[MAX_PATH]   = {};
static bool  s_pathEditMode          = false;
static bool  s_pathEditModeJustActivated = false;
static std::vector<std::string> s_navHistory;   // navigation back/forward
static int   s_navPos                = -1;
static bool  s_navigating            = false;
static std::vector<std::string> s_folderHistory; // recently visited folders (persisted)
static char  s_histFilter[128] = "";
static char  s_fileBrowserFilter[256] = "";
static int   s_histSortMode = 0;  // 0=time (insert order), 1=frequency
static std::vector<MidiPlayer::FileEntry> s_fileList;
static int   s_selectedFileIndex     = -1;
static int   s_hoveredFileIndex      = -1;
static std::string s_lastExitedFolder;
static std::string s_currentPlayingFilePath;
static std::map<std::string, float> s_pathScrollPositions;
static std::map<int, MidiPlayer::TextScrollState> s_textScrollStates;
static std::map<std::string, MidiPlayer::TextScrollState> s_tagScrollStates;
// Smooth auto-scroll for file browser
static float s_scrollAnimY = 0.0f;         // current animated scroll position
static int   s_lastTrackedIndex = -1;      // last tracked file index
static std::string s_trackedFolderPath;    // folder path being tracked
static bool  s_autoScrollActive = false;   // auto-scroll currently animating
static std::vector<std::string> s_playlist;      // vgm files in current dir (for sequential play)
static int   s_playlistIndex         = -1;
static bool  s_autoPlayNext          = true;
static bool  s_isSequentialPlayback  = true;
static float s_masterVolume          = 1.0f;  // 0.0 - 2.0
static std::atomic<bool> s_loading{false}; // protect against concurrent load/render

// ===== Shadow register state =====
// Flat register arrays per chip type, updated by replaying VgmEvents up to current tick
// Index [chip][...]: chip=0 first chip, chip=1 second chip (dual-chip VGMs)
// OPN family
static UINT8 s_shadowYM2612[2][2][0x100];  // [chip][port][addr]  OPN2
static UINT8 s_shadowYM2151[2][0x100];     // [chip][addr]  OPM
static UINT8 s_shadowYM2413[2][0x80];      // [chip][addr]  OPLL
static UINT8 s_shadowYM2203[2][0x100];     // [chip][addr]  OPN
static UINT8 s_shadowYM2608[2][2][0x100];  // [chip][port][addr]  OPNA
static UINT8 s_shadowYM2610[2][2][0x100];  // [chip][port][addr]  OPNB
static UINT8 s_shadowYM2610B[2][2][0x100]; // [chip][port][addr]  OPNB2
// OPL family
static UINT8 s_shadowYMF262[2][2][0x100];  // [chip][port][addr]  OPL3
static UINT8 s_shadowYM3812[2][0x100];     // [chip][addr]  OPL2
static UINT8 s_shadowYM3526[2][0x100];     // [chip][addr]  OPL
static UINT8 s_shadowY8950[2][0x100];      // [chip][addr]  MSX-Audio
static UINT8 s_shadowYMF271[2][4][0x100];    // [chip][group][addr]  OPL3-L
static UINT8 s_shadowYMF278B_fm[2][2][0x100]; // [chip][port][addr]  OPL4 FM
static UINT8 s_shadowYMF278B_pcm[2][0x100];   // [chip][addr]  OPL4 PCM
// PSG
static uint16_t s_shadowSN76496[2][8];   // [chip][ch*2/ch*2+1]
static UINT8 s_shadowAY8910[2][0x10];    // [chip][addr]
// ADPCM / sampler
static UINT8 s_shadowOKIM6258[2][0x08];  // [chip][addr]
static UINT8 s_shadowOKIM6295[2][0x10];  // [chip][addr]
// Misc
static UINT8 s_shadowNES_APU[2][0x20];   // [chip][addr]
static UINT8 s_shadowGB_DMG[2][0x30];    // [chip][addr]
static UINT8 s_shadowC6280[2][6][0x08];  // [chip][ch][reg] reg0=freq_lo,1=freq_hi,2=vol,3=wave_ctrl,4=env,5=pan
static UINT8 s_shadowSAA1099[2][0x20];   // [chip][addr]
static UINT8 s_shadowPOKEY[2][0x10];     // [chip][addr]
static UINT8 s_shadowWSWAN[2][0x100];    // [chip][addr]
static UINT8 s_shadowK051649[2][5][0x60]; // [chip][port][offset] port0=wave,1=freq,2=vol,3=keyon,4=wave(SCC+)
static UINT8 s_shadowRF5C68[2][0x20];    // [chip][addr]
// Additional chips
static UINT8 s_shadowYMZ280B[2][0x80];   // [chip][addr]
static UINT8 s_shadowYMW258[2][0x100];   // [chip][addr]
static UINT8 s_shadowUPD7759[2][0x08];   // [chip][addr]
static UINT8 s_shadowK054539[2][2][0x100]; // [chip][port][addr]
static UINT8 s_shadowC140[2][0x100];     // [chip][addr]
static UINT8 s_shadowK053260[2][0x30];   // [chip][addr]
static UINT8 s_shadowQSound[2][0x100];   // [chip][addr]
static UINT8 s_shadowSCSP[2][0x400];     // [chip][addr] 32 slots * 0x20 bytes
static UINT8 s_shadowVBOY_VSU[2][0x100]; // [chip][addr]
static UINT8 s_shadowES5503[2][0x100];   // [chip][addr]
static UINT8 s_shadowES5506[2][0x100];   // [chip][addr]
static UINT8 s_shadowX1_010[2][0x100];   // [chip][addr] (low byte only)
static UINT8 s_shadowC352[2][0x200];     // [chip][addr] 32 ch * 16 bytes = 0x200
static UINT8 s_shadowGA20[2][0x20];      // [chip][addr]
static UINT8 s_shadowSEGAPCM[2][0x100];  // [chip][addr] (low byte only)
static UINT16 s_shadow32XPWM[2][5];    // [chip][reg] 32X PWM: 5x12-bit regs (Int/Cycle/L/R/Mono)

// Per-channel keyon state for write-only strobe registers
static bool   s_ym2151KeyOn[2][8]   = {};  // [chip] YM2151: 8 channels
static bool   s_ym2151KeyOff[2][8]  = {};  // keyoff state
static bool   s_ym2203KeyOn[2][3]   = {};  // [chip] YM2203: 3 FM channels
static bool   s_ym2203KeyOff[2][3]  = {};
static bool   s_ym2612KeyOn[2][6]   = {};  // [chip] YM2612: 6 FM channels
static bool   s_ym2612KeyOff[2][6]  = {};
static bool   s_ym2608KeyOn[2][6]   = {};  // [chip] YM2608: 6 FM channels
static bool   s_ym2608KeyOff[2][6]  = {};
static bool   s_ym2608_adpcmbKeyOff[2] = {};
static bool   s_ym2610KeyOn[2][4]   = {};  // [chip] YM2610: 4 FM channels
static bool   s_ym2610KeyOff[2][4]  = {};
// OPL/OPLL: keyon is stored in register, track keyoff state for envelope decay
static bool   s_ym3812KeyOn[2][9]   = {};  // YM3812 (OPL2): 9 channels
static bool   s_ym3812KeyOff[2][9]  = {};  // keyoff state
static bool   s_ym3526KeyOn[2][9]   = {};  // YM3526 (OPL): 9 channels
static bool   s_ym3526KeyOff[2][9]  = {};
static bool   s_y8950KeyOn[2][9]    = {};  // Y8950: 9 channels
static bool   s_y8950KeyOff[2][9]   = {};
static bool   s_ymf262KeyOn[2][18]  = {};  // YMF262 (OPL3): 18 channels
static bool   s_ymf262KeyOff[2][18] = {};  // keyoff state
static bool   s_ymf271KeyOn[2][12]  = {};  // YMF271 (OPX): 12 groups
static bool   s_ymf271KeyOff[2][12] = {};
static bool   s_ymf278bKeyOn[2][18] = {};  // YMF278B (OPL4): 18 FM channels
static bool   s_ymf278bKeyOff[2][18]= {};
// SCSP: 32 channels with hardware envelope

struct ChVizState {
    bool  key_on;
    bool  key_on_event;
    float visual_note;
    float target_note;
    float start_note;    // note at key_on, used as poff anchor for portamento display
    float vibrato_offset;
    int   prev_kf;
    float decay;         // per-frame decay for level bar [0,1], reset to 1 on keyon
    float smooth_vol;    // smoothed volume for software-volume chips (lerp toward target each frame)
};
static ChVizState s_ym2151_viz[2][8] = {};  // [instance][ch]
static ChVizState s_ym2612_viz[2][6] = {};  // [instance][ch]
static ChVizState s_ym2612_dac_viz[2] = {}; // [instance] DAC channel
static UINT8 s_ym2612_dac_val[2] = {};      // [instance] last DAC visual peak
static UINT8 s_ym2612_dac_peak[2] = {};     // [instance] peak amplitude for normalization
static UINT32 s_ym2612_dac_accu[2] = {};     // [instance] DAC amplitude accumulator (MegaGRRL style)
static UINT32 s_ym2612_dac_count[2] = {};    // [instance] DAC write counter
static double s_ym2612_dac_last_tick[2] = {}; // [instance] last DAC write tick
static ChVizState s_ym2203_viz[2][3] = {};  // [instance][ch]
static ChVizState s_ym2608_viz[2][6] = {};  // [instance][ch]
static float  s_ym2608RhyKeyOn[2][6] = {};  // [chip] YM2608: 6 rhythm (ADPCM-A) channels, decay value
static float  s_ym2610RhyKeyOn[2][6] = {};  // [chip] YM2610: 6 ADPCM-A channels, decay value
static ChVizState s_ym2610_viz[2][4] = {};  // [instance][ch]
static ChVizState s_ym2608_adpcma_viz[2][6] = {}; // [instance][6ch]
static ChVizState s_ym2608_adpcmb_viz[2]    = {}; // [instance]
// OPL/OPLL viz
static ChVizState s_ymf262_viz[2][18] = {}; // [instance][18ch]
static ChVizState s_ym3812_viz[2][9]  = {}; // [instance][9ch]
static ChVizState s_ym3526_viz[2][9]  = {}; // [instance][9ch]
static ChVizState s_y8950_viz[2][9]   = {}; // [instance][9ch]
static ChVizState s_ymf271_viz[2][12] = {}; // [instance][12ch]
static ChVizState s_ymf278b_viz[2][18]= {}; // [instance][18ch]
static ChVizState s_scsp_viz[2][32]   = {}; // [instance][32ch]
static ChVizState s_ym2413_viz[2][9]  = {}; // [instance][9ch]
static ChVizState s_ym2413Rhy_viz[2][5] = {}; // [instance][5 rhythm ch] BD/HH/SD/TOM/CYM
static UINT8 s_ym2413RhyPrevKon[2]      = {}; // previous event rhythm keyon bits (for edge detect in event handler)
static bool  s_ym2413KeyOff[2][9]       = {}; // true = melody channel is in keyoff state
static bool  s_c6280KeyOff[2][6]        = {}; // HuC6280
static bool  s_saa1099KeyOff[2][6]      = {}; // SAA1099
static bool  s_k051649KeyOff[2][5]      = {}; // SCC
static bool  s_rf5c68KeyOff[2][8]       = {}; // RF5C68
static bool  s_okim6295KeyOff[2][4]     = {}; // OKIM6295
static bool  s_segapcmKeyOff[2][16]     = {}; // SegaPCM
static bool  s_es5506KeyOff[2][32]      = {}; // ES5506
static bool  s_nes_apuKeyOff[2][3]       = {}; // NES APU
static bool  s_gb_dmgKeyOff[2][3]        = {}; // GB DMG
static bool  s_qsoundKeyOff[2][16]       = {}; // QSound
static bool  s_ymw258KeyOff[2][28]       = {}; // MultiPCM
static bool  s_k054539KeyOff[2][8]        = {}; // K054539
static bool  s_ymz280bKeyOff[2][8]      = {}; // YMZ280B
static bool  s_k053260KeyOff[2][4]      = {}; // K053260
static bool  s_okim6258KeyOff[2][1]     = {}; // OKIM6258
static bool  s_wswanKeyOff[2][4]        = {}; // WonderSwan
static bool  s_es5503KeyOff[2][32]      = {}; // ES5503
static bool  s_x1_010KeyOff[2][16]      = {}; // X1-010
static bool  s_c352KeyOff[2][32]        = {}; // C352
static bool  s_pokeyKeyOff[2][4]        = {}; // POKEY
static bool  s_upd7759KeyOff[2][1]       = {}; // uPD7759
static bool  s_ga20KeyOff[2][4]         = {}; // GA20
static bool  s_c140KeyOff[2][24]         = {}; // C140
static bool  s_scspKeyOff[2][32]         = {}; // SCSP
// PSG viz
static ChVizState s_ay8910_viz[2][4]  = {}; // [instance][3tone+1noise]
static ChVizState s_ay8910_env_viz[2] = {}; // [instance] envelope frequency
static ChVizState s_sn76489_viz[2][3] = {}; // [instance][3 tone ch]
static float s_sn76489_noise_viz[2]   = {}; // [instance] noise decay
static ChVizState s_ym2203_ssg_viz[2][3] = {};
static ChVizState s_ym2608_ssg_viz[2][3] = {};
static ChVizState s_ym2610_ssg_viz[2][3] = {};
static ChVizState s_sn76496_viz[2][3] = {}; // [instance][3 tone ch]
static ChVizState s_c6280_viz[2][6]   = {}; // [instance][6ch]
static ChVizState s_saa1099_viz[2][6] = {}; // [instance][6ch]
static ChVizState s_k051649_viz[2][5] = {}; // [instance][5ch]
static ChVizState s_wswan_viz[2][4]   = {}; // [instance][4ch]
static ChVizState s_gb_dmg_viz[2][3]  = {}; // [instance][3 pitched ch]
static ChVizState s_nes_apu_viz[2][3] = {}; // [instance][P1/P2/Tri]
static ChVizState s_rf5c68_viz[2][8]  = {}; // [instance][8ch]
static ChVizState s_pokey_viz[2][4]   = {}; // [instance][4ch]
static ChVizState s_upd7759_viz[2][1]  = {}; // [instance][1ch]
static ChVizState s_okim6295_viz[2][4] = {}; // [instance][4ch]
static ChVizState s_segapcm_viz[2][16] = {}; // [instance][16ch]
static ChVizState s_es5506_viz[2][32]  = {}; // [instance][32ch]
static ChVizState s_qsound_viz[2][16]  = {}; // [instance][16ch]
static ChVizState s_ymw258_viz[2][28]  = {}; // [instance][28ch]
static ChVizState s_k054539_viz[2][8]  = {}; // [instance][8ch]
static ChVizState s_ymz280b_viz[2][8]  = {}; // [instance][8ch]
static ChVizState s_k053260_viz[2][4]  = {}; // [instance][4ch]
static ChVizState s_okim6258_viz[2][1] = {}; // [instance][1ch]
static ChVizState s_es5503_viz[2][32]  = {}; // [instance][32ch]
static ChVizState s_x1_010_viz[2][16]  = {}; // [instance][16ch]
static ChVizState s_c352_viz[2][32]    = {}; // [instance][32ch]
static ChVizState s_ga20_viz[2][4]     = {}; // [instance][4ch]
static ChVizState s_c140_viz[2][24]    = {}; // [instance][24ch]

static int    s_shadowSN76496Latch[2] = {0, 0}; // latch index for SN76489 writes, per chip
static size_t s_shadowEventIdx = 0;
static UINT32 s_shadowLastSample = (UINT32)-1;
static bool   s_shadowNeedsReset = false;

static void ResetShadowRegisters() {
    memset(s_ym2612_dac_viz,  0, sizeof(s_ym2612_dac_viz));
    memset(s_ym2612_dac_val,  0, sizeof(s_ym2612_dac_val));
    for(int i=0; i<2; i++) { s_ym2612_dac_last_tick[i] = 0; s_ym2612_dac_accu[i] = 0; s_ym2612_dac_count[i] = 0; s_ym2612_dac_peak[i] = 0; s_ym2612_dac_val[i] = 0; }
    memset(s_shadowYM2612,    0, sizeof(s_shadowYM2612));
    memset(s_shadowYM2151,    0, sizeof(s_shadowYM2151));
    memset(s_shadowYM2413,    0, sizeof(s_shadowYM2413));
    memset(s_shadowYM2203,    0, sizeof(s_shadowYM2203));
    memset(s_shadowYM2608,    0, sizeof(s_shadowYM2608));
    memset(s_shadowYM2610,    0, sizeof(s_shadowYM2610));
    memset(s_shadowYM2610B,   0, sizeof(s_shadowYM2610B));
    memset(s_shadowYMF262,    0, sizeof(s_shadowYMF262));
    memset(s_shadowYM3812,    0, sizeof(s_shadowYM3812));
    memset(s_shadowYM3526,    0, sizeof(s_shadowYM3526));
    memset(s_shadowY8950,     0, sizeof(s_shadowY8950));
    memset(s_shadowUPD7759,   0, sizeof(s_shadowUPD7759));
    memset(s_shadowYMF271,    0, sizeof(s_shadowYMF271));
    memset(s_shadowYMF278B_fm,  0, sizeof(s_shadowYMF278B_fm));
    memset(s_ym2203_ssg_viz, 0, sizeof(s_ym2203_ssg_viz));
    memset(s_ym2608_ssg_viz, 0, sizeof(s_ym2608_ssg_viz));
    memset(s_ym2610_ssg_viz, 0, sizeof(s_ym2610_ssg_viz));
    memset(s_shadowYMF278B_pcm, 0, sizeof(s_shadowYMF278B_pcm));
    memset(s_shadowSN76496,   0, sizeof(s_shadowSN76496));
    // SN76496 power-on: all attenuation registers = 0xF (silent)
    for (int c = 0; c < 2; c++)
        for (int ch = 0; ch < 4; ch++)
            s_shadowSN76496[c][ch*2+1] = 0xF;
    memset(s_shadowAY8910,    0, sizeof(s_shadowAY8910));
    memset(s_shadowOKIM6258,  0, sizeof(s_shadowOKIM6258));
    memset(s_shadowOKIM6295,  0, sizeof(s_shadowOKIM6295));
    memset(s_shadowNES_APU,   0, sizeof(s_shadowNES_APU));
    memset(s_shadowGB_DMG,    0, sizeof(s_shadowGB_DMG));
    memset(s_shadowC6280,     0, sizeof(s_shadowC6280));
    memset(s_shadowSAA1099,   0, sizeof(s_shadowSAA1099));
    memset(s_shadowPOKEY,     0, sizeof(s_shadowPOKEY));
    memset(s_shadowWSWAN,     0, sizeof(s_shadowWSWAN));
    memset(s_shadowK051649,   0, sizeof(s_shadowK051649));
    memset(s_shadowRF5C68,    0, sizeof(s_shadowRF5C68));
    memset(s_shadowYMZ280B,   0, sizeof(s_shadowYMZ280B));
    memset(s_shadowYMW258,    0, sizeof(s_shadowYMW258));
    memset(s_shadowK054539,   0, sizeof(s_shadowK054539));
    memset(s_shadowC140,      0, sizeof(s_shadowC140));
    memset(s_shadowK053260,   0, sizeof(s_shadowK053260));
    memset(s_shadowQSound,    0, sizeof(s_shadowQSound));
    memset(s_shadowSCSP,      0, sizeof(s_shadowSCSP));
    memset(s_shadowVBOY_VSU,  0, sizeof(s_shadowVBOY_VSU));
    memset(s_shadowES5503,    0, sizeof(s_shadowES5503));
    memset(s_shadowES5506,    0, sizeof(s_shadowES5506));
    memset(s_shadowX1_010,    0, sizeof(s_shadowX1_010));
    memset(s_shadowC352,      0, sizeof(s_shadowC352));
    memset(s_shadowGA20,      0, sizeof(s_shadowGA20));
    memset(s_shadowSEGAPCM,   0, sizeof(s_shadowSEGAPCM));
    memset(s_shadow32XPWM,    0, sizeof(s_shadow32XPWM));
    memset(s_ym2151KeyOn,     0, sizeof(s_ym2151KeyOn));
    memset(s_ym2151KeyOff,    1, sizeof(s_ym2151KeyOff));
    memset(s_ym2203KeyOn,     0, sizeof(s_ym2203KeyOn));
    memset(s_ym2203KeyOff,    1, sizeof(s_ym2203KeyOff));
    memset(s_ym2612KeyOn,     0, sizeof(s_ym2612KeyOn));
    memset(s_ym2612KeyOff,    1, sizeof(s_ym2612KeyOff));
    memset(s_ym2608KeyOn,       0, sizeof(s_ym2608KeyOn));
    memset(s_ym2608KeyOff,      1, sizeof(s_ym2608KeyOff));
    memset(s_ym2608_adpcmbKeyOff, 1, sizeof(s_ym2608_adpcmbKeyOff));
    memset(s_ym2608RhyKeyOn,    0, sizeof(s_ym2608RhyKeyOn));
    memset(s_ym2610RhyKeyOn,    0, sizeof(s_ym2610RhyKeyOn));
    memset(s_ym2610KeyOn,       0, sizeof(s_ym2610KeyOn));
    memset(s_ym2610KeyOff,      1, sizeof(s_ym2610KeyOff));
    memset(s_ym3812KeyOff,      1, sizeof(s_ym3812KeyOff));
    memset(s_ym3526KeyOff,      1, sizeof(s_ym3526KeyOff));
    memset(s_y8950KeyOff,       1, sizeof(s_y8950KeyOff));
    memset(s_ymf262KeyOff,      1, sizeof(s_ymf262KeyOff));
    memset(s_ymf271KeyOff,      1, sizeof(s_ymf271KeyOff));
    memset(s_ymf278bKeyOff,     1, sizeof(s_ymf278bKeyOff));
    memset(s_scspKeyOff,        1, sizeof(s_scspKeyOff));
    memset(s_ym2151_viz,      0, sizeof(s_ym2151_viz));
    memset(s_ym2612_viz,      0, sizeof(s_ym2612_viz));
    memset(s_ym2203_viz,      0, sizeof(s_ym2203_viz));
    memset(s_ym2608_viz,      0, sizeof(s_ym2608_viz));
    memset(s_ym2608_adpcma_viz, 0, sizeof(s_ym2608_adpcma_viz));
    memset(s_ym2608_adpcmb_viz, 0, sizeof(s_ym2608_adpcmb_viz));
    memset(s_ym2610_viz,      0, sizeof(s_ym2610_viz));
    memset(s_ymf262_viz,      0, sizeof(s_ymf262_viz));
    memset(s_ym3812_viz,      0, sizeof(s_ym3812_viz));
    memset(s_ym3526_viz,      0, sizeof(s_ym3526_viz));
    memset(s_y8950_viz,       0, sizeof(s_y8950_viz));
    memset(s_ymf271_viz,      0, sizeof(s_ymf271_viz));
    memset(s_ymf278b_viz,     0, sizeof(s_ymf278b_viz));
    memset(s_scsp_viz,        0, sizeof(s_scsp_viz));
    memset(s_ym2413_viz,      0, sizeof(s_ym2413_viz));
    memset(s_ym2413Rhy_viz,    0, sizeof(s_ym2413Rhy_viz));
    memset(s_ym2413RhyPrevKon,  0, sizeof(s_ym2413RhyPrevKon));
    // Initialize all YM2413 channels as keyoff (like MDPlayer: Off[]=true at init)
    memset(s_ym2413KeyOff, 1, sizeof(s_ym2413KeyOff));
    memset(s_ay8910_viz,      0, sizeof(s_ay8910_viz));
    memset(s_sn76496_viz,     0, sizeof(s_sn76496_viz));
    memset(s_c6280_viz,       0, sizeof(s_c6280_viz));
    memset(s_saa1099_viz,     0, sizeof(s_saa1099_viz));
    memset(s_k051649_viz,     0, sizeof(s_k051649_viz));
    memset(s_wswan_viz,       0, sizeof(s_wswan_viz));
    memset(s_gb_dmg_viz,      0, sizeof(s_gb_dmg_viz));
    memset(s_nes_apu_viz,     0, sizeof(s_nes_apu_viz));
    memset(s_rf5c68_viz,      0, sizeof(s_rf5c68_viz));
    memset(s_okim6295_viz,    0, sizeof(s_okim6295_viz));
    memset(s_segapcm_viz,     0, sizeof(s_segapcm_viz));
    memset(s_es5506_viz,      0, sizeof(s_es5506_viz));
    memset(s_qsound_viz,      0, sizeof(s_qsound_viz));
    memset(s_ymw258_viz,      0, sizeof(s_ymw258_viz));
    memset(s_k054539_viz,     0, sizeof(s_k054539_viz));
    memset(s_c140_viz,        0, sizeof(s_c140_viz));
    memset(s_okim6295KeyOff,  1, sizeof(s_okim6295KeyOff));
    memset(s_segapcmKeyOff,   1, sizeof(s_segapcmKeyOff));
    memset(s_es5506KeyOff,    1, sizeof(s_es5506KeyOff));
    memset(s_nes_apuKeyOff,   1, sizeof(s_nes_apuKeyOff));
    memset(s_gb_dmgKeyOff,    1, sizeof(s_gb_dmgKeyOff));
    memset(s_qsoundKeyOff,    1, sizeof(s_qsoundKeyOff));
    memset(s_ymw258KeyOff,    1, sizeof(s_ymw258KeyOff));
    memset(s_k054539KeyOff,   1, sizeof(s_k054539KeyOff));
    memset(s_ymz280bKeyOff,   1, sizeof(s_ymz280bKeyOff));
    memset(s_k053260KeyOff,   1, sizeof(s_k053260KeyOff));
    memset(s_okim6258KeyOff,  1, sizeof(s_okim6258KeyOff));
    memset(s_pokeyKeyOff,     1, sizeof(s_pokeyKeyOff));
    memset(s_upd7759KeyOff,    1, sizeof(s_upd7759KeyOff));
    memset(s_wswanKeyOff,     1, sizeof(s_wswanKeyOff));
    memset(s_es5503KeyOff,    1, sizeof(s_es5503KeyOff));
    memset(s_x1_010KeyOff,    1, sizeof(s_x1_010KeyOff));
    memset(s_c352KeyOff,      1, sizeof(s_c352KeyOff));
    memset(s_ga20KeyOff,      1, sizeof(s_ga20KeyOff));
    memset(s_c140KeyOff,      1, sizeof(s_c140KeyOff));
    memset(s_ymz280b_viz,     0, sizeof(s_ymz280b_viz));
    memset(s_k053260_viz,     0, sizeof(s_k053260_viz));
    memset(s_okim6258_viz,    0, sizeof(s_okim6258_viz));
    memset(s_pokey_viz,       0, sizeof(s_pokey_viz));
    memset(s_upd7759_viz,      0, sizeof(s_upd7759_viz));
    memset(s_es5503_viz,      0, sizeof(s_es5503_viz));
    memset(s_x1_010_viz,      0, sizeof(s_x1_010_viz));
    memset(s_c352_viz,        0, sizeof(s_c352_viz));
    memset(s_ga20_viz,        0, sizeof(s_ga20_viz));
    s_shadowSN76496Latch[0] = 0;
    s_shadowSN76496Latch[1] = 0;
    s_shadowEventIdx = 0;
    s_shadowLastSample = (UINT32)-1;
}

// Detect active chip types by checking if any shadow register has non-zero data
static std::set<UINT8> GetActiveDevTypesFromShadow() {
    std::set<UINT8> active;
    auto checkNonZero = [](const void* data, size_t len) -> bool {
        const UINT8* p = (const UINT8*)data;
        for (size_t i = 0; i < len; i++) { if (p[i]) return true; }
        return false;
    };
    // Check chip 0 only (most VGMs use single chip)
    // Using correct DEVID values from SoundDevs.h
    if (checkNonZero(s_shadowSN76496[0], sizeof(s_shadowSN76496[0]))) active.insert(0x00); // SN76496
    if (checkNonZero(s_shadowYM2413[0], sizeof(s_shadowYM2413[0])))  active.insert(0x01); // YM2413
    if (checkNonZero(s_shadowYM2612[0], sizeof(s_shadowYM2612[0])))  active.insert(0x02); // YM2612
    if (checkNonZero(s_shadowYM2151[0], sizeof(s_shadowYM2151[0])))  active.insert(0x03); // YM2151
    if (checkNonZero(s_shadowSEGAPCM[0], sizeof(s_shadowSEGAPCM[0]))) active.insert(0x04); // SegaPCM
    if (checkNonZero(s_shadowRF5C68[0], sizeof(s_shadowRF5C68[0])))  active.insert(0x05); // RF5C68
    if (checkNonZero(s_shadowYM2203[0], sizeof(s_shadowYM2203[0])))  active.insert(0x06); // YM2203
    if (checkNonZero(s_shadowYM2608[0], sizeof(s_shadowYM2608[0])))  active.insert(0x07); // YM2608
    if (checkNonZero(s_shadowYM2610[0], sizeof(s_shadowYM2610[0])))  active.insert(0x08); // YM2610
    if (checkNonZero(s_shadowYM2610B[0], sizeof(s_shadowYM2610B[0]))) active.insert(0x08); // YM2610B (same as YM2610)
    if (checkNonZero(s_shadowYM3812[0], sizeof(s_shadowYM3812[0])))  active.insert(0x09); // YM3812
    if (checkNonZero(s_shadowYM3526[0], sizeof(s_shadowYM3526[0])))  active.insert(0x0A); // YM3526
    if (checkNonZero(s_shadowY8950[0], sizeof(s_shadowY8950[0])))    active.insert(0x0B); // Y8950
    if (checkNonZero(s_shadowYMF262[0], sizeof(s_shadowYMF262[0])))  active.insert(0x0C); // YMF262
    if (checkNonZero(s_shadowYMF278B_fm[0], sizeof(s_shadowYMF278B_fm[0])) ||
        checkNonZero(s_shadowYMF278B_pcm[0], sizeof(s_shadowYMF278B_pcm[0]))) active.insert(0x0D); // YMF278B
    if (checkNonZero(s_shadowYMF271[0], sizeof(s_shadowYMF271[0])))  active.insert(0x0E); // YMF271
    if (checkNonZero(s_shadowYMZ280B[0], sizeof(s_shadowYMZ280B[0]))) active.insert(0x0F); // YMZ280B
    if (checkNonZero(s_shadow32XPWM[0], sizeof(s_shadow32XPWM[0])))  active.insert(0x11); // 32X_PWM
    if (checkNonZero(s_shadowAY8910[0], sizeof(s_shadowAY8910[0])))  active.insert(0x12); // AY8910
    if (checkNonZero(s_shadowGB_DMG[0], sizeof(s_shadowGB_DMG[0])))  active.insert(0x13); // GB_DMG
    if (checkNonZero(s_shadowNES_APU[0], sizeof(s_shadowNES_APU[0]))) active.insert(0x14); // NES_APU
    if (checkNonZero(s_shadowYMW258[0], sizeof(s_shadowYMW258[0])))  active.insert(0x15); // YMW258
    if (checkNonZero(s_shadowUPD7759[0], sizeof(s_shadowUPD7759[0]))) active.insert(0x16); // uPD7759
    if (checkNonZero(s_shadowOKIM6258[0], sizeof(s_shadowOKIM6258[0]))) active.insert(0x17); // OKIM6258
    if (checkNonZero(s_shadowOKIM6295[0], sizeof(s_shadowOKIM6295[0]))) active.insert(0x18); // OKIM6295
    if (checkNonZero(s_shadowK051649[0], sizeof(s_shadowK051649[0]))) active.insert(0x19); // K051649
    if (checkNonZero(s_shadowK054539[0], sizeof(s_shadowK054539[0]))) active.insert(0x1A); // K054539
    if (checkNonZero(s_shadowC6280[0], sizeof(s_shadowC6280[0])))    active.insert(0x1B); // C6280
    if (checkNonZero(s_shadowC140[0], sizeof(s_shadowC140[0])))      active.insert(0x1C); // C140
    if (checkNonZero(s_shadowK053260[0], sizeof(s_shadowK053260[0]))) active.insert(0x1D); // K053260
    if (checkNonZero(s_shadowPOKEY[0], sizeof(s_shadowPOKEY[0])))    active.insert(0x1E); // POKEY
    if (checkNonZero(s_shadowQSound[0], sizeof(s_shadowQSound[0])))  active.insert(0x1F); // QSound
    if (checkNonZero(s_shadowSCSP[0], sizeof(s_shadowSCSP[0])))      active.insert(0x20); // SCSP
    if (checkNonZero(s_shadowWSWAN[0], sizeof(s_shadowWSWAN[0])))    active.insert(0x21); // WSWAN
    if (checkNonZero(s_shadowVBOY_VSU[0], sizeof(s_shadowVBOY_VSU[0]))) active.insert(0x22); // VBOY_VSU
    if (checkNonZero(s_shadowSAA1099[0], sizeof(s_shadowSAA1099[0]))) active.insert(0x23); // SAA1099
    if (checkNonZero(s_shadowES5503[0], sizeof(s_shadowES5503[0])))  active.insert(0x24); // ES5503
    if (checkNonZero(s_shadowES5506[0], sizeof(s_shadowES5506[0])))  active.insert(0x25); // ES5506
    if (checkNonZero(s_shadowX1_010[0], sizeof(s_shadowX1_010[0])))  active.insert(0x26); // X1_010
    if (checkNonZero(s_shadowC352[0], sizeof(s_shadowC352[0])))      active.insert(0x27); // C352
    if (checkNonZero(s_shadowGA20[0], sizeof(s_shadowGA20[0])))      active.insert(0x28); // GA20
    return active;
}

static float s_pianoLabelFontSize   = 10.0f; // forward-declared for persistence
static float s_pianoLabelOffsetY   = 0.0f;  // extra offset from bottom of key (positive = up)
static bool  s_showPianoLabels     = false; // forward-declared for persistence

// ===== Chip alias persistence =====

// ===== Unified config persistence =====

static void SaveConfig() {
    char exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s%s", exeDir, k_configFile);
    FILE* f = fopen(path, "w");
    if (!f) return;

    // Player state
    fprintf(f, "[PlayerState]\n");
    fprintf(f, "CurrentPath=%s\n", s_currentPath);
    fprintf(f, "LoopCount=%d\n", s_loopCount);
    fprintf(f, "MasterVolume=%.4f\n", s_masterVolume);
    fprintf(f, "ShowScope=%d\n", s_showScope ? 1 : 0);
    fprintf(f, "ScopeHeight=%.1f\n", s_scopeHeight);
    fprintf(f, "ScopeBackgroundHeight=%.1f\n", s_scopeBackgroundHeight);
    fprintf(f, "SSGScopeShift=%d\n", g_ssg_scope_shift);
    // (Rendering options are now per-chip in [ScopePerChip])

    // UI state
    fprintf(f, "[UIState]\n");
    fprintf(f, "VgmHistoryCollapsed=%d\n", s_vgmHistoryCollapsed ? 1 : 0);
    fprintf(f, "VgmPlayerCollapsed=%d\n", s_vgmPlayerCollapsed ? 1 : 0);
    fprintf(f, "VgmBrowserCollapsed=%d\n", s_vgmBrowserCollapsed ? 1 : 0);
    fprintf(f, "VgmInlinePlayerCollapsed=%d\n", s_vgmInlinePlayerCollapsed ? 1 : 0);

    // Piano labels
    fprintf(f, "[PianoLabels]\n");
    fprintf(f, "LabelSize=%.0f\n", s_pianoLabelFontSize);
    fprintf(f, "LabelOffset=%.0f\n", s_pianoLabelOffsetY);
    fprintf(f, "ShowLabels=%d\n", s_showPianoLabels ? 1 : 0);

    // Folder history with use count
    fprintf(f, "[FolderHistory]\n");
    for (const auto& p : s_folderHistory)
        fprintf(f, "Folder=%s\n", p.c_str());

    // Chip aliases
    fprintf(f, "[ChipAliases]\n");
    for (const auto& kv : s_chipAliases)
        fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());

    // Per-chip scope settings
    fprintf(f, "[ScopePerChip]\n");
    for (const auto& kv : s_scopeChipSettings)
        fprintf(f, "0x%02X=%d,%d,%d,%d,%d,%.1f,%.1f,%d\n", kv.first,
            kv.second.samples, kv.second.offset,
            kv.second.search_window,
            kv.second.edge_align ? 1 : 0,
            kv.second.ac_mode,
            kv.second.width,
            kv.second.amplitude,
            kv.second.legacy_mode ? 1 : 0);

    // Per-chip per-channel color overrides
    fprintf(f, "[ScopeColors]\n");
    for (const auto& kv : s_scopeChipSettings) {
        // Only write if at least one channel has a custom color
        bool hasCustom = false;
        int maxCh = 0;
        for (int i = 0; i < 32; i++) {
            if (kv.second.channel_colors[i] != 0) {
                hasCustom = true;
                maxCh = i + 1;
            }
        }
        if (!hasCustom) continue;
        fprintf(f, "0x%02X=", kv.first);
        for (int i = 0; i < maxCh; i++) {
            if (i > 0) fprintf(f, ",");
            fprintf(f, "%08X", kv.second.channel_colors[i]);
        }
        fprintf(f, "\n");
    }

    fclose(f);
}

static const int k_maxFolderHistory = 100;

static void LoadConfig() {
    char exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s%s", exeDir, k_configFile);
    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[MAX_PATH + 32];
    std::string currentSection;

    while (fgets(line, sizeof(line), f)) {
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';

        // Skip empty lines and comments
        if (strlen(line) == 0 || line[0] == ';') continue;

        // Section header
        if (line[0] == '[' && line[strlen(line)-1] == ']') {
            currentSection = line;
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* val = eq + 1;

        if (currentSection == "[PlayerState]") {
            if (strcmp(line, "CurrentPath") == 0)
                snprintf(s_currentPath, MAX_PATH, "%s", val);
            else if (strcmp(line, "LoopCount") == 0)
                s_loopCount = atoi(val);
            else if (strcmp(line, "MasterVolume") == 0)
                s_masterVolume = (float)atof(val);
            else if (strcmp(line, "ShowScope") == 0)
                s_showScope = (atoi(val) != 0);
            else if (strcmp(line, "ScopeHeight") == 0)
                s_scopeHeight = (float)atof(val);
            else if (strcmp(line, "ScopeBackgroundHeight") == 0) {
                float h = (float)atof(val);
                if (h >= 100.0f && h <= 600.0f) s_scopeBackgroundHeight = h;
            }
            else if (strcmp(line, "SSGScopeShift") == 0) {
                int v = atoi(val);
                if (v >= 0 && v <= 10) g_ssg_scope_shift = v;
            }
            // (Rendering options are now per-chip in [ScopePerChip])
        }
        else if (currentSection == "[PianoLabels]") {
            if (strcmp(line, "LabelSize") == 0) {
                float sz = (float)atof(val);
                if (sz >= 6.0f && sz <= 20.0f) s_pianoLabelFontSize = sz;
            } else if (strcmp(line, "LabelOffset") == 0) {
                float v = (float)atof(val);
                if (v >= -50.0f && v <= 200.0f) s_pianoLabelOffsetY = v;
            } else if (strcmp(line, "ShowLabels") == 0) {
                s_showPianoLabels = (atoi(val) != 0);
            }
        }
        else if (currentSection == "[UIState]") {
            if (strcmp(line, "VgmHistoryCollapsed") == 0)
                s_vgmHistoryCollapsed = (atoi(val) != 0);
            else if (strcmp(line, "VgmPlayerCollapsed") == 0)
                s_vgmPlayerCollapsed = (atoi(val) != 0);
            else if (strcmp(line, "VgmBrowserCollapsed") == 0)
                s_vgmBrowserCollapsed = (atoi(val) != 0);
            else if (strcmp(line, "VgmInlinePlayerCollapsed") == 0)
                s_vgmInlinePlayerCollapsed = (atoi(val) != 0);
        }
        else if (currentSection == "[FolderHistory]") {
            if (strcmp(line, "Folder") == 0) {
                // Skip duplicates
                bool dup = false;
                for (const auto& existing : s_folderHistory)
                    if (existing == val) { dup = true; break; }
                if (!dup)
                    s_folderHistory.push_back(val);
            }
        }
        else if (currentSection == "[ChipAliases]") {
            s_chipAliases[line] = val;
        }
        else if (currentSection == "[ScopePerChip]") {
            // Parse "0x02=441,0,735,1,1,90.0,3.0,0" (samples,offset,search_window,edge_align,ac_mode,width,amplitude,legacy_mode)
            // Also accepts old format without trailing fields
            if (line[0] == '0' && line[1] == 'x') {
                UINT8 devType = (UINT8)strtol(line, NULL, 16);
                int vals[6] = {}; int n = 0; float fvals[2] = {90.0f, 3.0f}; int nf = 0;
                char tmp[256]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                char* tok = strtok(tmp, ",");
                // Parse first 5 integer fields: samples,offset,search_window,edge_align,ac_mode
                while (tok && n < 5) { vals[n++] = atoi(tok); tok = strtok(NULL, ","); }
                // Parse 2 float fields: width,amplitude
                while (tok && nf < 2) { fvals[nf++] = (float)atof(tok); tok = strtok(NULL, ","); }
                // Parse last integer field: legacy_mode
                if (tok) vals[5] = atoi(tok);

                if (n >= 1 && vals[0] >= 2 && vals[0] <= 4000) s_scopeChipSettings[devType].samples = vals[0];
                if (n >= 2 && vals[1] >= 0 && vals[1] <= 2000) s_scopeChipSettings[devType].offset = vals[1];
                if (n >= 3 && vals[2] >= 0 && vals[2] <= 4096) s_scopeChipSettings[devType].search_window = vals[2];
                if (n >= 4) s_scopeChipSettings[devType].edge_align = (vals[3] != 0);
                if (n >= 5 && vals[4] >= 0 && vals[4] <= 2) s_scopeChipSettings[devType].ac_mode = vals[4];
                if (nf >= 1 && fvals[0] >= 40.0f && fvals[0] <= 500.0f) s_scopeChipSettings[devType].width = fvals[0];
                if (nf >= 2 && fvals[1] >= 0.1f && fvals[1] <= 10.0f) s_scopeChipSettings[devType].amplitude = fvals[1];
                s_scopeChipSettings[devType].legacy_mode = (vals[5] != 0);
            }
        }
        else if (currentSection == "[ScopeColors]") {
            if (line[0] == '0' && line[1] == 'x') {
                UINT8 devType = (UINT8)strtol(line, NULL, 16);
                char tmp[1024]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                char* tok = strtok(tmp, ",");
                int ci = 0;
                while (tok && ci < 32) {
                    unsigned int c = (unsigned int)strtoul(tok, NULL, 16);
                    s_scopeChipSettings[devType].channel_colors[ci] = (ImU32)c;
                    ci++;
                    tok = strtok(NULL, ",");
                }
            }
        }
    }
    fclose(f);

    // Trim folder history to limit
    if ((int)s_folderHistory.size() > k_maxFolderHistory)
        s_folderHistory.resize(k_maxFolderHistory);
}

// Legacy functions for backward compatibility (redirect to unified config)
static void SaveChipAliases() { SaveConfig(); }
static void LoadChipAliases() { LoadConfig(); }
static void SaveFolderHistory() { SaveConfig(); }
static void LoadFolderHistory() { LoadConfig(); }
void SavePlayerState() { SaveConfig(); }
static void LoadPlayerState() { LoadConfig(); }

bool IsPlaying() { return s_fileLoaded && (s_playState == VGM_PLAY); }

void Pause() {
    if (!s_fileLoaded || s_playState != VGM_PLAY) return;
    s_playState = VGM_PAUSE;
    if (s_audDrv) AudioDrv_Pause(s_audDrv);
}

void Resume() {
    if (!s_fileLoaded || s_playState != VGM_PAUSE) return;
    s_playState = VGM_PLAY;
    if (s_audDrv) AudioDrv_Resume(s_audDrv);
}

static void AddToFolderHistory(const char* folder) {
    // Remove duplicate if exists
    for (auto it = s_folderHistory.begin(); it != s_folderHistory.end(); ++it)
        if (*it == folder) { s_folderHistory.erase(it); break; }
    s_folderHistory.insert(s_folderHistory.begin(), folder);
    // Enforce limit
    if ((int)s_folderHistory.size() > k_maxFolderHistory)
        s_folderHistory.resize(k_maxFolderHistory);
    SaveConfig();
}

// ===== Audio helpers =====


static UINT32 FillBuffer(void* /*drvStruct*/, void* userParam, UINT32 bufSize, void* data) {
    if (s_loading) {
        memset(data, 0, bufSize);
        return bufSize;
    }
    PlayerA* plr = (PlayerA*)userParam;
    if (!(plr->GetState() & PLAYSTATE_PLAY)) {
        memset(data, 0, bufSize);
        return bufSize;
    }
    UINT32 rendered = plr->Render(bufSize, data);
    if (rendered < bufSize)
        memset((UINT8*)data + rendered, 0, bufSize - rendered);
    return bufSize;
}

static UINT8 VgmFilePlayCallback(PlayerBase* /*player*/, void* /*userParam*/, UINT8 evtType, void* /*evtParam*/) {
    if (evtType == PLREVT_END)
        s_playState.fetch_or(0x80);  // mark end-of-track
    return 0x00;
}

static UINT8 StartAudioDevice() {
    UINT8 ret = AudioDrv_Init(s_audDrvId, &s_audDrv);
    if (ret & 0x80) { s_audDrv = nullptr; return ret; }

    // Set HWND for DirectSound (required for audio to work)
    AUDDRV_INFO* drvInfo = nullptr;
    Audio_GetDriverInfo(s_audDrvId, &drvInfo);
    if (drvInfo && drvInfo->drvSig == ADRVSIG_DSOUND) {
        DSound_SetHWnd(AudioDrv_GetDrvData(s_audDrv), MidiPlayer::g_mainWindow);
    }

    AUDIO_OPTS* opts = AudioDrv_GetOptions(s_audDrv);
    opts->sampleRate     = s_sampleRate;
    opts->numChannels    = 2;
    opts->numBitsPerSmpl = 16;
    opts->usecPerBuf     = 10000;  // 10 ms per buffer
    opts->numBuffers     = 10;     // 100 ms latency
    AudioDrv_SetCallback(s_audDrv, FillBuffer, &s_player);
    ret = AudioDrv_Start(s_audDrv, 0);  // device 0 = default
    if (ret & 0x80) { AudioDrv_Deinit(&s_audDrv); s_audDrv = nullptr; return ret; }
    UINT32 smplSize  = opts->numChannels * opts->numBitsPerSmpl / 8;
    UINT32 smplAlloc = AudioDrv_GetBufferSize(s_audDrv) / smplSize;
    s_player.SetOutputSettings(opts->sampleRate, opts->numChannels,
                               opts->numBitsPerSmpl, smplAlloc);
    return 0x00;
}

static void StopAudioDevice() {
    if (s_audDrv) {
        AudioDrv_Stop(s_audDrv);
        AudioDrv_Deinit(&s_audDrv);
        s_audDrv = nullptr;
    }
}

// ===== File list helpers =====

static bool IsSupportedExtW(const wchar_t* filename) {
    const wchar_t* dot = wcsrchr(filename, L'.');
    if (!dot) return false;
    static const wchar_t* exts[] = { L".vgm", L".vgz", L".s98", L".gym", L".dro", nullptr };
    for (int i = 0; exts[i]; i++)
        if (_wcsicmp(dot, exts[i]) == 0) return true;
    return false;
}

static void RefreshFileList() {
    s_fileList.clear();
    s_selectedFileIndex = -1;
    s_textScrollStates.clear();

    // Add parent entry if not at root
    if (strlen(s_currentPath) > 3) {
        MidiPlayer::FileEntry parent;
        parent.name = "..";
        parent.fullPath = "";
        parent.isDirectory = true;
        s_fileList.push_back(parent);
    }

    std::wstring wCurrentPath = MidiPlayer::UTF8ToWide(s_currentPath);
    std::wstring wSearchPath = wCurrentPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<MidiPlayer::FileEntry> dirs, files;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        MidiPlayer::FileEntry entry;
        entry.name = MidiPlayer::WideToUTF8(fd.cFileName);
        std::wstring wFullPath = wCurrentPath + L"\\" + fd.cFileName;
        entry.fullPath = MidiPlayer::WideToUTF8(wFullPath);
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (entry.isDirectory) dirs.push_back(entry);
        else if (IsSupportedExtW(fd.cFileName)) files.push_back(entry);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(dirs.begin(), dirs.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b){ return a.name < b.name; });
    std::sort(files.begin(), files.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b){ return a.name < b.name; });
    for (auto& d : dirs) s_fileList.push_back(d);
    for (auto& f : files) s_fileList.push_back(f);

    // Also rebuild playlist (files only, sorted)
    s_playlist.clear();
    for (auto& f : files) s_playlist.push_back(f.fullPath);
}

static void NavigateTo(const char* rawPath) {
    // Use Unicode _wfullpath to correctly handle non-ASCII (e.g. Chinese) paths
    std::wstring wRaw = MidiPlayer::UTF8ToWide(rawPath);
    wchar_t wCanon[MAX_PATH];
    if (_wfullpath(wCanon, wRaw.c_str(), MAX_PATH) == nullptr)
        wcsncpy(wCanon, wRaw.c_str(), MAX_PATH);
    std::string canon = MidiPlayer::WideToUTF8(wCanon);
    snprintf(s_currentPath, MAX_PATH, "%s", canon.c_str());
    if (!s_navigating) {
        if (s_navPos < (int)s_navHistory.size() - 1)
            s_navHistory.erase(s_navHistory.begin() + s_navPos + 1, s_navHistory.end());
        s_navHistory.push_back(canon);
        s_navPos++;
    }
    s_navigating = false;
    // Clear file browser filter when changing directory
    s_fileBrowserFilter[0] = '\0';
    // Reset auto-scroll tracking when changing directory
    s_trackedFolderPath.clear();
    s_scrollAnimY = 0.0f;
    s_autoScrollActive = false;
    RefreshFileList();
    AddToFolderHistory(s_currentPath);
    SavePlayerState();
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
    strncpy(parentPath, s_currentPath, MAX_PATH);
    int len = (int)strlen(parentPath);
    while (len > 0 && parentPath[len-1] == '\\') { parentPath[--len] = '\0'; }
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        s_lastExitedFolder = std::string(lastSlash + 1);
        *lastSlash = '\0';
        NavigateTo(parentPath);
    }
}

// ===== Lifecycle =====

void Init() {
    if (g_initialized) return;
    LoadFolderHistory();
    LoadPlayerState();
    LoadChipAliases();
    RefreshFileList();

    UINT8 ret = Audio_Init();
    if (ret & 0x80) return;

    // Find DirectSound driver by name (same as vgmplay_imgui.cpp)
    UINT32 drvCount = Audio_GetDriverCount();
    s_audDrvId = (UINT32)-1;
    for (UINT32 i = 0; i < drvCount; i++) {
        AUDDRV_INFO* info = nullptr;
        Audio_GetDriverInfo(i, &info);
        if (info && strcmp(info->drvName, "DirectSound") == 0) {
            s_audDrvId = i;
            break;
        }
    }
    if (s_audDrvId == (UINT32)-1) { Audio_Deinit(); return; }
    ret = StartAudioDevice();
    if (ret & 0x80) { Audio_Deinit(); return; }

    s_player.SetEventCallback(VgmFilePlayCallback, nullptr);

    PlayerA::Config pCfg = s_player.GetConfiguration();
    pCfg.masterVol  = (UINT32)(s_masterVolume * 0x10000);
    pCfg.loopCount  = s_loopCount;
    pCfg.fadeSmpls  = s_sampleRate * 4;  // 4 second fade
    pCfg.endSilenceSmpls = s_sampleRate / 2;  // 0.5 s silence at end
    pCfg.pbSpeed    = 1.0;
    s_player.SetConfiguration(pCfg);

    s_player.RegisterPlayerEngine(new VGMPlayer());
    s_player.RegisterPlayerEngine(new S98Player());
    s_player.RegisterPlayerEngine(new GYMPlayer());
    s_player.RegisterPlayerEngine(new DROPlayer());

    g_initialized = true;
    s_scope.Init();
}

void Shutdown() {
    if (!g_initialized) return;
    UnloadFile();
    StopAudioDevice();
    Audio_Deinit();
    g_initialized = false;
}

bool LoadFile(const char* path) {
    if (!g_initialized) return false;
    // Hold s_loading=true for the entire operation so FillBuffer stays silent.
    s_loading = true;
    // Wait >1 audio buffer period (10ms) so any in-flight Render() call completes.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Stop and release any current file without touching s_loading.
    if (s_fileLoaded) {
        s_player.Stop();
        s_vgmFile.Unload();
        s_loadedPath.clear();
        s_fileLoaded = false;
        s_playState  = 0;
    }

    std::wstring wPath = MidiPlayer::UTF8ToWide(path);

    // Extract filename for logging
    const char* fname = strrchr(path, '\\');
    if (!fname) fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    // VgmLog("\n=== LoadFile: %s ===\n", fname);
    // VgmLog("  Path: %s\n", path);

    // Load into VGMFile for tag/header/event parsing (must read entire file)
    // VgmLog("[1/3] Opening file for VGMFile parser...\n");
    DATA_LOADER* dLoad1 = FileLoader_InitW(wPath.c_str());
    if (!dLoad1) {
        VgmLog("  ERROR: FileLoader_InitW failed\n");
        s_loading = false; return false;
    }
    if (DataLoader_Load(dLoad1)) {
        VgmLog("  ERROR: DataLoader_Load failed\n");
        DataLoader_Deinit(dLoad1); s_loading = false; return false;
    }
    // VgmLog("  File total: %u bytes\n", (unsigned)DataLoader_GetTotalSize(dLoad1));
    // VgmLog("  Bytes loaded before ReadAll: %u\n", (unsigned)DataLoader_GetSize(dLoad1));
    DataLoader_ReadAll(dLoad1);
    // VgmLog("  Bytes loaded after ReadAll: %u\n", (unsigned)DataLoader_GetSize(dLoad1));
    bool parseOk = s_vgmFile.Load(dLoad1);
    DataLoader_Deinit(dLoad1);
    {
        const VGM_HEADER* hdr = s_vgmFile.GetHeader();
        size_t evCount = s_vgmFile.GetEvents().size();
        VgmLog("  Parse result: %s\n", parseOk ? "OK" : "FAILED");
        if (hdr) {
            VgmLog("  VGM ver=0x%X sn_clk=0x%X numTicks=%u\n",
                hdr->fileVer, (unsigned)0, hdr->numTicks);
        }
        // VgmLog("  Events parsed: %u\n", (unsigned)evCount);
        if (!s_vgmFile._parseDbg.empty())
            VgmLog("%s", s_vgmFile._parseDbg.c_str());
        // Count events per chip type
        std::map<int,int> chipEvCount;
        for (const auto& ev : s_vgmFile.GetEvents()) chipEvCount[ev.chip_type]++;
        // (Event count logging removed)
    }

    // Load into PlayerA for audio (PlayerA owns this loader)
    // VgmLog("[2/3] Opening file for PlayerA...\n");
    DATA_LOADER* dLoad2 = FileLoader_InitW(wPath.c_str());
    if (!dLoad2) {
        VgmLog("  ERROR: FileLoader_InitW failed\n");
        s_vgmFile.Unload(); s_loading = false; return false;
    }
    DataLoader_SetPreloadBytes(dLoad2, 0x100);
    if (DataLoader_Load(dLoad2)) {
        VgmLog("  ERROR: DataLoader_Load failed\n");
        DataLoader_Deinit(dLoad2); s_vgmFile.Unload(); s_loading = false; return false;
    }
    UINT8 plrRet = s_player.LoadFile(dLoad2);
    if (plrRet) {
        VgmLog("  ERROR: player.LoadFile failed (ret=0x%02X)\n", plrRet);
        DataLoader_Deinit(dLoad2);
        s_vgmFile.Unload();
        s_loading = false;
        return false;
    }
    // VgmLog("[3/3] Starting playback...\n");

    s_loadedPath = path;
    s_currentPlayingFilePath = path;
    s_fileLoaded = true;
    ResetShadowRegisters();
    s_scope.ResetOffsets();  // Clear oscilloscope buffers on track change
    s_player.Start();
    s_playState = VGM_PLAY;
    s_loading = false;
    // Update file browser selection highlight to match current playing track
    for (int i = 0; i < (int)s_fileList.size(); i++) {
        if (s_fileList[i].fullPath == path) {
            s_selectedFileIndex = i;
            s_trackedFolderPath = s_currentPath;  // Track this folder for auto-scroll
            s_autoScrollActive = true;
            break;
        }
    }
    {
        std::vector<PLR_DEV_INFO> devs;
        PlayerBase* pb = s_player.GetPlayer();
        if (pb) pb->GetSongDeviceInfo(devs);
        for (const auto& d : devs) {
            if (d.type == DEVID_YM3812 || d.type == DEVID_YM3526 ||
                d.type == DEVID_Y8950 || d.type == DEVID_YMF262 || d.type == DEVID_YM2413) {
                const char* typeName = "?";
                switch (d.type) {
                    case DEVID_YM3812: typeName = "YM3812"; break;
                    case DEVID_YM3526: typeName = "YM3526"; break;
                    case DEVID_Y8950:  typeName = "Y8950";  break;
                    case DEVID_YMF262: typeName = "YMF262"; break;
                    case DEVID_YM2413: typeName = "YM2413"; break;
                }
                const char* coreName = "?";
                switch (d.core) {
                    case FCC_MAME: coreName = "MAME"; break;
                    case FCC_ADLE: coreName = "AdLibEmu"; break;
                    case FCC_NUKE: coreName = "Nuked"; break;
                    case FCC_EMU_: coreName = "EMU2413"; break;
                }
                VgmLog("  [SCOPE_DBG] %s core=%s (FCC=0x%08X)\n", typeName, coreName, d.core);
            }
        }
        // Log chip instances for dual-chip debugging
        for (size_t i = 0; i < devs.size(); i++) {
            VgmLog("  [ChipInfo] #%zu: type=0x%02X inst=%d\n", i, devs[i].type, (int)devs[i].instance);
        }
    }
    // VgmLog("=== Load complete ===\n");
    return true;
}

void UnloadFile() {
    if (!s_fileLoaded) return;
    s_loading = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    s_player.Stop();
    s_vgmFile.Unload();
    s_loadedPath.clear();
    s_currentPlayingFilePath.clear();
    s_fileLoaded = false;
    s_playState  = 0;
    s_loading = false;
    s_tagScrollStates.clear();
    scope_reset_all();
}

// ===== Scrolling text helper =====
// Draws text that auto-scrolls horizontally when wider than available width,
// like a marquee. Uses s_tagScrollStates for per-ID scroll state.
static void DrawScrollingText(const char* id, const char* text, ImU32 col) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float availWidth = ImGui::GetContentRegionAvail().x;
    bool needsScrolling = textSize.x > availWidth;

    if (needsScrolling) {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::Dummy(ImVec2(availWidth, lineH));

        auto& st = s_tagScrollStates[id];
        if (st.lastUpdateTime.time_since_epoch().count() == 0) {
            st.scrollOffset = 0.0f; st.scrollDirection = 1.0f;
            st.pauseTimer = 1.0f;
            st.lastUpdateTime = std::chrono::steady_clock::now();
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - st.lastUpdateTime).count();
        st.lastUpdateTime = now;
        if (st.pauseTimer > 0.0f) st.pauseTimer -= dt;
        else {
            float scrollSpeed = 30.0f;
            st.scrollOffset += st.scrollDirection * scrollSpeed * dt;
            float maxScroll = textSize.x - availWidth + 20.0f;
            if (st.scrollOffset >= maxScroll) { st.scrollOffset = maxScroll; st.scrollDirection = -1.0f; st.pauseTimer = 1.0f; }
            else if (st.scrollOffset <= 0.0f) { st.scrollOffset = 0.0f; st.scrollDirection = 1.0f; st.pauseTimer = 1.0f; }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + lineH), true);
        dl->AddText(ImVec2(cursorPos.x - st.scrollOffset, cursorPos.y), col, text);
        dl->PopClipRect();
    } else {
        ImGui::TextColored(ImVec4(
            ((col >>  0) & 0xFF) / 255.0f,
            ((col >>  8) & 0xFF) / 255.0f,
            ((col >> 16) & 0xFF) / 255.0f,
            ((col >> 24) & 0xFF) / 255.0f), "%s", text);
        s_tagScrollStates.erase(id);
    }
}

// Scrolling text with explicit max width
static void DrawScrollingText(const char* id, const char* text, ImU32 col, float maxWidth) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float availWidth = (maxWidth > 0.0f) ? maxWidth : ImGui::GetContentRegionAvail().x;
    bool needsScrolling = textSize.x > availWidth;

    if (needsScrolling) {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::Dummy(ImVec2(availWidth, lineH));

        auto& st = s_tagScrollStates[id];
        if (st.lastUpdateTime.time_since_epoch().count() == 0) {
            st.scrollOffset = 0.0f; st.scrollDirection = 1.0f;
            st.pauseTimer = 1.0f;
            st.lastUpdateTime = std::chrono::steady_clock::now();
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - st.lastUpdateTime).count();
        st.lastUpdateTime = now;
        if (st.pauseTimer > 0.0f) st.pauseTimer -= dt;
        else {
            float scrollSpeed = 30.0f;
            st.scrollOffset += st.scrollDirection * scrollSpeed * dt;
            float maxScroll = textSize.x - availWidth + 20.0f;
            if (st.scrollOffset >= maxScroll) { st.scrollOffset = maxScroll; st.scrollDirection = -1.0f; st.pauseTimer = 1.0f; }
            else if (st.scrollOffset <= 0.0f) { st.scrollOffset = 0.0f; st.scrollDirection = 1.0f; st.pauseTimer = 1.0f; }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + lineH), true);
        dl->AddText(ImVec2(cursorPos.x - st.scrollOffset, cursorPos.y), col, text);
        dl->PopClipRect();
    } else {
        ImGui::TextColored(ImVec4(
            ((col >>  0) & 0xFF) / 255.0f,
            ((col >>  8) & 0xFF) / 255.0f,
            ((col >> 16) & 0xFF) / 255.0f,
            ((col >> 24) & 0xFF) / 255.0f), "%s", text);
        s_tagScrollStates.erase(id);
    }
}

// ===== Shadow register update =====
static uint64_t s_shadowUpdatedFrame = (uint64_t)-1;

static void UpdateShadowRegisters() {
    // Only update once per ImGui frame to avoid replaying events multiple times
    uint64_t thisFrame = ImGui::GetFrameCount();
    if (thisFrame == s_shadowUpdatedFrame) return;
    s_shadowUpdatedFrame = thisFrame;

    if (!s_fileLoaded || s_loading) return;
    // Guard: if end-of-track is pending, shadow is already at max tick - skip
    if (s_playState & 0x80) return;

    const std::vector<VgmEvent>& events = s_vgmFile.GetEvents();

    // (Silenced [DBG] logs)

    if (events.empty()) return;

    // Get current playback sample position
    PlayerBase* pBase = s_player.GetPlayer();
    if (!pBase) return;
    // Only update if player is actively playing
    if (!(s_player.GetState() & PLAYSTATE_PLAY)) return;

    // Use PLAYTIME_LOOP_EXCL for event sync: it jumps back on loop, so curSample
    // matches the VgmEvent tick domain correctly across multiple loops.
    // Use PLAYTIME_LOOP_INCL only to detect when a loop occurred (EXCL decreased).
    double curTimeSec = s_player.GetCurTime(PLAYTIME_LOOP_EXCL);
    UINT32 curSample = (UINT32)(curTimeSec * 44100.0);

    // Detect seek/restart/loop: sample went backwards, or explicit reset, or event overflow
    bool looped = (curSample < s_shadowLastSample);
    bool eventOverflow = (!events.empty() && s_shadowEventIdx >= events.size());
    if (looped || s_shadowNeedsReset || eventOverflow) {
        ResetShadowRegisters();
        s_shadowNeedsReset = false;
    }
    s_shadowLastSample = curSample;

    // curSample is already in VgmEvent tick units (44100Hz), no conversion needed
    UINT32 curTick = curSample;

    // Replay events up to current tick
    while (s_shadowEventIdx < events.size() &&
           events[s_shadowEventIdx].tick <= curTick)
    {
        const VgmEvent& ev = events[s_shadowEventIdx];
        switch (ev.chip_type) {
            // ---- OPN family ----
            case DEVID_YM2612: {
                int c = ev.chip_num & 1;
                // YM2612 DAC handling (0x2B in port 0 enables DAC, 0x80..0x8F are DAC writes)
                if (ev.cmd >= 0x80 && ev.cmd <= 0x8F) {
                    // 0x2B bit 7 must be set to enable DAC output instead of FM6
                    if (s_shadowYM2612[c][0][0x2B] & 0x80) {
                        s_ym2612_dac_last_tick[c] = (double)ev.tick;
                    }
                }
                // skip 0x80..0x8F (YM2612 DAC + delay cmds) in shadow reg update
                if (ev.cmd < 0x80 || ev.cmd > 0x8F) {
                    // YM2612 keyon strobe: reg 0x28 (port0), bits0-2=slot, bit4=slot keyon
                    if (ev.port == 0 && ev.addr == 0x28) {
                        int slot = ev.data & 0x07;
                        int ch = (slot < 4) ? slot : slot - 1;
                        if (ch < 6) {
                            // Track keyon at channel level: any slot keyon = channel keyon
                            bool new_kon = (ev.data >> 4) & 1;
                            if (new_kon && !s_ym2612KeyOn[c][ch]) {
                                s_ym2612_viz[c][ch].key_on_event = true;
                                s_ym2612_viz[c][ch].decay = 1.0f;
                                s_ym2612KeyOff[c][ch] = false;
                                // VgmLog("[2612] ch=%d keyon\n", ch+1);
                            } else if (!new_kon && s_ym2612KeyOn[c][ch]) {
                                // Check if any other slot in this channel is still keyon
                                bool other_keyon = false;
                                for (int s = 0; s < 4; s++) {
                                    int other_ch = (s < 4) ? s : s - 1;
                                    if (other_ch == ch && (s_shadowYM2612[c][0][0x28] & 0x10)) continue;
                                    UINT8 reg_data = s_shadowYM2612[c][0][0x28];
                                    int test_slot = s;
                                    if ((reg_data & 0x07) == test_slot && ((reg_data >> 4) & 1)) {
                                        other_keyon = true;
                                        break;
                                    }
                                }
                                if (!other_keyon)
                                    s_ym2612KeyOff[c][ch] = true;
                                // VgmLog("[2612] ch=%d keyoff\n", ch+1);
                            }
                            // Update channel keyon: set if any slot keyon, clear if all slots off
                            s_ym2612KeyOn[c][ch] = new_kon;
                        }
                    }
                    s_shadowYM2612[c][ev.port & 1][ev.addr] = ev.data;
                }
                break;
            }
            case DEVID_YM2151: {
                int c = ev.chip_num & 1;
                if (ev.addr == 0x08) {
                    // YM2151 keyon strobe: bits0-2=ch, bits6-4=KeyOn bits for slots 1-4
                    int ch = ev.data & 0x07;
                    bool new_kon = (ev.data & 0x78) != 0;
                    if (new_kon && !s_ym2151KeyOn[c][ch]) {
                        s_ym2151_viz[c][ch].key_on_event = true;
                        s_ym2151_viz[c][ch].decay = 1.0f;
                        s_ym2151KeyOff[c][ch] = false;
                        // VgmLog("[2151] ch=%d keyon\n", ch+1);
                    } else if (!new_kon && s_ym2151KeyOn[c][ch]) {
                        s_ym2151KeyOff[c][ch] = true;
                        // VgmLog("[2151] ch=%d keyoff\n", ch+1);
                    }
                    s_ym2151KeyOn[c][ch] = new_kon;
                }
                s_shadowYM2151[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_YM2413: {
                int c = ev.chip_num & 1;
                if (ev.addr < 0x80) {
                    // YM2413 reg 0x20-0x28: bit4=KEY, bit3-1=BLOCK, bit0=F-num MSB
                    if (ev.addr >= 0x20 && ev.addr <= 0x28) {
                        int ch2413 = ev.addr - 0x20;
                        bool prev_kon = (s_shadowYM2413[c][ev.addr] >> 4) & 1;
                        bool new_kon  = (ev.data >> 4) & 1;
                        if (new_kon && !prev_kon) {
                            // KEY rising edge: trigger fresh keyon
                            s_ym2413_viz[c][ch2413].key_on_event = true;
                            s_ym2413_viz[c][ch2413].decay = 1.0f;
                            s_ym2413_viz[c][ch2413].key_on = false;
                            s_ym2413KeyOff[c][ch2413] = false;
                        } else if (!new_kon && prev_kon) {
                            // KEY falling edge: start fast release
                            s_ym2413KeyOff[c][ch2413] = true;
                            s_ym2413_viz[c][ch2413].key_on = false;
                        }
                    }
                    // YM2413 reg 0x0E: rhythm mode + keyon bits (MDPlayer method)
                    // bits: 4=BD, 3=SD, 2=TOM, 1=CYM, 0=HH  (same order as MDPlayer c=0..4)
                    if (ev.addr == 0x0E) {
                        // kRhBit[i]: bit index in reg 0x0E for each drum i=BD/HH/SD/TOM/CYM
                        // MDPlayer order: BD=bit4, SD=bit3, TOM=bit2, CYM=bit1, HH=bit0
                        // Our order: i=0=BD,1=HH,2=SD,3=TOM,4=CYM -> bits 4,0,3,2,1
                        static const int kRhBit[5] = {4,0,3,2,1};
                        for (int i = 0; i < 5; i++) {
                            bool prev = (s_ym2413RhyPrevKon[c] >> kRhBit[i]) & 1;
                            bool cur  = (ev.data >> kRhBit[i]) & 1;
                            if (cur && !prev) {
                                // Rising edge: keyon pulse (MDPlayer: On[]=true only if Off[]==true)
                                s_ym2413Rhy_viz[c][i].key_on_event = true;
                                s_ym2413Rhy_viz[c][i].decay = 1.0f;
                            } else if (!cur && prev) {
                                // Falling edge: keyoff
                                s_ym2413Rhy_viz[c][i].key_on = false;
                            }
                        }
                        s_ym2413RhyPrevKon[c] = ev.data & 0x1F;
                    }
                    s_shadowYM2413[c][ev.addr] = ev.data;
                }
                break;
            }
            case DEVID_YM2203: {
                int c = ev.chip_num & 1;
                if (ev.addr == 0x28) {
                    int ch = ev.data & 0x03;
                    if (ch < 3) {
                        bool new_kon = (ev.data & 0xF0) != 0;
                        if (new_kon && !s_ym2203KeyOn[c][ch]) {
                            s_ym2203_viz[c][ch].key_on_event = true;
                            s_ym2203_viz[c][ch].decay = 1.0f;
                            s_ym2203KeyOff[c][ch] = false;
                        } else if (!new_kon && s_ym2203KeyOn[c][ch]) {
                            s_ym2203KeyOff[c][ch] = true;
                        }
                        s_ym2203KeyOn[c][ch] = new_kon;
                    }
                }
                s_shadowYM2203[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_YM2608: {
                int c = ev.chip_num & 1;
                if (ev.port == 0 && ev.addr == 0x28) {
                    int slot = ev.data & 0x07;
                    int ch = (slot < 4) ? slot : slot - 1;
                    if (ch < 6) {
                        bool new_kon = (ev.data & 0xF0) != 0;
                        if (new_kon && !s_ym2608KeyOn[c][ch]) {
                            s_ym2608_viz[c][ch].key_on_event = true;
                            s_ym2608_viz[c][ch].decay = 1.0f;
                            s_ym2608KeyOff[c][ch] = false;
                        } else if (!new_kon && s_ym2608KeyOn[c][ch]) {
                            s_ym2608KeyOff[c][ch] = true;
                        }
                        s_ym2608KeyOn[c][ch] = new_kon;
                    }
                }
                // Rhythm (ADPCM-A) keyon: port0 addr=0x10, bit7=0->keyon, bit7=1->keyoff
                if (ev.port == 0 && ev.addr == 0x10) {
                    bool key_off = (ev.data & 0x80) != 0;
                    for (int ch = 0; ch < 6; ch++) {
                        if ((ev.data >> ch) & 1) {
                            s_ym2608RhyKeyOn[c][ch] = key_off ? 0.0f : 1.0f;
                            if (!key_off) {
                                s_ym2608_adpcma_viz[c][ch].key_on_event = true;
                                s_ym2608_adpcma_viz[c][ch].decay = 1.0f;
                            }
                        }
                    }
                }
                // ADPCM-B keyon: port1 addr=0x00
                if (ev.port == 1 && ev.addr == 0x00) {
                    bool start = (ev.data & 0x80) != 0;
                    if (start) {
                        s_ym2608_adpcmb_viz[c].key_on_event = true;
                        s_ym2608_adpcmb_viz[c].decay = 1.0f;
                        s_ym2608_adpcmbKeyOff[c] = false;
                    } else {
                        s_ym2608_adpcmbKeyOff[c] = true;
                    }
                }
                s_shadowYM2608[c][ev.port & 1][ev.addr] = ev.data;
                break;
            }
            case DEVID_YM2610: {
                int c = ev.chip_num & 1;
                if ((ev.port & 0xFE) == 0) {
                    if (ev.port == 0 && ev.addr == 0x28) {
                        int slot = ev.data & 0x07;
                        int ch = (slot < 4) ? slot : (slot == 4 ? 3 : -1);
                        if (ch >= 0 && ch < 4) {
                            bool new_kon = (ev.data & 0xF0) != 0;
                            if (new_kon && !s_ym2610KeyOn[c][ch]) {
                                s_ym2610_viz[c][ch].key_on_event = true;
                                s_ym2610_viz[c][ch].decay = 1.0f;
                                s_ym2610KeyOff[c][ch] = false;
                            } else if (!new_kon && s_ym2610KeyOn[c][ch]) {
                                s_ym2610KeyOff[c][ch] = true;
                            }
                            s_ym2610KeyOn[c][ch] = new_kon;
                        }
                    }
                    // ADPCM-A keyon: port1 addr=0x00, bit7=0->keyon, bit7=1->keyoff
                    if (ev.port == 1 && ev.addr == 0x00) {
                        bool key_off = (ev.data & 0x80) != 0;
                        for (int ch = 0; ch < 6; ch++) {
                            if ((ev.data >> ch) & 1)
                                s_ym2610RhyKeyOn[c][ch] = key_off ? 0.0f : 1.0f;
                        }
                    }
                    s_shadowYM2610[c][ev.port & 1][ev.addr] = ev.data;
                } else {
                    s_shadowYM2610B[c][ev.port & 1][ev.addr] = ev.data;
                }
                break;
            }
            // ---- OPL family ----
            case DEVID_YMF262: {
                int c = ev.chip_num & 1;
                int p = ev.port & 1;
                // OPL3 keyon/keyoff: port0 reg 0xB0-0xB8 bit5, port1 reg 0x1B0-0x1B8 bit5
                if (p == 0 && ev.addr >= 0xB0 && ev.addr <= 0xB8) {
                    int ch = ev.addr - 0xB0;
                    bool prev_kon = (s_shadowYMF262[c][0][ev.addr] >> 5) & 1;
                    bool new_kon = (ev.data >> 5) & 1;
                    if (new_kon && !prev_kon) {
                        s_ymf262_viz[c][ch].key_on_event = true;
                        s_ymf262_viz[c][ch].decay = 1.0f;
                        s_ymf262KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ymf262KeyOff[c][ch] = true;
                    }
                    s_ymf262KeyOn[c][ch] = new_kon;
                } else if (p == 1 && ev.addr >= 0xB0 && ev.addr <= 0xB8) {
                    int ch = 9 + (ev.addr - 0xB0);  // channels 9-17
                    bool prev_kon = (s_shadowYMF262[c][1][ev.addr] >> 5) & 1;
                    bool new_kon = (ev.data >> 5) & 1;
                    if (new_kon && !prev_kon) {
                        s_ymf262_viz[c][ch].key_on_event = true;
                        s_ymf262_viz[c][ch].decay = 1.0f;
                        s_ymf262KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ymf262KeyOff[c][ch] = true;
                    }
                    s_ymf262KeyOn[c][ch] = new_kon;
                }
                s_shadowYMF262[c][p][ev.addr] = ev.data;
                break;
            }
            case DEVID_YM3812: {
                int c = ev.chip_num & 1;
                // OPL2 keyon/keyoff: reg 0xB0-0xB8 bit5
                if (ev.addr >= 0xB0 && ev.addr <= 0xB8) {
                    int ch = ev.addr - 0xB0;
                    bool prev_kon = (s_shadowYM3812[c][ev.addr] >> 5) & 1;
                    bool new_kon = (ev.data >> 5) & 1;
                    if (new_kon && !prev_kon) {
                        s_ym3812_viz[c][ch].key_on_event = true;
                        s_ym3812_viz[c][ch].decay = 1.0f;
                        s_ym3812KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ym3812KeyOff[c][ch] = true;
                    }
                    s_ym3812KeyOn[c][ch] = new_kon;
                }
                s_shadowYM3812[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_YM3526: {
                int c = ev.chip_num & 1;
                // OPL keyon/keyoff: reg 0xB0-0xB8 bit5
                if (ev.addr >= 0xB0 && ev.addr <= 0xB8) {
                    int ch = ev.addr - 0xB0;
                    bool prev_kon = (s_shadowYM3526[c][ev.addr] >> 5) & 1;
                    bool new_kon = (ev.data >> 5) & 1;
                    if (new_kon && !prev_kon) {
                        s_ym3526_viz[c][ch].key_on_event = true;
                        s_ym3526_viz[c][ch].decay = 1.0f;
                        s_ym3526KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ym3526KeyOff[c][ch] = true;
                    }
                    s_ym3526KeyOn[c][ch] = new_kon;
                }
                s_shadowYM3526[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_Y8950: {
                int c = ev.chip_num & 1;
                // OPL keyon/keyoff: reg 0xB0-0xB8 bit5
                if (ev.addr >= 0xB0 && ev.addr <= 0xB8) {
                    int ch = ev.addr - 0xB0;
                    bool prev_kon = (s_shadowY8950[c][ev.addr] >> 5) & 1;
                    bool new_kon = (ev.data >> 5) & 1;
                    if (new_kon && !prev_kon) {
                        s_y8950_viz[c][ch].key_on_event = true;
                        s_y8950_viz[c][ch].decay = 1.0f;
                        s_y8950KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_y8950KeyOff[c][ch] = true;
                    }
                    s_y8950KeyOn[c][ch] = new_kon;
                }
                s_shadowY8950[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_YMF271: {
                int c = ev.chip_num & 1;
                int grp = ev.port & 3;
                // YMF271 (OPX) keyon: port reg 0x0 bit0
                if (ev.addr == 0x0) {
                    bool prev_kon = s_shadowYMF271[c][grp][0] & 0x01;
                    bool new_kon = ev.data & 0x01;
                    if (new_kon && !prev_kon) {
                        s_ymf271_viz[c][grp].key_on_event = true;
                        s_ymf271_viz[c][grp].decay = 1.0f;
                        s_ymf271KeyOff[c][grp] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ymf271KeyOff[c][grp] = true;
                    }
                    s_ymf271KeyOn[c][grp] = new_kon;
                }
                if (grp < 4) s_shadowYMF271[c][grp][ev.addr] = ev.data;
                break;
            }
            case DEVID_YMF278B: {
                int c = ev.chip_num & 1;
                int p = ev.port;
                if (p <= 1) {
                    // YMF278B (OPL4) FM keyon: OPL3-compatible reg 0xB0-0xB8 bit5
                    if (ev.addr >= 0xB0 && ev.addr <= 0xB8) {
                        int ch = (p == 0) ? (ev.addr - 0xB0) : 9 + (ev.addr - 0xB0);
                        bool prev_kon = (s_shadowYMF278B_fm[c][p][ev.addr] >> 5) & 1;
                        bool new_kon = (ev.data >> 5) & 1;
                        if (new_kon && !prev_kon) {
                            s_ymf278b_viz[c][ch].key_on_event = true;
                            s_ymf278b_viz[c][ch].decay = 1.0f;
                            s_ymf278bKeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_ymf278bKeyOff[c][ch] = true;
                        }
                        s_ymf278bKeyOn[c][ch] = new_kon;
                    }
                    s_shadowYMF278B_fm[c][p][ev.addr] = ev.data;
                } else {
                    s_shadowYMF278B_pcm[c][ev.addr] = ev.data;
                }
                break;
            }
            // ---- PSG / DCSG ----
            case DEVID_SN76496: {
                int c = ev.chip_num & 1;
                UINT8 d = ev.data;
                if (d & 0x80) {
                    s_shadowSN76496Latch[c] = (d >> 4) & 0x07;
                    int ch = s_shadowSN76496Latch[c] >> 1;
                    int type = s_shadowSN76496Latch[c] & 1;
                    if (type == 1 && ch < 3) { // volume latch
                        bool prev_on = (s_shadowSN76496[c][ch*2+1] & 0x0F) < 0x0F;
                        bool new_on = (d & 0x0F) < 0x0F;
                        if (new_on && !prev_on) {
                            s_sn76496_viz[c][ch].key_on_event = true;
                            s_sn76496_viz[c][ch].decay = 1.0f;
                        }
                    }
                    if (type == 0)
                        s_shadowSN76496[c][ch * 2] = (s_shadowSN76496[c][ch * 2] & 0xFFF0) | (d & 0x0F);
                    else
                        s_shadowSN76496[c][ch * 2 + 1] = d & 0x0F;
                } else {
                    if ((s_shadowSN76496Latch[c] & 1) == 0) {
                        int ch = s_shadowSN76496Latch[c] >> 1;
                        if (ch < 3)
                            s_shadowSN76496[c][ch * 2] = ((d & 0x3F) << 4) | (s_shadowSN76496[c][ch * 2] & 0x000F);
                    }
                }
                break;
            }
            case DEVID_AY8910: {
                int c = ev.chip_num & 1;
                // AY8910 keyon: registers 0x08, 0x09, 0x0A bits 4:0 (volume)
                // and register 0x07 (enable bits 0-5)
                if (ev.addr >= 0x07 && ev.addr <= 0x0A) {
                    UINT8 mix = s_shadowAY8910[c][0x07];
                    if (ev.addr == 0x07) mix = ev.data;
                    for (int ch = 0; ch < 3; ch++) {
                        bool prev_on = !((s_shadowAY8910[c][0x07] >> ch) & 1) && (s_shadowAY8910[c][0x08+ch] & 0x1F) > 0;
                        bool new_on = !((mix >> ch) & 1) && ((ev.addr == 0x08+ch ? ev.data : s_shadowAY8910[c][0x08+ch]) & 0x1F) > 0;
                        if (new_on && !prev_on) {
                            s_ay8910_viz[c][ch].key_on_event = true;
                            s_ay8910_viz[c][ch].decay = 1.0f;
                            // AY8910 doesn't have a clean KeyOff variable usually, let's use decay
                        }
                    }
                }
                if (ev.addr < 0x10) s_shadowAY8910[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_C6280: {
                static UINT8 c6280SelCh[2] = {0, 0};
                int cc = ev.chip_num & 1;
                if (ev.addr == 0x00) {
                    c6280SelCh[cc] = ev.data & 0x07;
                } else if (ev.addr == 0x04) {
                    // HuC6280 control register: bit7=keyon (1=on, 0=off)
                    int ch = c6280SelCh[cc];
                    if (ch < 6) {
                        bool prev_kon = (s_shadowC6280[cc][ch][2] >> 7) & 1;
                        bool new_kon = (ev.data >> 7) & 1;
                        if (new_kon && !prev_kon) {
                            s_c6280_viz[cc][ch].key_on_event = true;
                            s_c6280_viz[cc][ch].decay = 1.0f;
                            s_c6280KeyOff[cc][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_c6280KeyOff[cc][ch] = true;
                        }
                    }
                    s_shadowC6280[cc][c6280SelCh[cc]][2] = ev.data;
                } else if (ev.addr >= 0x02 && ev.addr <= 0x07) {
                    s_shadowC6280[cc][c6280SelCh[cc]][ev.addr - 0x02] = ev.data;
                } else if (ev.addr == 0x08) {
                    s_shadowC6280[cc][0][6] = ev.data; // main vol (L/R)
                } else if (ev.addr == 0x09) {
                    s_shadowC6280[cc][0][7] = ev.data; // LFO ctrl
                }
                break;
            }
            case DEVID_SAA1099: {
                int c = ev.chip_num & 1;
                // SAA1099 keyon: register 0x14 (enable ch0-5), bit 0-5
                if (ev.addr == 0x14) {
                    for (int ch = 0; ch < 6; ch++) {
                        bool prev_kon = (s_shadowSAA1099[c][0x14] >> ch) & 1;
                        bool new_kon = (ev.data >> ch) & 1;
                        if (new_kon && !prev_kon) {
                            s_saa1099_viz[c][ch].key_on_event = true;
                            s_saa1099_viz[c][ch].decay = 1.0f;
                            s_saa1099KeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_saa1099KeyOff[c][ch] = true;
                        }
                    }
                }
                if (ev.addr < 0x20) s_shadowSAA1099[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_POKEY: {
                int c = ev.chip_num & 1;
                // POKEY keyon: registers 0x01, 0x03, 0x05, 0x07 bits 3:0 (volume)
                // 0 = off, >0 = on. Use as basic edge detection.
                if (ev.addr == 0x01 || ev.addr == 0x03 || ev.addr == 0x05 || ev.addr == 0x07) {
                    int ch = ev.addr / 2;
                    bool prev_kon = (s_shadowPOKEY[c][ev.addr] & 0x0F) > 0;
                    bool new_kon = (ev.data & 0x0F) > 0;
                    if (new_kon && !prev_kon) {
                        s_pokey_viz[c][ch].key_on_event = true;
                        s_pokey_viz[c][ch].decay = 1.0f;
                        s_pokeyKeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_pokeyKeyOff[c][ch] = true;
                    }
                }
                if (ev.addr < 0x10) s_shadowPOKEY[c][ev.addr] = ev.data;
                break;
            }
            // ---- ADPCM / sampler ----
            case DEVID_OKIM6258: {
                int c = ev.chip_num & 1;
                // 0x01: bits 0=start, 1=stop
                if (ev.addr == 0x01) {
                    bool prev_kon = (s_shadowOKIM6258[c][0x01] & 0x01) != 0;
                    bool new_kon = (ev.data & 0x01) != 0;
                    if (new_kon && !prev_kon) {
                        s_okim6258_viz[c][0].key_on_event = true;
                        s_okim6258_viz[c][0].decay = 1.0f;
                        s_okim6258KeyOff[c][0] = false;
                    } else if (!new_kon && prev_kon) {
                        s_okim6258KeyOff[c][0] = true;
                    }
                }
                if (ev.addr < 0x08) s_shadowOKIM6258[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_OKIM6295: {
                int c = ev.chip_num & 1;
                // OKIM6295 keyon: write to register 0x0 with bit 7=1, bits 3-0=channel mask
                if (ev.addr == 0x0) {
                    if (ev.data & 0x80) { // keyon
                        for (int ch = 0; ch < 4; ch++) {
                            if ((ev.data >> ch) & 1) {
                                s_okim6295_viz[c][ch].key_on_event = true;
                                s_okim6295_viz[c][ch].decay = 1.0f;
                                s_okim6295KeyOff[c][ch] = false;
                            }
                        }
                    } else { // keyoff
                        for (int ch = 0; ch < 4; ch++) {
                            if ((ev.data >> ch) & 1) {
                                s_okim6295KeyOff[c][ch] = true;
                            }
                        }
                    }
                }
                if (ev.addr < 0x10) s_shadowOKIM6295[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_RF5C68: {
                int c = ev.chip_num & 1;
                // RF5C68 keyon: register 0x08 (channel enable, bit 0-7, 0=on, 1=off)
                if (ev.addr == 0x08) {
                    for (int ch = 0; ch < 8; ch++) {
                        bool prev_kon = !((s_shadowRF5C68[c][0x08] >> ch) & 1);
                        bool new_kon = !((ev.data >> ch) & 1);
                        if (new_kon && !prev_kon) {
                            s_rf5c68_viz[c][ch].key_on_event = true;
                            s_rf5c68_viz[c][ch].decay = 1.0f;
                            s_rf5c68KeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_rf5c68KeyOff[c][ch] = true;
                        }
                    }
                }
                if (ev.addr < 0x20) s_shadowRF5C68[c][ev.addr] = ev.data;
                break;
            }
            // ---- Misc ----
            case DEVID_NES_APU: {
                int c = ev.chip_num & 1;
                if (ev.addr < 0x20) {
                    // NES APU keyon: register 0x15 (bits 0-4 for P1, P2, Tri, Noise, DMC)
                    if (ev.addr == 0x15) {
                        for (int ch = 0; ch < 3; ch++) { // P1, P2, Tri
                            bool prev_kon = (s_shadowNES_APU[c][0x15] >> ch) & 1;
                            bool new_kon = (ev.data >> ch) & 1;
                            if (new_kon && !prev_kon) {
                                s_nes_apu_viz[c][ch].key_on_event = true;
                                s_nes_apu_viz[c][ch].decay = 1.0f;
                                s_nes_apuKeyOff[c][ch] = false;
                            } else if (!new_kon && prev_kon) {
                                s_nes_apuKeyOff[c][ch] = true;
                            }
                        }
                    }
                    s_shadowNES_APU[c][ev.addr] = ev.data;
                }
                break;
            }
            case DEVID_GB_DMG: {
                int c = ev.chip_num & 1;
                if (ev.addr < 0x30) {
                    // GB DMG keyon: register 0x26 (NR52) bits 0-3 for CH1-4 status,
                    // and also trigger bits in NR14/24/34/44 (bit 7)
                    if (ev.addr == 0x14 || ev.addr == 0x19 || ev.addr == 0x1E || ev.addr == 0x23) {
                        int ch = (ev.addr - 0x14) / 5;
                        if (ch >= 0 && ch < 3) { // CH1, CH2, CH3 (pitched)
                            if (ev.data & 0x80) { // Trigger (KeyOn)
                                s_gb_dmg_viz[c][ch].key_on_event = true;
                                s_gb_dmg_viz[c][ch].decay = 1.0f;
                                s_gb_dmgKeyOff[c][ch] = false;
                            }
                        }
                    }
                    // KeyOff detection for GB DMG is tricky because it has hardware envelopes.
                    // However, we can use NR52 status as a hint if it ever goes low for a channel.
                    if (ev.addr == 0x26) {
                        for (int ch = 0; ch < 3; ch++) {
                            bool on = (ev.data >> ch) & 1;
                            if (!on) s_gb_dmgKeyOff[c][ch] = true;
                        }
                    }
                    s_shadowGB_DMG[c][ev.addr] = ev.data;
                }
                break;
            }
            case DEVID_WSWAN: {
                int c = ev.chip_num & 1;
                // WSwan keyon: SNDMOD reg (VGM offset 0x10, core 0x90) bits 0-3
                if (ev.addr == 0x10) {
                    for (int ch = 0; ch < 4; ch++) {
                        bool prev_kon = (s_shadowWSWAN[c][0x10] >> ch) & 1;
                        bool new_kon = (ev.data >> ch) & 1;
                        if (new_kon && !prev_kon) {
                            s_wswan_viz[c][ch].key_on_event = true;
                            s_wswan_viz[c][ch].decay = 1.0f;
                            s_wswanKeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_wswanKeyOff[c][ch] = true;
                        }
                    }
                }
                s_shadowWSWAN[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_K051649: {
                int c = ev.chip_num & 1;
                int port = ev.port & 0x07;
                // K051649 (SCC) keyon: port 3 reg 0x0 (bits 0-4)
                if (port == 3 && ev.addr == 0x0) {
                    for (int ch = 0; ch < 5; ch++) {
                        bool prev_kon = (s_shadowK051649[c][3][0] >> ch) & 1;
                        bool new_kon = (ev.data >> ch) & 1;
                        if (new_kon && !prev_kon) {
                            s_k051649_viz[c][ch].key_on_event = true;
                            s_k051649_viz[c][ch].decay = 1.0f;
                            s_k051649KeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_k051649KeyOff[c][ch] = true;
                        }
                    }
                }
                if (port < 5 && ev.addr < 0x60)
                    s_shadowK051649[c][port][ev.addr] = ev.data;
                break;
            }
            case DEVID_YMZ280B: {
                int c = ev.chip_num & 1;
                if ((ev.addr & 3) == 0) {
                    int ch = (ev.addr / 4) & 7;
                    bool prev_kon = (s_shadowYMZ280B[c][ev.addr] & 0x80) != 0;
                    bool new_kon = (ev.data & 0x80) != 0;
                    if (new_kon && !prev_kon) {
                        s_ymz280b_viz[c][ch].key_on_event = true;
                        s_ymz280b_viz[c][ch].decay = 1.0f;
                        s_ymz280bKeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ymz280bKeyOff[c][ch] = true;
                    }
                }
                if (ev.addr < 0x80) s_shadowYMZ280B[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_YMW258: {
                int c = ev.chip_num & 1;
                if ((ev.addr & 0x07) == 0x05) {
                    int ch = ev.addr / 8;
                    bool prev_kon = (s_shadowYMW258[c][ev.addr] & 0x80) != 0;
                    bool new_kon = (ev.data & 0x80) != 0;
                    if (new_kon && !prev_kon) {
                        s_ymw258_viz[c][ch].key_on_event = true;
                        s_ymw258_viz[c][ch].decay = 1.0f;
                        s_ymw258KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ymw258KeyOff[c][ch] = true;
                    }
                }
                s_shadowYMW258[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_uPD7759: {
                int c = ev.chip_num & 1;
                if (ev.addr == 0) {
                    bool prev_kon = (s_shadowUPD7759[c][0] & 0x01) != 0;
                    bool new_kon = (ev.data & 0x01) != 0;
                    if (new_kon && !prev_kon) {
                        s_upd7759_viz[c][0].key_on_event = true;
                        s_upd7759_viz[c][0].decay = 1.0f;
                        s_upd7759KeyOff[c][0] = false;
                    } else if (!new_kon && prev_kon) {
                        s_upd7759KeyOff[c][0] = true;
                    }
                }
                if (ev.addr < 0x08) s_shadowUPD7759[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_K054539: {
                int c = ev.chip_num & 1;
                if (ev.port == 0 && ev.addr == 0x14) {
                    for (int ch = 0; ch < 8; ch++) {
                        bool prev_kon = (s_shadowK054539[c][0][0x14] >> ch) & 1;
                        bool new_kon = (ev.data >> ch) & 1;
                        if (new_kon && !prev_kon) {
                            s_k054539_viz[c][ch].key_on_event = true;
                            s_k054539_viz[c][ch].decay = 1.0f;
                            s_k054539KeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_k054539KeyOff[c][ch] = true;
                        }
                    }
                }
                s_shadowK054539[c][ev.port & 1][ev.addr] = ev.data;
                break;
            }
            case DEVID_C140: {
                int c = ev.chip_num & 1;
                if ((ev.addr & 0x0F) == 0x05) {
                    int ch = ev.addr / 16;
                    bool prev_kon = (s_shadowC140[c][ev.addr] & 0x80) != 0;
                    bool new_kon = (ev.data & 0x80) != 0;
                    if (new_kon && !prev_kon) {
                        s_c140_viz[c][ch].key_on_event = true;
                        s_c140_viz[c][ch].decay = 1.0f;
                        s_c140KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_c140KeyOff[c][ch] = true;
                    }
                }
                s_shadowC140[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_K053260: {
                int c = ev.chip_num & 1;
                if (ev.addr == 0x00) {
                    for (int ch = 0; ch < 4; ch++) {
                        bool prev_kon = (s_shadowK053260[c][0x00] >> ch) & 1;
                        bool new_kon = (ev.data >> ch) & 1;
                        if (new_kon && !prev_kon) {
                            s_k053260_viz[c][ch].key_on_event = true;
                            s_k053260_viz[c][ch].decay = 1.0f;
                            s_k053260KeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_k053260KeyOff[c][ch] = true;
                        }
                    }
                }
                if (ev.addr < 0x30) s_shadowK053260[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_QSOUND: {
                int c = ev.chip_num & 1;
                // QSound keyon/off: regs 0x00-0x0F are channel volumes.
                // 0 = off, >0 = on. Use as basic edge detection.
                if (ev.addr < 0x10) {
                    int ch = ev.addr;
                    bool prev_kon = s_shadowQSound[c][ch] > 0;
                    bool new_kon = ev.data > 0;
                    if (new_kon && !prev_kon) {
                        s_qsound_viz[c][ch].key_on_event = true;
                        s_qsound_viz[c][ch].decay = 1.0f;
                        s_qsoundKeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_qsoundKeyOff[c][ch] = true;
                    }
                }
                s_shadowQSound[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_SCSP: {
                int c = ev.chip_num & 1;
                // SCSP keyon: register word 0 (addr ch*0x20) bit3
                if ((ev.addr & 0x1F) == 0x0) {
                    bool prev_kon = s_shadowSCSP[c][ev.addr] & 0x08;
                    bool new_kon = ev.data & 0x08;
                    int ch = ev.addr / 0x20;
                    if (new_kon && !prev_kon) {
                        s_scsp_viz[c][ch].key_on_event = true;
                        s_scsp_viz[c][ch].decay = 1.0f;
                        s_scspKeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_scspKeyOff[c][ch] = true;
                    }
                }
                s_shadowSCSP[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_VBOY_VSU:
                s_shadowVBOY_VSU[ev.chip_num & 1][ev.addr] = ev.data;
                break;
            case DEVID_ES5503: {
                int c = ev.chip_num & 1;
                // 0xA0-0xBF: control regs for 32 oscillators. bit0: 0=On, 1=Off
                if (ev.addr >= 0xA0 && ev.addr <= 0xBF) {
                    int ch = ev.addr - 0xA0;
                    bool prev_kon = !(s_shadowES5503[c][ev.addr] & 0x01);
                    bool new_kon = !(ev.data & 0x01);
                    if (new_kon && !prev_kon) {
                        s_es5503_viz[c][ch].key_on_event = true;
                        s_es5503_viz[c][ch].decay = 1.0f;
                        s_es5503KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_es5503KeyOff[c][ch] = true;
                    }
                }
                s_shadowES5503[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_ES5506: {
                int c = ev.chip_num & 1;
                // ES5506: 32 voices. Register [ch*4 + 1] is volume/on-off
                if ((ev.addr & 0x03) == 0x01) {
                    int ch = (ev.addr >> 2) & 0x1F;
                    bool prev_kon = s_shadowES5506[c][ev.addr] > 0;
                    bool new_kon = ev.data > 0;
                    if (new_kon && !prev_kon) {
                        s_es5506_viz[c][ch].key_on_event = true;
                        s_es5506_viz[c][ch].decay = 1.0f;
                        s_es5506KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_es5506KeyOff[c][ch] = true;
                    }
                }
                s_shadowES5506[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_X1_010: {
                int c = ev.chip_num & 1;
                // 16 channels, each 0x10 bytes. bit0 of byte 0: 1=On, 0=Off
                if ((ev.addr & 0x0F) == 0x00) {
                    int ch = (ev.addr >> 4) & 0x0F;
                    bool prev_kon = (s_shadowX1_010[c][ev.addr] & 0x01) != 0;
                    bool new_kon = (ev.data & 0x01) != 0;
                    if (new_kon && !prev_kon) {
                        s_x1_010_viz[c][ch].key_on_event = true;
                        s_x1_010_viz[c][ch].decay = 1.0f;
                        s_x1_010KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_x1_010KeyOff[c][ch] = true;
                    }
                }
                s_shadowX1_010[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_C352: {
                int c = ev.chip_num & 1;
                // 0xE1: addr = (ev.port<<8)|ev.addr (16-bit), data16 = ev.data16
                UINT16 c352addr = ((UINT16)ev.port << 8) | ev.addr;
                if (c352addr < 0x200) {
                    // C352 KeyOn/Off: Write to flags reg (ch*8+3). Bit 15 (high byte bit 7) is BUSY/KeyOn.
                    if ((c352addr & 0x07) == 0x06) {
                        int ch = (c352addr >> 3) & 0x1F;
                        bool prev_kon = (s_shadowC352[c][c352addr] & 0x80) != 0;
                        bool new_kon = (ev.data16 >> 8) & 0x80;
                        if (new_kon && !prev_kon) {
                            s_c352_viz[c][ch].key_on_event = true;
                            s_c352_viz[c][ch].decay = 1.0f;
                            s_c352KeyOff[c][ch] = false;
                        } else if (!new_kon && prev_kon) {
                            s_c352KeyOff[c][ch] = true;
                        }
                    }
                    s_shadowC352[c][c352addr]     = (UINT8)(ev.data16 >> 8);
                    if (c352addr + 1 < 0x200)
                        s_shadowC352[c][c352addr + 1] = (UINT8)(ev.data16 & 0xFF);
                }
                break;
            }
            case DEVID_GA20: {
                int c = ev.chip_num & 1;
                // 4 channels, each 8 bytes. byte 6: 0=Off, >0=On
                if ((ev.addr & 0x07) == 0x06) {
                    int ch = (ev.addr >> 3) & 0x03;
                    bool prev_kon = s_shadowGA20[c][ev.addr] != 0;
                    bool new_kon = ev.data != 0;
                    if (new_kon && !prev_kon) {
                        s_ga20_viz[c][ch].key_on_event = true;
                        s_ga20_viz[c][ch].decay = 1.0f;
                        s_ga20KeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_ga20KeyOff[c][ch] = true;
                    }
                }
                if (ev.addr < 0x20) s_shadowGA20[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_SEGAPCM: {
                int c = ev.chip_num & 1;
                // SegaPCM: 16 channels, each 8 bytes. Register 0x07 bit 0: 0=On, 1=Off
                if ((ev.addr & 0x07) == 0x07) {
                    int ch = (ev.addr >> 3) & 0x0F;
                    bool prev_kon = !(s_shadowSEGAPCM[c][ev.addr] & 0x01);
                    bool new_kon = !(ev.data & 0x01);
                    if (new_kon && !prev_kon) {
                        s_segapcm_viz[c][ch].key_on_event = true;
                        s_segapcm_viz[c][ch].decay = 1.0f;
                        s_segapcmKeyOff[c][ch] = false;
                    } else if (!new_kon && prev_kon) {
                        s_segapcmKeyOff[c][ch] = true;
                    }
                }
                s_shadowSEGAPCM[c][ev.addr] = ev.data;
                break;
            }
            case DEVID_32X_PWM: {
                int c = ev.chip_num & 1;
                if (ev.addr < 5) {
                    s_shadow32XPWM[c][ev.addr] = ev.data16 & 0x0FFF;
                }
                break;
            }
        }
        s_shadowEventIdx++;
    }

    // Per-frame volume decay (called once per ImGui frame, not per BuildLevelMeters call)
    for (int i = 0; i < 2; i++) {
        s_ym2612_dac_viz[i].smooth_vol -= 50.0f;
        if (s_ym2612_dac_viz[i].smooth_vol < 0.0f) s_ym2612_dac_viz[i].smooth_vol = 0.0f;
    }
}

// ===== File Browser UI =====

static void RenderFileBrowser() {
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

    // Breadcrumb path bar or edit mode
    if (!s_pathEditMode) {
        float availWidth = ImGui::GetContentRegionAvail().x;
        std::vector<std::string> segments = MidiPlayer::SplitPath(s_currentPath);
        std::vector<float> buttonWidths;
        std::vector<std::string> accumulatedPaths;
        std::string accumulatedPath;
        ImGuiStyle& style = ImGui::GetStyle();
        float framePaddingX = style.FramePadding.x;
        float itemSpacingX = style.ItemSpacing.x;
        float buttonBorderSize = style.FrameBorderSize;
        for (size_t i = 0; i < segments.size(); i++) {
            if (i == 0) accumulatedPath = segments[i];
            else { if (accumulatedPath.back() != '\\') accumulatedPath += "\\"; accumulatedPath += segments[i]; }
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
            if (ImGui::Button("...##ellipsis")) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                strcpy(s_pathInput, s_currentPath);
            }
            ImGui::SameLine();
            ImGui::Text(">"); ImGui::SameLine();
        }
        for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
            std::string btnId = segments[i] + "##seg" + std::to_string(i);
            if (ImGui::Button(btnId.c_str())) {
                NavigateTo(accumulatedPaths[i].c_str());
            }
            if (i < (int)segments.size() - 1) { ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine(); }
        }
        ImGui::EndGroup();
        ImVec2 barEndPos = ImGui::GetItemRectMax();
        float emptySpaceWidth = barStartPos.x + availWidth - barEndPos.x;
        if (emptySpaceWidth > 0) {
            ImGui::SameLine();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(barEndPos.x, barStartPos.y), ImVec2(barEndPos.x + emptySpaceWidth, barEndPos.y),
                ImGui::GetColorU32(ImGuiCol_FrameBg));
            ImGui::InvisibleButton("##pathEmpty", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                strcpy(s_pathInput, s_currentPath);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##PathInputVgm", s_pathInput, MAX_PATH,
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            NavigateTo(s_pathInput);
            s_pathEditMode = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_pathEditMode = false; s_pathEditModeJustActivated = false;
            strcpy(s_pathInput, s_currentPath);
        } else if (!s_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            s_pathEditMode = false;
            strcpy(s_pathInput, s_currentPath);
        }
        if (s_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            s_pathEditModeJustActivated = false;
        }
    }

    // Folder history dropdown
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##vgmHist", "VGM History", ImGuiComboFlags_HeightLarge)) {
        if (s_folderHistory.empty()) {
            ImGui::TextDisabled("(no history)");
        } else {
            for (int i = 0; i < (int)s_folderHistory.size(); i++) {
                size_t lastSlash = s_folderHistory[i].find_last_of("\\/");
                std::string folderName = (lastSlash != std::string::npos) ? s_folderHistory[i].substr(lastSlash + 1) : s_folderHistory[i];
                if (ImGui::Selectable(folderName.c_str(), false))
                    NavigateTo(s_folderHistory[i].c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", s_folderHistory[i].c_str());
            }
        }
        ImGui::EndCombo();
    }

    // Chip alias window is rendered separately in RenderChipAliasWindow()

    // File list
    ImGui::BeginChild("VgmFileList", ImVec2(-1, 0), true);

    std::string currentPathStr(s_currentPath);
    if (strlen(s_currentPath) > 0 && !s_autoScrollActive)
        s_pathScrollPositions[currentPathStr] = ImGui::GetScrollY();
    static std::string s_lastRestoredPath;
    if (currentPathStr != s_lastRestoredPath && s_pathScrollPositions.count(currentPathStr) > 0) {
        ImGui::SetScrollY(s_pathScrollPositions[currentPathStr]);
        s_lastRestoredPath = currentPathStr;
    }

    // Pre-scan to find the rendered index of currently playing file
    int playingFileRenderIndex = -1;
    int renderIndex = 0;
    for (int i = 0; i < (int)s_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = s_fileList[i];
        // Apply filter (same logic as render loop)
        if (s_fileBrowserFilter[0] != '\0' && entry.name != "..") {
            std::string lowerName = entry.name;
            std::string lowerFilter = s_fileBrowserFilter;
            for (auto& c : lowerName) c = tolower(c);
            for (auto& c : lowerFilter) c = tolower(c);
            if (lowerName.find(lowerFilter) == std::string::npos) continue;
        }
        // Check if this is the playing file
        if (!entry.isDirectory && entry.fullPath == s_currentPlayingFilePath) {
            playingFileRenderIndex = renderIndex;
            break;
        }
        renderIndex++;
    }

    // Smooth auto-scroll to keep playing file visible (only on track change)
    if (playingFileRenderIndex >= 0 && s_trackedFolderPath == currentPathStr && s_autoScrollActive) {
        float itemHeight = ImGui::GetTextLineHeightWithSpacing();
        ImVec2 childSize = ImGui::GetWindowSize();
        float windowHeight = childSize.y > 0 ? childSize.y : ImGui::GetContentRegionAvail().y;
        float currentScroll = ImGui::GetScrollY();
        float maxScroll = ImGui::GetScrollMaxY();

        // Detect user scroll interaction → cancel auto-scroll
        if (ImGui::GetIO().MouseWheel != 0.0f || ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            s_autoScrollActive = false;
            s_scrollAnimY = currentScroll;
        } else {
            float playingItemTop = playingFileRenderIndex * itemHeight;
            float playingItemBottom = playingItemTop + itemHeight;
            float visibleTop = currentScroll;
            float visibleBottom = currentScroll + windowHeight;

            // Detect when item is outside visible area (2px margin)
            float targetScroll = -1.0f;
            if (playingItemBottom > visibleBottom - 2.0f) {
                targetScroll = playingItemBottom - windowHeight + 2.0f;
            } else if (playingItemTop < visibleTop + 2.0f) {
                targetScroll = playingItemTop - 2.0f;
            }

            if (targetScroll >= 0) {
                if (targetScroll < 0) targetScroll = 0;
                if (targetScroll > maxScroll) targetScroll = maxScroll;

                float smoothFactor = 0.15f;
                s_scrollAnimY += (targetScroll - s_scrollAnimY) * smoothFactor;

                if (fabsf(s_scrollAnimY - targetScroll) < 0.5f) {
                    s_scrollAnimY = targetScroll;
                    s_autoScrollActive = false;  // Animation complete
                }

                ImGui::SetScrollY(s_scrollAnimY);
            } else {
                // Item already visible — no need to scroll
                s_scrollAnimY = currentScroll;
                s_autoScrollActive = false;
            }
        }

        s_lastTrackedIndex = playingFileRenderIndex;
    } else if (s_trackedFolderPath != currentPathStr) {
        s_scrollAnimY = ImGui::GetScrollY();
    }

    s_hoveredFileIndex = -1;
    for (int i = 0; i < (int)s_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = s_fileList[i];
        // Apply filter (always show ".." parent entry)
        if (s_fileBrowserFilter[0] != '\0' && entry.name != "..") {
            std::string lowerName = entry.name;
            std::string lowerFilter = s_fileBrowserFilter;
            for (auto& c : lowerName) c = tolower(c);
            for (auto& c : lowerFilter) c = tolower(c);
            if (lowerName.find(lowerFilter) == std::string::npos) continue;
        }
        bool isSelected = (s_selectedFileIndex == i);
        bool isExitedFolder = (!s_lastExitedFolder.empty() && entry.isDirectory
                               && entry.name == s_lastExitedFolder);
        bool isPlayingPath = false;
        bool isPlayingFile = false;
        if (!s_currentPlayingFilePath.empty()) {
            if (entry.isDirectory) {
                std::string entryPath = entry.fullPath;
                if (!entryPath.empty() && entryPath.back() != '\\') entryPath += "\\";
                if (s_currentPlayingFilePath.find(entryPath) == 0) isPlayingPath = true;
            } else if (entry.fullPath == s_currentPlayingFilePath) {
                isPlayingFile = true;
            }
        }

        std::string label;
        if (entry.name == "..") label = "[UP] " + entry.name;
        else if (entry.isDirectory) label = "[DIR] " + entry.name;
        else label = entry.name;

        if (isExitedFolder) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        else if (isPlayingFile) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
        else if (isPlayingPath) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 1.0f, 1.0f));

        ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        float availWidth = ImGui::GetContentRegionAvail().x;
        bool needsScrolling = textSize.x > availWidth;
        bool isHovered = false;

        auto handleClick = [&]() {
            s_selectedFileIndex = i;
            if (entry.name == "..") {
                NavToParent();
            } else if (entry.isDirectory) {
                s_lastExitedFolder.clear();
                NavigateTo(entry.fullPath.c_str());
            } else {
                s_currentPlayingFilePath = entry.fullPath;
                // find index in playlist
                for (int pi = 0; pi < (int)s_playlist.size(); pi++) {
                    if (s_playlist[pi] == entry.fullPath) { s_playlistIndex = pi; break; }
                }
                LoadFile(entry.fullPath.c_str());
            }
        };

        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImVec2 itemSize = ImVec2(availWidth, ImGui::GetTextLineHeightWithSpacing());
        ImGui::InvisibleButton(("##vgmitem" + std::to_string(i)).c_str(), itemSize);
        isHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) handleClick();

        if (needsScrolling && (isSelected || isExitedFolder || isHovered)) {
            if (!s_textScrollStates.count(i)) {
                MidiPlayer::TextScrollState st;
                st.scrollOffset = 0.0f; st.scrollDirection = 1.0f;
                st.pauseTimer = 1.0f;
                st.lastUpdateTime = std::chrono::steady_clock::now();
                s_textScrollStates[i] = st;
            }
            MidiPlayer::TextScrollState& ss = s_textScrollStates[i];
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - ss.lastUpdateTime).count();
            ss.lastUpdateTime = now;
            if (ss.pauseTimer > 0.0f) ss.pauseTimer -= dt;
            else {
                float scrollSpeed = 30.0f;
                ss.scrollOffset += ss.scrollDirection * scrollSpeed * dt;
                float maxScroll = textSize.x - availWidth + 20.0f;
                if (ss.scrollOffset >= maxScroll) { ss.scrollOffset = maxScroll; ss.scrollDirection = -1.0f; ss.pauseTimer = 1.0f; }
                else if (ss.scrollOffset <= 0.0f) { ss.scrollOffset = 0.0f; ss.scrollDirection = 1.0f; ss.pauseTimer = 1.0f; }
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (isPlayingFile) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), IM_COL32(40,80,50,255));
            else if (isSelected) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
            else if (isHovered) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            dl->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), true);
            dl->AddText(ImVec2(cursorPos.x - ss.scrollOffset, cursorPos.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            dl->PopClipRect();
        } else {
            if (needsScrolling && s_textScrollStates.count(i)) s_textScrollStates.erase(i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (isPlayingFile) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), IM_COL32(40,80,50,255));
            else if (isSelected) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_Header));
            else if (isHovered) dl->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + itemSize.y), ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            dl->AddText(cursorPos, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        }

        if (isHovered) s_hoveredFileIndex = i;
        if (isExitedFolder || isPlayingFile || isPlayingPath) ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

// ===== Main VGM Window =====

void Render() {
    if (!g_windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("libvgm Player", &g_windowOpen)) { ImGui::End(); return; }

    // ---- Transport controls ----
    bool hasFile = s_fileLoaded;
    bool isPlaying = hasFile && (s_playState == VGM_PLAY);
    (void)isPlaying;

    if (ImGui::Button(hasFile ? "Restart" : "Play")) {
        if (hasFile) {
            std::string reloadPath = s_loadedPath;
            UnloadFile();
            LoadFile(reloadPath.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { UnloadFile(); }
    ImGui::SameLine();
    if (ImGui::Button("|<")) {
        if (!s_playlist.empty() && s_playlistIndex > 0) {
            s_playlistIndex--;
            if (s_playlistIndex < (int)s_playlist.size())
                LoadFile(s_playlist[s_playlistIndex].c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(">|")) {
        if (!s_playlist.empty() && s_playlistIndex + 1 < (int)s_playlist.size()) {
            s_playlistIndex++;
            LoadFile(s_playlist[s_playlistIndex].c_str());
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("Loops", &s_loopCount)) {
        if (s_loopCount < 1) s_loopCount = 1;
        if (s_loopCount > 99) s_loopCount = 99;
        s_player.SetLoopCount((UINT32)s_loopCount);
        SavePlayerState();
    }

    // ---- Track info ----
    if (hasFile) {
        ImGui::SameLine();
        // Playback position
        double pos  = s_player.GetCurTime(0);
        double dur  = s_player.GetTotalTime(0);
        ImGui::Text("  %.1f / %.1f s", pos, dur);

        // File name
        const char* fname = s_loadedPath.c_str();
        const char* slash = strrchr(fname, '\\');
        if (!slash) slash = strrchr(fname, '/');
        ImGui::TextUnformatted(slash ? slash + 1 : fname);
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled(" No file loaded");
    }

    ImGui::Separator();

    // ---- File browser (takes remaining space) ----
    RenderFileBrowser();

    ImGui::End();
}

static bool  s_showChipAliasWindow = false;

void RenderChipAliasWindow() {
    if (!s_showChipAliasWindow) return;
    ImGui::SetNextWindowSize(ImVec2(280, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Chip Aliases", &s_showChipAliasWindow)) { ImGui::End(); return; }
    ImGui::TextDisabled("Override short names shown on piano keys.");
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("Label size", &s_pianoLabelFontSize, 6.0f, 20.0f, "%.0fpx"))
        SaveChipAliases();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("Bottom offset", &s_pianoLabelOffsetY, -50.0f, 200.0f, "%.0fpx"))
        SaveChipAliases();
    if (ImGui::Checkbox("Show labels on piano keys", &s_showPianoLabels))
        SaveChipAliases();
    ImGui::Separator();
    static const char* kChipNames[] = {
        "YM2612","YM2151","YM2413","YM2203","YM2608","YM2610",
        "YMF262","YM3812","YM3526","Y8950","YMF271","YMF278B",
        "AY8910","SN76489","HuC6280","SAA1099","POKEY","NES APU",
        "GB DMG","OKI6258","OKI6295","RF5C68","K051649","WSwan",
        "YMZ280B","MultiPCM","uPD7759","K054539","C140","K053260",
        "QSound","SCSP","VSU","ES5503","ES5506","X1-010",
        "C352","GA20","SegaPCM"
    };
    static const char* kDefaultAbbrevs[] = {
        "2612","2151","2413","2203","2608","2610",
        "F262","3812","3526","8950","F271","278B",
        "AY89","SN76","C628","SAA","POK","NES",
        "DMG","6258","6295","RF5C","SCC","WSW",
        "Z280","MPCM","7759","K054","C140","K053",
        "QSnd","SCSP","VSU","5503","5506","X1",
        "C352","GA20","SPCM"
    };
    static const int kNumChips = (int)(sizeof(kChipNames)/sizeof(kChipNames[0]));
    bool anyChanged = false;
    if (ImGui::BeginTable("##aliases", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Chip",  ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Abbrev",ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kNumChips; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kChipNames[i]);
            ImGui::TableNextColumn();
            char buf[6];
            auto it = s_chipAliases.find(kChipNames[i]);
            if (it != s_chipAliases.end())
                snprintf(buf, sizeof(buf), "%s", it->second.c_str());
            else
                snprintf(buf, sizeof(buf), "%s", kDefaultAbbrevs[i]);
            ImGui::SetNextItemWidth(75);
            char inputId[16]; snprintf(inputId, sizeof(inputId), "##ca%d", i);
            if (ImGui::InputText(inputId, buf, sizeof(buf))) {
                if (buf[0] && strcmp(buf, kDefaultAbbrevs[i]) != 0)
                    s_chipAliases[kChipNames[i]] = buf;
                else
                    s_chipAliases.erase(kChipNames[i]);
                anyChanged = true;
            }
        }
        ImGui::EndTable();
    }
    if (anyChanged) SaveChipAliases();
    ImGui::Separator();
    if (ImGui::Button("Clear All")) { s_chipAliases.clear(); SaveChipAliases(); }
    ImGui::End();
}

// Build a flat list of (label, level 0.0-1.0, color, keyon, note) from current shadow state
struct LevelMeterEntry {
    char label[8];
    char chip_label[16]; // non-empty only on the first channel of each chip group
    char chip_abbrev[6]; // short abbreviation for piano key display
    float level;         // 0.0 = silent, 1.0 = max
    ImU32 color;
    bool  keyon;
    bool  keyoff;        // true = recently released, show faded color on piano
    bool  group_start;   // true if this is the first meter in a chip group
    int   note;          // MIDI note number (-1 = no pitch info)
    float pitch_offset;  // fractional semitone offset for vibrato/KF visualization [-0.5, 0.5]
    float tremolo;      // AMS tremolo modulation [-1, 1] for vertical indicator
    int   voice_ch;      // modizer voice buffer channel index (-1 = no scope data)
    UINT8 devType;       // DEVID for per-chip scope settings lookup
};

// Forward declarations
static std::vector<LevelMeterEntry> BuildLevelMeters();
static std::vector<PLR_DEV_INFO> GetActiveDevices();

void RenderScopeSettingsWindow() {
    if (!s_showScopeSettingsWindow) return;
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scope Settings", &s_showScopeSettingsWindow)) {
        ImGui::End();
        return;
    }

    // Known chip types - using correct DEVID values from SoundDevs.h
    static const UINT8 knownChips[] = {
        0x00/*SN76496*/, 0x01/*YM2413*/, 0x02/*YM2612*/, 0x03/*YM2151*/,
        0x04/*SEGAPCM*/, 0x05/*RF5C68*/, 0x06/*YM2203*/, 0x07/*YM2608*/,
        0x08/*YM2610*/, 0x09/*YM3812*/, 0x0A/*YM3526*/, 0x0B/*Y8950*/,
        0x0C/*YMF262*/, 0x0D/*YMF278B*/, 0x0E/*YMF271*/, 0x0F/*YMZ280B*/,
        0x11/*32X_PWM*/, 0x12/*AY8910*/, 0x13/*GB_DMG*/, 0x14/*NES_APU*/,
        0x15/*YMW258*/, 0x16/*uPD7759*/, 0x17/*OKIM6258*/, 0x18/*OKIM6295*/,
        0x19/*K051649*/, 0x1A/*K054539*/, 0x1B/*C6280*/, 0x1C/*C140*/,
        0x1D/*K053260*/, 0x1E/*POKEY*/, 0x1F/*QSOUND*/, 0x20/*SCSP*/,
        0x21/*WSWan*/, 0x22/*VBOY_VSU*/, 0x23/*SAA1099*/, 0x24/*ES5503*/,
        0x25/*ES5506*/, 0x26/*X1_010*/, 0x27/*C352*/, 0x28/*GA20*/,
        0x29/*MIKEY*/, 0x2A/*K007232*/, 0x2B/*K005289*/,
    };
    static const int numKnownChips = sizeof(knownChips) / sizeof(knownChips[0]);

    auto chipName = [](UINT8 devType) -> const char* {
        switch(devType) {
            case 0x00: return "SN76496"; case 0x01: return "YM2413"; case 0x02: return "YM2612";
            case 0x03: return "YM2151";  case 0x04: return "SegaPCM"; case 0x05: return "RF5C68";
            case 0x06: return "YM2203";  case 0x07: return "YM2608"; case 0x08: return "YM2610";
            case 0x09: return "YM3812";  case 0x0A: return "YM3526"; case 0x0B: return "Y8950";
            case 0x0C: return "YMF262";  case 0x0D: return "YMF278B"; case 0x0E: return "YMF271";
            case 0x0F: return "YMZ280B"; case 0x11: return "32X_PWM"; case 0x12: return "AY8910";
            case 0x13: return "GB_DMG";  case 0x14: return "NES_APU"; case 0x15: return "YMW258";
            case 0x16: return "uPD7759"; case 0x17: return "OKI6258"; case 0x18: return "OKI6295";
            case 0x19: return "K051649"; case 0x1A: return "K054539"; case 0x1B: return "C6280";
            case 0x1C: return "C140";    case 0x1D: return "K053260"; case 0x1E: return "POKEY";
            case 0x1F: return "QSound";  case 0x20: return "SCSP"; case 0x21: return "WSwan";
            case 0x22: return "VBOY_VSU"; case 0x23: return "SAA1099"; case 0x24: return "ES5503";
            case 0x25: return "ES5506";  case 0x26: return "X1_010"; case 0x27: return "C352";
            case 0x28: return "GA20";    case 0x29: return "MIKEY"; case 0x2A: return "K007232";
            case 0x2B: return "K005289";
            default:   return "Unknown";
        }
    };

    // Build ordered list: active chips first (from BuildLevelMeters, which matches scope rendering), then inactive
    std::set<UINT8> activeDevTypes;
    // Cache per-chip channel labels and default colors for color picker UI
    struct ChipChannelInfo { std::vector<std::pair<std::string, ImU32>> channels; };
    std::map<UINT8, ChipChannelInfo> chipChannelInfo;
    if (s_fileLoaded) {
        // Get active chips directly from the scope rendering system
        std::vector<LevelMeterEntry> meters = BuildLevelMeters();
        for (const auto& m : meters) {
            activeDevTypes.insert(m.devType);
            chipChannelInfo[m.devType].channels.push_back({m.label, m.color});
        }
    }
    std::vector<UINT8> orderedChips;
    for (int i = 0; i < numKnownChips; i++)
        if (activeDevTypes.count(knownChips[i]))
            orderedChips.push_back(knownChips[i]);
    for (int i = 0; i < numKnownChips; i++)
        if (!activeDevTypes.count(knownChips[i]))
            orderedChips.push_back(knownChips[i]);

    // Per-chip collapsible settings: active chips expanded, inactive collapsed
    int numActive = (int)activeDevTypes.size();
    for (int i = 0; i < (int)orderedChips.size(); i++) {
        UINT8 dt = orderedChips[i];
        auto& cs = s_scopeChipSettings[dt];
        bool isActive = (i < numActive);

        std::string label = std::string(chipName(dt)) + "##chip" + std::to_string(dt);
        ImGuiTreeNodeFlags flags = isActive ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        if (!isActive) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        bool open = ImGui::CollapsingHeader(label.c_str(), flags);
        if (!isActive) ImGui::PopStyleColor();
        if (!open) continue;

        ImGui::Indent();

        int maxSamples = 4096 - cs.search_window - 64;
        if (maxSamples < 50) maxSamples = 50;

        ImGui::PushItemWidth(200.0f);
        if (ImGui::SliderFloat(("Width##w"+std::to_string(dt)).c_str(), &cs.width, 40.0f, 500.0f, "%.0fpx")) SaveConfig();

        if (ImGui::SliderFloat(("Amplitude##amp"+std::to_string(dt)).c_str(), &cs.amplitude, 0.1f, 10.0f, "%.1f")) SaveConfig();

        if (ImGui::SliderInt(("Samples##s"+std::to_string(dt)).c_str(), &cs.samples, 50, maxSamples, "%d")) SaveConfig();

        if (ImGui::SliderInt(("Offset##o"+std::to_string(dt)).c_str(), &cs.offset, 0, 4096, "%d")) SaveConfig();

        if (ImGui::SliderInt(("Search window##sw"+std::to_string(dt)).c_str(), &cs.search_window, 0, 4096, "%d")) SaveConfig();

        if (ImGui::Checkbox(("Edge-align##ea"+std::to_string(dt)).c_str(), &cs.edge_align)) SaveConfig();

        // Silence clear option - clear waveform when no new data (for PCM chips)
        if (ImGui::Checkbox(("Clear on Silence##cs"+std::to_string(dt)).c_str(), &cs.legacy_mode)) SaveConfig();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear waveform when silent (no new sample data).\nUseful for PCM chips (SegaPCM, OKIM6295, ADPCM) to prevent ghost waveforms.");
        }

        if (dt == 0x12 /* AY8910 */ || dt == 0x14 /* NES_APU */) {
            if (ImGui::Combo(("AC mode##ac"+std::to_string(dt)).c_str(), &cs.ac_mode, "Off\0Center\0Bottom\0")) SaveConfig();
        }

        if (dt == 0x12 /* AY8910 */) {
            if (ImGui::SliderInt("SSG Shift##ssg_shift", &g_ssg_scope_shift, 0, 10, ">>%d")) SaveConfig();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("SSG scope bit shift. Higher = smaller waveform, prevents clipping at high volume.");
            }
        }

        ImGui::PopItemWidth();

        // Channel color customization
        {
            auto& cci = chipChannelInfo[dt];
            if (!cci.channels.empty()) {
                ImGui::Text("Channel Colors");
                for (int ci = 0; ci < (int)cci.channels.size() && ci < 32; ci++) {
                    ImGui::PushID(ci);
                    ImU32 curCol = cs.channel_colors[ci];
                    // Initialize color from stored value; if unset, use default color at full brightness
                    float colF[4];
                    if (curCol != 0) {
                        colF[0] = ((curCol >>  0) & 0xFF) / 255.0f;
                        colF[1] = ((curCol >>  8) & 0xFF) / 255.0f;
                        colF[2] = ((curCol >> 16) & 0xFF) / 255.0f;
                        colF[3] = ((curCol >> 24) & 0xFF) / 255.0f;
                    } else {
                        ImU32 defCol = cci.channels[ci].second;
                        colF[0] = ((defCol >>  0) & 0xFF) / 255.0f;
                        colF[1] = ((defCol >>  8) & 0xFF) / 255.0f;
                        colF[2] = ((defCol >> 16) & 0xFF) / 255.0f;
                        colF[3] = 1.0f;
                    }
                    std::string editId = "##clredit" + std::to_string(dt) + "_" + std::to_string(ci);
                    if (ImGui::ColorEdit4(editId.c_str(), colF, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
                        cs.channel_colors[ci] = IM_COL32(
                            (int)(colF[0] * 255 + 0.5f),
                            (int)(colF[1] * 255 + 0.5f),
                            (int)(colF[2] * 255 + 0.5f),
                            (int)(colF[3] * 255 + 0.5f));
                        SaveConfig();
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(cci.channels[ci].first.c_str());
                    if (cs.channel_colors[ci] == 0) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(auto)");
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset")) {
                        cs.channel_colors[ci] = 0;
                        SaveConfig();
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton(("Reset All Colors##rclr"+std::to_string(dt)).c_str())) {
                    memset(cs.channel_colors, 0, sizeof(cs.channel_colors));
                    SaveConfig();
                }
                ImGui::Spacing();
            }
        }

        if (ImGui::SmallButton(("Reset##reset"+std::to_string(dt)).c_str())) {
            s_scopeChipSettings.erase(dt);
            SaveConfig();
        }

        ImGui::Unindent();
        ImGui::Spacing();
    }

    ImGui::Separator();
    if (ImGui::Button("Reset All")) {
        s_scopeChipSettings.clear();
        SaveConfig();
    }
    ImGui::End();
}

void RenderControls() {
    if (!ImGui::CollapsingHeader("libvgm (Simulation)##controls", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (ImGui::SmallButton("Aliases")) s_showChipAliasWindow = !s_showChipAliasWindow;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, s_showScope ? ImVec4(0.2f,0.7f,0.3f,1.0f) : ImVec4(0.3f,0.3f,0.3f,1.0f));
    if (ImGui::SmallButton("Scope")) { s_showScope = !s_showScope; SavePlayerState(); }
    ImGui::PopStyleColor();

    // Oscilloscope height (unified for all chips)
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Scope Height", &s_scopeHeight, 30.0f, 500.0f, "%.0fpx")) {
        SavePlayerState();
    }

    // Per-chip scope settings button (toggles non-modal window)
    ImGui::PushStyleColor(ImGuiCol_Button, s_showScopeSettingsWindow ? ImVec4(0.2f,0.7f,0.3f,1.0f) : ImVec4(0.3f,0.3f,0.3f,1.0f));
    if (ImGui::SmallButton("Scope Settings...")) {
        s_showScopeSettingsWindow = !s_showScopeSettingsWindow;
    }
    ImGui::PopStyleColor();

    ImGui::Separator();

    if (s_fileLoaded) {
        // ---- VGM Header Info ----
        const VGM_HEADER* hdr = s_vgmFile.GetHeader();
        if (hdr) {
            // Version: stored as BCD 0x0171 = v1.71
            UINT32 v = hdr->fileVer;
            ImGui::TextColored(ImVec4(0.6f,0.9f,1.0f,1.0f),
                "VGM v%X.%02X", (v >> 8) & 0xFF, v & 0xFF);

            // Total duration
            double totalSec = (double)hdr->numTicks / 44100.0;
            int tMin = (int)totalSec / 60;
            int tSec = (int)totalSec % 60;
            int tMs  = (int)((totalSec - (int)totalSec) * 1000);
            ImGui::TextDisabled("Duration:"); ImGui::SameLine();
            ImGui::Text("%02d:%02d.%03d", tMin, tSec, tMs);

            // Loop point
            if (hdr->loopOfs && hdr->loopTicks) {
                double loopSec = totalSec - (double)hdr->loopTicks / 44100.0;
                int lMin = (int)loopSec / 60;
                int lSec = (int)loopSec % 60;
                int lMs  = (int)((loopSec - (int)loopSec) * 1000);
                ImGui::TextDisabled("Loop at:"); ImGui::SameLine();
                ImGui::Text("%02d:%02d.%03d", lMin, lSec, lMs);
            } else {
                ImGui::TextDisabled("Loop:    none");
            }

            // Record Hz
            if (hdr->recordHz)
                ImGui::TextDisabled("Rate: %u Hz", hdr->recordHz);

            // Chips used (from device info)
            PlayerBase* pBase = s_player.GetPlayer();
            if (pBase) {
                std::vector<PLR_DEV_INFO> devList;
                pBase->GetSongDeviceInfo(devList);
                if (!devList.empty()) {
                    ImGui::TextDisabled("Chips:");
                    bool multiChip = (devList.size() > 1);
                    for (const auto& d : devList) {
                        const char* cn = "?";
                        switch (d.type) {
                            case DEVID_YM2612:  cn = "YM2612"; break;
                            case DEVID_YM2151:  cn = "YM2151"; break;
                            case DEVID_YM2413:  cn = "YM2413"; break;
                            case DEVID_YM2203:  cn = "YM2203"; break;
                            case DEVID_YM2608:  cn = "YM2608"; break;
                            case DEVID_YM2610:  cn = "YM2610"; break;
                            case DEVID_YMF262:  cn = "YMF262"; break;
                            case DEVID_YM3812:  cn = "YM3812"; break;
                            case DEVID_YM3526:  cn = "YM3526"; break;
                            case DEVID_Y8950:   cn = "Y8950";  break;
                            case DEVID_YMF271:  cn = "YMF271"; break;
                            case DEVID_YMF278B: cn = "YMF278B"; break;
                            case DEVID_SN76496: cn = "SN76496"; break;
                            case DEVID_AY8910:  cn = "AY8910"; break;
                            case DEVID_C6280:   cn = "C6280";  break;
                            case DEVID_SAA1099: cn = "SAA1099"; break;
                            case DEVID_POKEY:   cn = "POKEY";  break;
                            case DEVID_NES_APU: cn = "2A03";   break;
                            case DEVID_GB_DMG:  cn = "GB DMG"; break;
                            case DEVID_OKIM6258: cn = "OKIM6258"; break;
                            case DEVID_OKIM6295: cn = "OKIM6295"; break;
                            case DEVID_RF5C68:  cn = "RF5C68"; break;
                            case DEVID_K051649: cn = "SCC";    break;
                            case DEVID_WSWAN:   cn = "WSwan";  break;
                        }
                        UINT32 clk = d.devCfg ? d.devCfg->clock : 0;
                        if (!multiChip) {
                            ImGui::SameLine();
                        }
                        if (clk)
                            ImGui::Text(" %s@%.3fMHz", cn, clk / 1000000.0);
                        else
                            ImGui::Text(" %s", cn);
                    }
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ---- GD3 Tags ----
        const GD3_TAGS& tags = s_vgmFile.GetTags();
        bool hasAnyTag = !tags.track_name_en.empty() || !tags.track_name_jp.empty() ||
                         !tags.game_name_en.empty()  || !tags.game_name_jp.empty()  ||
                         !tags.system_name_en.empty()|| !tags.system_name_jp.empty()||
                         !tags.artist_en.empty()     || !tags.artist_jp.empty()     ||
                         !tags.release_date.empty()  || !tags.vgm_creator.empty()   ||
                         !tags.notes.empty();

        if (hasAnyTag) {
            // Track name (prefer EN, show JP if different / EN empty) — auto-scroll if too wide
            if (!tags.track_name_en.empty()) {
                DrawScrollingText("##tgen", tags.track_name_en.c_str(), IM_COL32(255,255,102,255));
            }
            if (!tags.track_name_jp.empty() && tags.track_name_jp != tags.track_name_en) {
                DrawScrollingText("##tgjp", tags.track_name_jp.c_str(), IM_COL32(255,255,102,191));
            }

            auto showBilingual = [](const char* label, const char* idBase,
                                    const std::string& en, const std::string& jp) {
                if (!en.empty()) {
                    std::string full = std::string(label) + en;
                    ImGui::TextDisabled("%s", label);
                    DrawScrollingText(idBase, full.c_str() + strlen(label), IM_COL32(220,220,220,255));
                }
                if (!jp.empty() && jp != en) {
                    std::string idJp = std::string(idBase) + "jp";
                    std::string fullJp = std::string(label) + jp;
                    ImGui::TextDisabled("%s", label);
                    DrawScrollingText(idJp.c_str(), fullJp.c_str() + strlen(label), IM_COL32(220,220,220,255));
                }
            };

            showBilingual("Game:  ", "##game", tags.game_name_en, tags.game_name_jp);
            showBilingual("System:", "##sys",  tags.system_name_en, tags.system_name_jp);
            showBilingual("Artist:", "##art",  tags.artist_en, tags.artist_jp);

            if (!tags.release_date.empty()) {
                ImGui::TextDisabled("Date:  ");
                DrawScrollingText("##date", tags.release_date.c_str(), IM_COL32(220,220,220,255));
            }
            if (!tags.vgm_creator.empty()) {
                ImGui::TextDisabled("Author:");
                DrawScrollingText("##author", tags.vgm_creator.c_str(), IM_COL32(220,220,220,255));
            }
            if (!tags.notes.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Notes:");
                ImGui::TextWrapped("%s", tags.notes.c_str());
            }
        } else {
            ImGui::TextDisabled("(no GD3 tags)");
        }

        ImGui::Spacing();
        ImGui::Separator();
    } else {
        ImGui::TextDisabled("[No file loaded]");
        ImGui::Spacing();
        ImGui::Text("Supported formats:");
        ImGui::TextDisabled(".vgm  .vgz");
        ImGui::TextDisabled(".s98  .gym  .dro");
        ImGui::Spacing();
        ImGui::Separator();
    }

    ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "libvgm audio simulation");
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "via DirectSound output.");
}

// Forward declaration - defined after BuildLevelMeters()
void RenderPianoArea();
static std::vector<PLR_DEV_INFO> GetActiveDevices() {
    std::vector<PLR_DEV_INFO> devList;
    if (!s_fileLoaded || s_loading) return devList;
    PlayerBase* pBase = s_player.GetPlayer();
    if (pBase) pBase->GetSongDeviceInfo(devList);

    // Fallback: if libvgm didn't register any devices (e.g. SN76489 clock=0 in header),
    // scan the parsed VgmEvents to discover which chip types / instances are actually used.
    if (devList.empty()) {
        // Collect unique (chip_type, chip_num) pairs from events
        std::map<std::pair<UINT8,UINT8>, bool> seen;
        for (const VgmEvent& ev : s_vgmFile.GetEvents()) {
            // chip_type==0 is DEVID_SN76496 (valid); events with no chip target
            // are not stored in _events (parser skips them), so all events here
            // represent real chip writes. Include all.
            seen[{ev.chip_type, ev.chip_num & 1}] = true;
        }
        for (const auto& kv : seen) {
            PLR_DEV_INFO di;
            memset(&di, 0, sizeof(di));
            di.type     = kv.first.first;
            di.instance = kv.first.second;
            di.id       = PLR_DEV_ID(di.type, di.instance);
            di.volume   = 0x100;
            devList.push_back(di);
        }
    }
    return devList;
}


// Number of modizer voice channels per chip type (for oscilloscope mapping)
static int GetModizerVoiceCount(UINT8 devType) {
    switch (devType) {
        case DEVID_YM2151:  return 8;   // 8 FM
        case DEVID_YM2203:  return 6;   // 3 FM + 3 SSG
        case DEVID_YM2608:  return 16;  // 6 FM + 3 SSG + 6 ADPCM-A + 1 ADPCM-B
        case DEVID_YM2610:  return 14;  // 4 FM + 3 SSG + 6 ADPCM-A + 1 ADPCM-B
        case DEVID_YM2612:  return 6;   // 6 FM
        case DEVID_YM2413:  return 14;  // 9 FM + 5 Rhythm (BD/SD/TOM/CYM/HH)
        case DEVID_AY8910:  return 3;   // 3 SSG
        case DEVID_SN76496: return 4;   // 3 Tone + 1 Noise
        case DEVID_K051649: return 5;   // 5 Waveform
        case DEVID_YM3812:  return 9;   // 9 FM (OPL2)
        case DEVID_YM3526:  return 9;   // 9 FM (OPL)
        case DEVID_Y8950:   return 10;  // 9 FM + 1 ADPCM
        case DEVID_YMF262:  return 18;  // 18 FM (OPL3)
        case DEVID_SEGAPCM: return 16;  // 16 channels
        case DEVID_RF5C68:  return 8;   // 8 channels (RF5C164/RF5C105)
        case DEVID_YMF278B: return 24;  // OPL4: 18 FM + 6 PCM
        case DEVID_YMF271:  return 12;  // OPX
        case DEVID_YMZ280B: return 8;   // 8 channels
        case DEVID_32X_PWM: return 2;   // 2 PWM
        case DEVID_GB_DMG:  return 4;   // 4 channels (Square1/2/Wave/Noise)
        case DEVID_NES_APU: return 6;   // 2 APU + 1 DMC + 2x triangle (ext) = 5, actually 6 with FDS
        case DEVID_YMW258:  return 28;  // MultiPCM
        case DEVID_uPD7759: return 1;   // 1 channel
        case DEVID_OKIM6258:return 1;   // 1 channel (ADPCM)
        case DEVID_OKIM6295:return 4;   // 4 channels
        case DEVID_K054539: return 8;   // 8 channels
        case DEVID_C6280:   return 6;   // 6 channels
        case DEVID_C140:    return 16;  // C140: 16 channels
        case DEVID_C219:    return 16;  // C219: 16 channels
        case DEVID_K053260: return 4;   // 4 channels
        case DEVID_POKEY:   return 4;   // 4 channels
        case DEVID_QSOUND:  return 19;  // QSound: 19 channels
        case DEVID_SCSP:    return 32;  // 32 channels
        case DEVID_WSWAN:   return 4;   // 4 channels
        case DEVID_VBOY_VSU:return 6;   // 6 channels
        case DEVID_SAA1099: return 6;   // 6 channels
        case DEVID_ES5503:  return 32;  // 32 channels
        case DEVID_ES5506:  return 32;  // 32 channels
        case DEVID_X1_010:  return 16;  // 16 channels
        case DEVID_C352:    return 32;  // 32 channels
        case DEVID_GA20:    return 4;   // 4 channels
        case DEVID_MIKEY:   return 4;   // 4 channels (Lynx)
        case DEVID_K007232: return 2;   // 2 channels
        case DEVID_K005289: return 3;   // 3 channels
        case DEVID_MSM5205: return 1;   // 1 channel
        default:            return 0;
    }
}

static std::vector<LevelMeterEntry> BuildLevelMeters() {
    std::vector<LevelMeterEntry> meters;
    if (!s_fileLoaded) return meters;

    // Helper: compute actual amplitude from oscilloscope buffer
    // Returns RMS amplitude normalized to [0, 1]
    auto getScopeAmplitude = [&](int voice_ch) -> float {
        if (voice_ch < 0 || voice_ch >= SOUND_MAXVOICES_BUFFER_FX) return 0.0f;
        if (!m_voice_buff[voice_ch]) return 0.0f;
        int64_t samples = (int)m_voice_current_ptr[voice_ch];
        if (samples < 16) return 0.0f;  // need some samples
        // Compute RMS from recent samples (last 64 samples or available)
        int count = (samples > 64) ? 64 : (int)samples;
        int start_idx = ((int)samples - count) & 0xFFF;  // buffer is 4096 bytes
        double sum_sq = 0.0;
        for (int i = 0; i < count; i++) {
            int idx = (start_idx + i) & 0xFFF;
            signed char s = m_voice_buff[voice_ch][idx];
            double v = s / 128.0;  // normalize to roughly [-1, 1]
            sum_sq += v * v;
        }
        float rms = sqrtf((float)(sum_sq / count));
        // Empirically scale: max typical RMS around 0.3-0.5 for full output
        return fminf(1.0f, rms * 3.0f);
    };

    const std::vector<PLR_DEV_INFO>& devs = GetActiveDevices();
    // Track chip type instances for dual-chip labeling (#2, #3, etc.)
    std::map<UINT8, int> chipTypeCount;  // Reset per file load
    for (const auto& dev : devs) {
        // Build human-readable chip name for group header
        const char* chipLabel  = "??";
        const char* chipAbbrev = "??";
        switch (dev.type) {
            case DEVID_YM2612:  chipLabel = "YM2612";  chipAbbrev = "2612"; break;
            case DEVID_YM2151:  chipLabel = "YM2151";  chipAbbrev = "2151"; break;
            case DEVID_YM2413:  chipLabel = "YM2413";  chipAbbrev = "2413"; break;
            case DEVID_YM2203:  chipLabel = "YM2203";  chipAbbrev = "2203"; break;
            case DEVID_YM2608:  chipLabel = "YM2608";  chipAbbrev = "2608"; break;
            case DEVID_YM2610:  chipLabel = "YM2610";  chipAbbrev = "2610"; break;
            case DEVID_YMF262:  chipLabel = "YMF262";  chipAbbrev = "F262"; break;
            case DEVID_YM3812:  chipLabel = "YM3812";  chipAbbrev = "3812"; break;
            case DEVID_YM3526:  chipLabel = "YM3526";  chipAbbrev = "3526"; break;
            case DEVID_Y8950:   chipLabel = "Y8950";   chipAbbrev = "8950"; break;
            case DEVID_YMF271:  chipLabel = "YMF271";  chipAbbrev = "F271"; break;
            case DEVID_YMF278B: chipLabel = "YMF278B"; chipAbbrev = "278B"; break;
            case DEVID_AY8910:  chipLabel = "AY8910";  chipAbbrev = "AY89"; break;
            case DEVID_SN76496: chipLabel = "SN76489"; chipAbbrev = "SN76"; break;
            case DEVID_C6280:   chipLabel = "HuC6280"; chipAbbrev = "C628"; break;
            case DEVID_SAA1099: chipLabel = "SAA1099"; chipAbbrev = "SAA";  break;
            case DEVID_POKEY:   chipLabel = "POKEY";   chipAbbrev = "POK";  break;
            case DEVID_NES_APU: chipLabel = "NES APU"; chipAbbrev = "NES";  break;
            case DEVID_GB_DMG:  chipLabel = "GB DMG";  chipAbbrev = "DMG";  break;
            case DEVID_OKIM6258:chipLabel = "OKI6258"; chipAbbrev = "6258"; break;
            case DEVID_OKIM6295:chipLabel = "OKI6295"; chipAbbrev = "6295"; break;
            case DEVID_RF5C68:  chipLabel = "RF5C68";  chipAbbrev = "RF5C"; break;
            case DEVID_K051649: chipLabel = "K051649"; chipAbbrev = "SCC";  break;
            case DEVID_WSWAN:   chipLabel = "WSwan";   chipAbbrev = "WSW";  break;
            case DEVID_YMZ280B: chipLabel = "YMZ280B"; chipAbbrev = "Z280"; break;
            case DEVID_YMW258:  chipLabel = "MultiPCM";chipAbbrev = "MPCM"; break;
            case DEVID_uPD7759: chipLabel = "uPD7759"; chipAbbrev = "7759"; break;
            case DEVID_K054539: chipLabel = "K054539"; chipAbbrev = "K054"; break;
            case DEVID_C140:    chipLabel = "C140";    chipAbbrev = "C140"; break;
            case DEVID_K053260: chipLabel = "K053260"; chipAbbrev = "K053"; break;
            case DEVID_QSOUND:  chipLabel = "QSound";  chipAbbrev = "QSnd"; break;
            case DEVID_SCSP:    chipLabel = "SCSP";    chipAbbrev = "SCSP"; break;
            case DEVID_VBOY_VSU:chipLabel = "VSU";     chipAbbrev = "VSU";  break;
            case DEVID_ES5503:  chipLabel = "ES5503";  chipAbbrev = "5503"; break;
            case DEVID_ES5506:  chipLabel = "ES5506";  chipAbbrev = "5506"; break;
            case DEVID_X1_010:  chipLabel = "X1-010";  chipAbbrev = "X1";   break;
            case DEVID_C352:    chipLabel = "C352";    chipAbbrev = "C352"; break;
            case DEVID_GA20:    chipLabel = "GA20";    chipAbbrev = "GA20"; break;
            case DEVID_SEGAPCM: chipLabel = "SegaPCM"; chipAbbrev = "SPCM"; break;
            case DEVID_32X_PWM: chipLabel = "32X PWM"; chipAbbrev = "PWM"; break;
            default: break;
        }
        // Apply user alias override if set
        auto aliasIt = s_chipAliases.find(chipLabel);
        if (aliasIt != s_chipAliases.end() && !aliasIt->second.empty())
            chipAbbrev = aliasIt->second.c_str();
        // Track chip type instances for dual-chip labeling (#1, #2, #3, etc.)
        int instNum = ++chipTypeCount[dev.type];
        // Add instance suffix for all chips (e.g., "YM2203 #1", "YM2203 #2")
        char chipLabelBuf[32];
        snprintf(chipLabelBuf, sizeof(chipLabelBuf), "%s #%d", chipLabel, instNum);
        chipLabel = chipLabelBuf;
        bool firstInGroup = true;
        int  groupChIdx = 0; // channel index within this chip group (for color variation)
        // Shift hue of a color by rotating R->G->B components with a small per-channel offset.
        // step 0 = base color, step 1..N = progressively shifted hue.
        auto shiftHue = [](ImU32 col, int step) -> ImU32 {
            if (step == 0) return col;
            int r = (col >> IM_COL32_R_SHIFT) & 0xFF;
            int g = (col >> IM_COL32_G_SHIFT) & 0xFF;
            int b = (col >> IM_COL32_B_SHIFT) & 0xFF;
            int a = (col >> IM_COL32_A_SHIFT) & 0xFF;
            // Rotate hue by ~30° per step: blend toward next primary in RGB cycle
            const int kShift = 40; // amount per step
            int delta = (step * kShift) % 256;
            // Simple hue rotation: shift R->G->B cyclically
            int nr = (r - delta + 256) % 256;
            int ng = (g + delta / 2) % 256;
            int nb = (b + delta / 3) % 256;
            // Clamp
            if (nr > 255) nr = 255;
            if (ng > 255) ng = 255;
            if (nb > 255) nb = 255;
            return IM_COL32(nr, ng, nb, a);
        };
        // Update ChVizState for chips without key_on_event (detect pitch change)
        // Returns poff for add() call. display_nt is set to roundf(vs.start_note) when active.
        auto updateVizPSG = [](ChVizState& vs, bool kon, int nt) -> float {
            if (!kon || nt < 0) {
                vs.key_on = false;
                vs.decay *= 0.85f;
                if (vs.decay < 0.01f) vs.decay = 0.0f;
                vs.visual_note += (0.0f - vs.visual_note) * 0.3f;
                return 0.0f;
            }
            float fnt = (float)nt;
            if (!vs.key_on) {
                // fresh keyon
                vs.key_on      = true;
                vs.target_note = fnt;
                vs.visual_note = fnt;
                vs.start_note  = fnt;
                vs.decay       = 1.0f;
            } else if (fabsf(fnt - vs.target_note) > 0.5f) {
                // pitch jumped > half semitone: treat as new note anchor
                vs.start_note  = fnt;
                vs.target_note = fnt;
                vs.decay       = 1.0f;
            } else {
                vs.target_note = fnt;
            }
            if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
            else
                vs.visual_note = vs.target_note;
            float poff = vs.visual_note - vs.start_note;
            if (poff >  1.0f) poff =  1.0f;
            if (poff < -1.0f) poff = -1.0f;
            return poff;
        };
        auto add = [&](const char* pfx, int ch, float lv, ImU32 col, bool kon, int nt = -1, float poff = 0.0f, float trem = 0.0f, bool koff = false, int voice_ch = -1) {
            LevelMeterEntry e;
            // Drum channels (2-char uppercase labels like BD, HH, SD, TM, CY, RM): no numeric suffix
            if (pfx[0] >= 'A' && pfx[0] <= 'Z' && pfx[1] >= 'A' && pfx[1] <= 'Z' &&
                (pfx[2] == '\0' || (pfx[2] >= 'A' && pfx[2] <= 'Z' && pfx[3] == '\0'))) {
                snprintf(e.label, sizeof(e.label), "%s", pfx);
            } else {
                snprintf(e.label, sizeof(e.label), "%s%d", pfx, ch);
            }
            snprintf(e.chip_abbrev, sizeof(e.chip_abbrev), "%s", chipAbbrev);
            if (firstInGroup) {
                snprintf(e.chip_label, sizeof(e.chip_label), "%s", chipLabel);
                e.group_start = true;
                firstInGroup = false;
            } else {
                e.chip_label[0] = '\0';
                e.group_start = false;
            }
            e.level = lv;
            { ImU32 uc = s_scopeChipSettings[dev.type].channel_colors[groupChIdx];
              e.color = (uc != 0) ? uc : shiftHue(col, groupChIdx); }
            e.keyon = kon; e.keyoff = koff; e.note = nt; e.pitch_offset = poff; e.tremolo = trem;
            e.voice_ch = voice_ch;
            e.devType = dev.type;
            groupChIdx++;
            meters.push_back(e);
        };
        // Helper: compute MIDI note from OPN-family F-Number + Block
        // freq = clock / 72 * fnum / 2^(20-block)  [matches imgui-vgmplayer reference]
        auto opnNote = [&](UINT8 fnumLo, UINT8 fnumHi, UINT32 clock) -> int {
            if (clock == 0) clock = 7670454;
            uint16_t fnum  = ((fnumHi & 0x07) << 8) | fnumLo;
            uint8_t  block = (fnumHi >> 3) & 0x07;
            if (fnum == 0) return -1;
            // OPN freq: clock / 72 * fnum / 2^(20-block) -> shift down 1 octave per MDPlayer: 2^(21-block)
            float freq = (float)clock / 72.0f * (float)fnum / (float)(1 << (21 - block));
            return FrequencyToMIDINote(freq);
        };
        // Helper: compute MIDI note from OPL F-Number + Block
        // freq = fnum * clock / (72 * 2^(19-block))  [MDPlayer verified formula]
        auto oplNote = [&](UINT8 fnumLo, UINT8 fnumHi, UINT32 clock) -> int {
            if (clock == 0) clock = 3579545;
            uint16_t fnum  = ((fnumHi & 0x03) << 8) | fnumLo;
            uint8_t  block = (fnumHi >> 2) & 0x07;
            if (fnum == 0) return -1;
            float freq = (float)(fnum * clock) / (float)(72 * (1 << (19 - block)));
            return FrequencyToMIDINote(freq);
        };
        // FM level: derive from TL (total level) of OP4 carrier (lower TL = louder)
        // Volume: 0=max, 127=silent for OPN; invert to 0.0-1.0
        switch (dev.type) {
            case DEVID_YM2612: {
                int c = dev.instance & 1;
                // Compute voice buffer offset for this chip instance
                // Voice offsets are sequential: YM2612[0] = 0-5, YM2612[1] = 6-11, etc.
                int voice_base = 0;
                for (const auto& d : devs) {
                    if (d.type == DEVID_YM2612 && (d.instance & 1) < c) {
                        voice_base += GetModizerVoiceCount(d.type);
                    } else if (d.type < DEVID_YM2612) {
                        voice_base += GetModizerVoiceCount(d.type);
                    }
                    if ((d.type == DEVID_YM2612 && (d.instance & 1) == c)) break;
                }

                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 7670454;
                // LFO: reg 0x22 bit3=enable, bits2:0=freq index
                static const float kYM2612LfoHz[8] = {3.98f,5.56f,6.02f,6.37f,6.88f,9.63f,48.1f,72.2f};
                UINT8 lfo_reg = s_shadowYM2612[c][0][0x22];
                bool  lfo_on  = (lfo_reg >> 3) & 1;
                float lfo_hz  = lfo_on ? kYM2612LfoHz[lfo_reg & 0x07] : 0.0f;
                static const UINT8 kOPNCarrierMask[8] = {0x8,0x8,0x8,0x8, 0xC,0xE,0xE,0xF};
                static const int   kOPNTLOff[4]       = {0x90,0x94,0x98,0x9C};
                for (int ch = 0; ch < 6; ch++) {
                    int port = ch < 3 ? 0 : 1;
                    int off  = ch % 3;
                    UINT8 algo = s_shadowYM2612[c][port][0xB0 + off] & 0x07;
                    UINT8 cmask = kOPNCarrierMask[algo];
                    UINT8 tl = 0x7F;
                    for (int op = 0; op < 4; op++)
                        if ((cmask >> (3 - op)) & 1)
                            tl = std::min(tl, (UINT8)(s_shadowYM2612[c][port][kOPNTLOff[op] + off] & 0x7F));
                    ChVizState& vs = s_ym2612_viz[c][ch];
                    // Keyoff: accelerate decay; normal sustain: slow decay
                    if (s_ym2612KeyOff[c][ch])
                        vs.decay *= 0.60f;  // fast release
                    else
                        vs.decay *= 0.90f;  // slow sustain
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    int nt    = s_ym2612KeyOn[c][ch] ? opnNote(s_shadowYM2612[c][port][0xA0+off], s_shadowYM2612[c][port][0xA4+off], clk) : -1;
                    if (nt >= 0) nt -= 12;  // YM2612: shift down 1 octave for display
                    // PMS: reg 0xB4+off port0, bits2:0
                    int pms = s_shadowYM2612[c][port][0xB4 + off] & 0x07;
                    // AMS: reg 0xB4+off port0, bits4:3
                    static const int kAMSShift[4] = {8, 3, 1, 0};
                    int ams_idx = (s_shadowYM2612[c][port][0xB4 + off] >> 4) & 0x03;
                    int ams_shift = kAMSShift[ams_idx];
                    // Simulate LFO triangle wave for AM (0..126)
                    float lfo_phase = fmodf((float)ImGui::GetTime() * lfo_hz, 1.0f);
                    float lfo_am_tri = (lfo_phase < 0.5f) ? (1.0f - lfo_phase * 2.0f) : ((lfo_phase - 0.5f) * 2.0f);
                    float am_mod = (lfo_on && ams_idx > 0) ? (lfo_am_tri * 126.0f / (float)(1 << ams_shift)) : 0.0f;
                    // Use oscilloscope amplitude for accurate level (replaces register-based estimation)
                    int voice_ch = voice_base + ch;
                    float lv_scope = getScopeAmplitude(voice_ch);
                    // Fallback to register-based level if scope has no data
                    float lv_reg  = (1.0f - (tl + am_mod) / 127.0f) * vs.decay;
                    float lv  = (lv_scope > 0.01f) ? lv_scope : lv_reg;
                    if (lv < 0.0f) lv = 0.0f;
                    if (lv > 1.0f) lv = 1.0f;
                    if (vs.key_on_event) {
                        vs.target_note    = (float)nt;
                        vs.visual_note    = (float)nt;
                        vs.start_note     = (float)nt;
                        vs.vibrato_offset = 0.0f;
                        vs.key_on_event   = false;
                    } else if (s_ym2612KeyOn[c][ch] && nt >= 0) {
                        vs.target_note = (float)nt;
                    }
                    vs.key_on = s_ym2612KeyOn[c][ch];
                    if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                        vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
                    else
                        vs.visual_note = vs.target_note;
                    bool vibrato_active = s_ym2612KeyOn[c][ch] && lfo_on && pms > 0;
                    if (vibrato_active) {
                        float max_offset = (pms / 7.0f) * 0.5f;
                        vs.vibrato_offset = sinf((float)ImGui::GetTime() * 2.0f * 3.14159f * lfo_hz) * max_offset;
                    } else {
                        vs.vibrato_offset *= 0.9f;
                        if (fabsf(vs.vibrato_offset) < 0.01f) vs.vibrato_offset = 0.0f;
                    }
                    int display_note = (nt >= 0) ? (int)roundf(vs.start_note) : nt;
                    // keyoff: keep showing on piano with faded color while decay > 0
                    bool isKeyOff = s_ym2612KeyOff[c][ch] && vs.decay > 0.01f;
                    if (isKeyOff) display_note = (int)roundf(vs.start_note); // keep last note
                    float poff = (nt >= 0) ? (vs.visual_note - vs.start_note) + vs.vibrato_offset : 0.0f;
                    if (poff >  1.0f) poff =  1.0f;
                    if (poff < -1.0f) poff = -1.0f;
                    float trem = (lfo_on && ams_idx > 0) ? (lfo_am_tri * 2.0f - 1.0f) : 0.0f;
                    if (ch == 5 && (s_shadowYM2612[c][0][0x2B] & 0x80)) {
                        // DAC mode: channel 6 outputs DAC data
                        add("DA", 1, lv, IM_COL32(200,200,80,255), true, -1, 0.0f);
                    } else {
                        add("F", ch+1, lv, IM_COL32(80,200,80,255), s_ym2612KeyOn[c][ch] || isKeyOff, display_note, poff, trem, isKeyOff);
                    }
                }
                break;
            }
            case DEVID_YM2151: {
                int c = dev.instance & 1;
                // OPM LFO params (global)
                UINT8 lfo_reg = s_shadowYM2151[c][0x18];
                bool  lfo_on  = (lfo_reg & 0x03) != 0;
                float lfo_hz  = lfo_on ? powf(2.0f, (lfo_reg & 0xFF) / 32.0f) : 0.0f;
                UINT8 pmd     = s_shadowYM2151[c][0x1B] & 0x7F;
                double now    = ImGui::GetTime();
                for (int ch = 0; ch < 8; ch++) {
                    // pick carrier TL by algorithm: carriers connect to output; take min TL (= loudest)
                    UINT8 algo2151 = s_shadowYM2151[c][0x20 + ch] & 0x07;
                    // carrier op indices per algorithm (M1=0,M2=1,C1=2,C2=3)
                    // algo 0-3: C2 only; algo 4: C1,C2; algo 5-6: M2,C1,C2; algo 7: all
                    static const UINT8 kCarrierMask[8] = {0x8,0x8,0x8,0x8, 0xC,0xE,0xE,0xF};
                    UINT8 cmask = kCarrierMask[algo2151];
                    UINT8 tl = 0x7F;
                    for (int op = 0; op < 4; op++)
                        if ((cmask >> (3 - op)) & 1)
                            tl = std::min(tl, (UINT8)(s_shadowYM2151[c][0x60 + op*8 + ch] & 0x7F));
                    // KC -> coarse note
                    UINT8 kc = s_shadowYM2151[c][0x28 + ch];
                    int oct      = (kc >> 4) & 0x07;
                    int note_idx = kc & 0x0F;
                    int semi = (note_idx < 3) ? note_idx : (note_idx < 7 ? note_idx - 1 : (note_idx < 11 ? note_idx - 2 : note_idx - 3));
                    // KF fine tune (6-bit, reg 0x30+ch bits7:2)
                    int kf = (s_shadowYM2151[c][0x30 + ch] >> 2) & 0x3F;
                    float target_note = (float)(oct * 12 + semi + 24) + kf / 64.0f;  // +24 = 2 octaves up
                    // Per-channel viz state
                    ChVizState& vs = s_ym2151_viz[c][ch];
                    // Keyoff: accelerate decay; normal sustain: slow decay
                    if (s_ym2151KeyOff[c][ch])
                        vs.decay *= 0.60f;  // fast release
                    else
                        vs.decay *= 0.90f;  // slow sustain
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float lv = (1.0f - tl / 127.0f) * vs.decay;
                    bool kon = s_ym2151KeyOn[c][ch];
                    if (vs.key_on_event) {
                        vs.visual_note    = target_note;
                        vs.target_note    = target_note;
                        vs.start_note     = target_note;
                        vs.vibrato_offset = 0.0f;
                        vs.key_on_event   = false;
                    } else if (kon) {
                        vs.target_note = target_note;
                        // smooth portamento
                        if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                            vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
                        else
                            vs.visual_note = vs.target_note;
                    }
                    vs.key_on = kon;
                    // LFO vibrato
                    int pms = (s_shadowYM2151[c][0x38 + ch] >> 4) & 0x07;
                    if (kon && lfo_on && pms > 0) {
                        float max_off = (pmd / 127.0f) * (pms / 7.0f) * 0.5f;
                        vs.vibrato_offset = sinf((float)(now * 2.0 * 3.14159265 * lfo_hz)) * max_off;
                    } else {
                        vs.vibrato_offset *= 0.9f;
                        if (fabsf(vs.vibrato_offset) < 0.01f) vs.vibrato_offset = 0.0f;
                    }
                    int nt = kon ? (int)roundf(vs.start_note) : -1;
                    if (nt >= 0) { if (nt > 127) nt = 127; }
                    float poff = kon ? ((vs.visual_note - vs.start_note) + vs.vibrato_offset) : 0.0f;
                    if (poff >  1.0f) poff =  1.0f;
                    if (poff < -1.0f) poff = -1.0f;
                    add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, nt, poff);
                }
                break;
            }
            case DEVID_YM2413: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
                UINT8 rh2413 = s_shadowYM2413[c][0x0E];
                bool  rhMode = (rh2413 >> 5) & 1;
                for (int ch = 0; ch < 9; ch++) {
                    UINT8 lo   = s_shadowYM2413[c][0x10 + ch];
                    UINT8 hi   = s_shadowYM2413[c][0x20 + ch];
                    UINT8 vol  = s_shadowYM2413[c][0x30 + ch] & 0x0F;
                    int fnum2413 = ((hi & 0x01) << 8) | lo;
                    int blk2413  = (hi >> 1) & 0x07;
                    int nt = (fnum2413 > 0) ? FrequencyToMIDINote(
                        (float)fnum2413 * (float)clk / (float)(72 * (1 << (19 - blk2413)))) : -1;
                    ChVizState& vs = s_ym2413_viz[c][ch];
                    bool sus = (hi >> 5) & 1; // SUS bit: sustain after keyoff
                    bool keyoff = s_ym2413KeyOff[c][ch];
                    // MDPlayer method: keyoff + no SUS -> immediately kill note
                    if (keyoff && !sus) {
                        vs.decay = 0.0f;
                    } else if (keyoff) {
                        // SUS=1: slow release decay
                        vs.decay *= 0.85f;
                        if (vs.decay < 0.01f) vs.decay = 0.0f;
                    } else {
                        // keyon: sustain at near-full
                        vs.decay *= 0.98f;
                        if (vs.decay < 0.01f) vs.decay = 0.0f;
                    }
                    // Consume key_on_event: reset note anchors so repeated notes flash
                    if (vs.key_on_event) {
                        vs.target_note    = (float)nt;
                        vs.visual_note    = (float)nt;
                        vs.start_note     = (float)nt;
                        vs.vibrato_offset = 0.0f;
                        vs.key_on_event   = false;
                    } else if (!keyoff && nt >= 0) {
                        vs.target_note = (float)nt;
                    }
                    if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                        vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
                    else
                        vs.visual_note = vs.target_note;
                    bool kon = vs.decay > 0.01f;
                    float lv = (1.0f - vol / 15.0f) * vs.decay;
                    int dnt  = kon ? (int)roundf(vs.start_note) : nt;
                    float poff = kon ? (vs.visual_note - vs.start_note) : 0.0f;
                    if (poff >  1.0f) poff =  1.0f;
                    if (poff < -1.0f) poff = -1.0f;
                    add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, dnt, poff);
                }
                // Always show rhythm channels (BD/HH/SD/TM/CY) even if rhMode is false
                // to keep oscilloscope window count stable
                static const char* kRh2413[5] = {"BD","HH","SD","TM","CY"};
                UINT8 bdVol  = (s_shadowYM2413[c][0x36] >> 4) & 0x0F;
                UINT8 hhVol  = (s_shadowYM2413[c][0x37] >> 4) & 0x0F;
                UINT8 sdVol  =  s_shadowYM2413[c][0x37] & 0x0F;
                UINT8 tomVol = (s_shadowYM2413[c][0x38] >> 4) & 0x0F;
                UINT8 cymVol =  s_shadowYM2413[c][0x38] & 0x0F;
                UINT8 rhVols[5] = {bdVol, hhVol, sdVol, tomVol, cymVol};
                for (int i = 0; i < 5; i++) {
                    ChVizState& rv = s_ym2413Rhy_viz[c][i];
                    if (rhMode && rv.key_on_event) {
                        // Rising edge pulse: reset decay to full (MDPlayer: volume = 19-tl)
                        rv.decay = 1.0f;
                        rv.key_on_event = false;
                    } else {
                        // No keyon: linear-style decay each frame
                        rv.decay *= 0.80f;
                        if (rv.decay < 0.01f) rv.decay = 0.0f;
                    }
                    float lv = (1.0f - rhVols[i] / 15.0f) * rv.decay;
                    // Rhythm channels: localCh = 9 + i (BD=9, HH=10, SD=11, TOM=12, CYM=13)
                    add(kRh2413[i], 0, lv, IM_COL32(200,150,80,255), rv.decay > 0.0f, -1, 0.0f, 0.0f, false, -1);
                }
                break;
            }
            case DEVID_YM2203: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3993600;
                static const UINT8 kOPNCarrierMask2[8] = {0x8,0x8,0x8,0x8, 0xC,0xE,0xE,0xF};
                static const int   kOPNTLOff2[4]        = {0x90,0x94,0x98,0x9C};
                // YM2203 LFO: reg 0x22 bits3=enable, bits2:0=freq
                UINT8 lfo2203 = s_shadowYM2203[c][0x22];
                bool  lfo2203_on = (lfo2203 >> 3) & 1;
                float lfo2203_hz = lfo2203_on ? powf(2.0f, (lfo2203 & 0x07) / 1.0f) * 0.5f : 0.0f;
                double now2203 = ImGui::GetTime();
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 algo = s_shadowYM2203[c][0xB0 + ch] & 0x07;
                    UINT8 cmask = kOPNCarrierMask2[algo];
                    UINT8 tl = 0x7F;
                    for (int op = 0; op < 4; op++)
                        if ((cmask >> (3 - op)) & 1)
                            tl = std::min(tl, (UINT8)(s_shadowYM2203[c][kOPNTLOff2[op] + ch] & 0x7F));
                    bool kon = s_ym2203KeyOn[c][ch];
                    ChVizState& vs = s_ym2203_viz[c][ch];
                    // Keyoff: accelerate decay; normal sustain: slow decay
                    if (s_ym2203KeyOff[c][ch])
                        vs.decay *= 0.60f;  // fast release
                    else
                        vs.decay *= 0.90f;  // slow sustain
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float lv = (1.0f - tl / 127.0f) * vs.decay;
                    int nt = kon ? opnNote(s_shadowYM2203[c][0xA0+ch], s_shadowYM2203[c][0xA4+ch], clk) : -1;
                    int pms = s_shadowYM2203[c][0xB4 + ch] & 0x07;
                    if (vs.key_on_event) {
                        vs.target_note    = (float)nt;
                        vs.visual_note    = (float)nt;
                        vs.start_note     = (float)nt;
                        vs.vibrato_offset = 0.0f;
                        vs.key_on_event   = false;
                    } else if (kon && nt >= 0) {
                        vs.target_note = (float)nt;
                    }
                    vs.key_on = kon;
                    if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                        vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
                    else
                        vs.visual_note = vs.target_note;
                    if (kon && lfo2203_on && pms > 0) {
                        float max_off = (pms / 7.0f) * 0.5f;
                        vs.vibrato_offset = sinf((float)(now2203 * 2.0 * 3.14159265 * lfo2203_hz)) * max_off;
                    } else {
                        vs.vibrato_offset *= 0.9f;
                        if (fabsf(vs.vibrato_offset) < 0.01f) vs.vibrato_offset = 0.0f;
                    }
                    int display_nt = (nt >= 0) ? (int)roundf(vs.start_note) : nt;
                    float poff = (nt >= 0) ? (vs.visual_note - vs.start_note) + vs.vibrato_offset : 0.0f;
                    if (poff >  1.0f) poff =  1.0f;
                    if (poff < -1.0f) poff = -1.0f;
                    add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, display_nt, poff);
                }
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 vol = s_shadowYM2203[c][0x08 + ch] & 0x0F;
                    bool on = vol > 0;
                    UINT8 plo = s_shadowYM2203[c][0x00 + ch * 2];
                    UINT8 phi = s_shadowYM2203[c][0x01 + ch * 2] & 0x0F;
                    uint16_t period = (phi << 8) | plo;
                    int nt = (on && period > 0) ? FrequencyToMIDINote((float)clk / (32.0f * period)) : -1;

                    ChVizState& vs = s_ym2203_ssg_viz[c][ch];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    float target = vol / 15.0f;
                    vs.smooth_vol += (target - vs.smooth_vol) * 0.3f;
                    add("S", ch+1, vs.smooth_vol, IM_COL32(100,180,255,255), on, dnt, poff);
                }
                break;
            }
            case DEVID_YM2608: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 8000000;
                static const UINT8 kOPNCarrierMask3[8] = {0x8,0x8,0x8,0x8, 0xC,0xE,0xE,0xF};
                static const int   kOPNTLOff3[4]        = {0x90,0x94,0x98,0x9C};
                // YM2608 LFO: reg 0x22 bits3=enable, bits2:0=freq (same as YM2612)
                UINT8 lfo2608 = s_shadowYM2608[c][0][0x22];
                bool  lfo2608_on = (lfo2608 >> 3) & 1;
                float lfo2608_hz = lfo2608_on ? powf(2.0f, (lfo2608 & 0x07) / 1.0f) * 0.5f : 0.0f;
                double now2608 = ImGui::GetTime();
                for (int ch = 0; ch < 6; ch++) {
                    int port = ch < 3 ? 0 : 1; int off = ch % 3;
                    UINT8 algo = s_shadowYM2608[c][port][0xB0 + off] & 0x07;
                    UINT8 cmask = kOPNCarrierMask3[algo];
                    UINT8 tl = 0x7F;
                    for (int op = 0; op < 4; op++)
                        if ((cmask >> (3 - op)) & 1)
                            tl = std::min(tl, (UINT8)(s_shadowYM2608[c][port][kOPNTLOff3[op] + off] & 0x7F));
                    bool kon = s_ym2608KeyOn[c][ch];
                    ChVizState& vs = s_ym2608_viz[c][ch];
                    if (s_ym2608KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float lv = (1.0f - tl / 127.0f) * vs.decay;
                    int nt = kon ? opnNote(s_shadowYM2608[c][port][0xA0+off], s_shadowYM2608[c][port][0xA4+off], clk) : -1;
                    int pms = s_shadowYM2608[c][port][0xB4 + off] & 0x07;
                    if (vs.key_on_event) {
                        vs.target_note    = (float)nt;
                        vs.visual_note    = (float)nt;
                        vs.start_note     = (float)nt;
                        vs.vibrato_offset = 0.0f;
                        vs.key_on_event   = false;
                    } else if (kon && nt >= 0) {
                        vs.target_note = (float)nt;
                    }
                    vs.key_on = kon;
                    if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                        vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
                    else
                        vs.visual_note = vs.target_note;
                    if (kon && lfo2608_on && pms > 0) {
                        float max_off = (pms / 7.0f) * 0.5f;
                        vs.vibrato_offset = sinf((float)(now2608 * 2.0 * 3.14159265 * lfo2608_hz)) * max_off;
                    } else {
                        vs.vibrato_offset *= 0.9f;
                        if (fabsf(vs.vibrato_offset) < 0.01f) vs.vibrato_offset = 0.0f;
                    }
                    int display_nt = (nt >= 0) ? (int)roundf(vs.start_note) : nt;
                    float poff = (nt >= 0) ? (vs.visual_note - vs.start_note) + vs.vibrato_offset : 0.0f;
                    if (poff >  1.0f) poff =  1.0f;
                    if (poff < -1.0f) poff = -1.0f;
                    add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, display_nt, poff);
                }
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 vol = s_shadowYM2608[c][0][0x08 + ch] & 0x0F;
                    bool on = vol > 0;

                    UINT8 plo = s_shadowYM2608[c][0][0x00 + ch * 2];
                    UINT8 phi = s_shadowYM2608[c][0][0x01 + ch * 2] & 0x0F;
                    uint16_t period = (phi << 8) | plo;
                    int nt = (on && period > 0) ? FrequencyToMIDINote((float)clk / (64.0f * period)) : -1;

                    ChVizState& vs = s_ym2608_ssg_viz[c][ch];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    float target = vol / 15.0f;
                    vs.smooth_vol += (target - vs.smooth_vol) * 0.3f;
                    add("S", ch+1, vs.smooth_vol, IM_COL32(100,180,255,255), on, dnt, poff);
                }
                {
                    static const char* kRhy2608[6] = {"BD","SD","CY","HH","TM","RM"};
                    UINT8 masterVol = s_shadowYM2608[c][0][0x11] & 0x3F;
                    float mvScale = (masterVol > 0) ? (masterVol / 63.0f) : 1.0f;
                    for (int ch = 0; ch < 6; ch++) {
                        float& decay = s_ym2608RhyKeyOn[c][ch];
                        decay *= 0.75f;  // per-frame decay (fast for percussion)
                        if (decay < 0.01f) decay = 0.0f;
                        bool  kon = decay > 0.0f;
                        UINT8 vol = s_shadowYM2608[c][0][0x18 + ch] & 0x1F;
                        float lv  = kon ? (vol / 31.0f) * mvScale * decay : 0.0f;
                        add(kRhy2608[ch], 0, lv, IM_COL32(255,160,60,255), kon);
                    }
                }
                {
                    // ADPCM-B
                    UINT8 vol = s_shadowYM2608[c][1][0x0B] & 0xFF;
                    ChVizState& vs = s_ym2608_adpcmb_viz[c];
                    if (s_ym2608_adpcmbKeyOff[c]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;

                    UINT16 deltaN = (s_shadowYM2608[c][1][0x11] << 8) | s_shadowYM2608[c][1][0x10];
                    float freq = (float)deltaN * (float)clk / (65536.0f * 144.0f);
                    int nt = (vs.decay > 0.01f && freq > 20.0f) ? FrequencyToMIDINote(freq) : -1;

                    float lv = (vol / 255.0f) * vs.decay;
                    add("B", 0, lv, IM_COL32(255,200,80,255), vs.decay > 0.01f, nt, 0.0f);
                }
                break;
            }
            case DEVID_YM2610: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 8000000;
                static const UINT8 kOPNCarrierMask4[8] = {0x8,0x8,0x8,0x8, 0xC,0xE,0xE,0xF};
                static const int   kOPNTLOff4[4]        = {0x90,0x94,0x98,0x9C};
                // YM2610 LFO: reg 0x22 bits3=enable, bits2:0=freq
                UINT8 lfo2610 = s_shadowYM2610[c][0][0x22];
                bool  lfo2610_on = (lfo2610 >> 3) & 1;
                float lfo2610_hz = lfo2610_on ? powf(2.0f, (lfo2610 & 0x07) / 1.0f) * 0.5f : 0.0f;
                double now2610 = ImGui::GetTime();
                for (int ch = 0; ch < 4; ch++) {
                    int port = ch < 3 ? 0 : 1; int off = ch % 3;
                    UINT8 algo = s_shadowYM2610[c][port][0xB0 + off] & 0x07;
                    UINT8 cmask = kOPNCarrierMask4[algo];
                    UINT8 tl = 0x7F;
                    for (int op = 0; op < 4; op++)
                        if ((cmask >> (3 - op)) & 1)
                            tl = std::min(tl, (UINT8)(s_shadowYM2610[c][port][kOPNTLOff4[op] + off] & 0x7F));
                    bool kon = s_ym2610KeyOn[c][ch];
                    ChVizState& vs = s_ym2610_viz[c][ch];
                    // Keyoff: accelerate decay; normal sustain: slow decay
                    if (s_ym2610KeyOff[c][ch])
                        vs.decay *= 0.60f;  // fast release
                    else
                        vs.decay *= 0.90f;  // slow sustain
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float lv = (1.0f - tl / 127.0f) * vs.decay;
                    int nt = kon ? opnNote(s_shadowYM2610[c][port][0xA0+off], s_shadowYM2610[c][port][0xA4+off], clk) : -1;
                    int pms = s_shadowYM2610[c][port][0xB4 + off] & 0x07;
                    if (vs.key_on_event) {
                        vs.target_note    = (float)nt;
                        vs.visual_note    = (float)nt;
                        vs.start_note     = (float)nt;
                        vs.vibrato_offset = 0.0f;
                        vs.key_on_event   = false;
                    } else if (kon && nt >= 0) {
                        vs.target_note = (float)nt;
                    }
                    vs.key_on = kon;
                    if (fabsf(vs.target_note - vs.visual_note) > 0.001f)
                        vs.visual_note += (vs.target_note - vs.visual_note) * 0.5f;
                    else
                        vs.visual_note = vs.target_note;
                    if (kon && lfo2610_on && pms > 0) {
                        float max_off = (pms / 7.0f) * 0.5f;
                        vs.vibrato_offset = sinf((float)(now2610 * 2.0 * 3.14159265 * lfo2610_hz)) * max_off;
                    } else {
                        vs.vibrato_offset *= 0.9f;
                        if (fabsf(vs.vibrato_offset) < 0.01f) vs.vibrato_offset = 0.0f;
                    }
                    int display_nt = (nt >= 0) ? (int)roundf(vs.start_note) : nt;
                    float poff = (nt >= 0) ? (vs.visual_note - vs.start_note) + vs.vibrato_offset : 0.0f;
                    if (poff >  1.0f) poff =  1.0f;
                    if (poff < -1.0f) poff = -1.0f;
                    add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, display_nt, poff);
                }
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 vol = s_shadowYM2610[c][0][0x08 + ch] & 0x0F;
                    bool on = vol > 0;
                    UINT8 plo = s_shadowYM2610[c][0][0x00 + ch * 2];
                    UINT8 phi = s_shadowYM2610[c][0][0x01 + ch * 2] & 0x0F;
                    uint16_t period = (phi << 8) | plo;
                    int nt = (on && period > 0) ? FrequencyToMIDINote((float)clk / (64.0f * period)) : -1;
                    ChVizState& vs = s_ym2610_ssg_viz[c][ch];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    float target = vol / 15.0f;
                    vs.smooth_vol += (target - vs.smooth_vol) * 0.3f;
                    add("S", ch+1, vs.smooth_vol, IM_COL32(100,180,255,255), on, dnt, poff);
                }
                {
                    static const char* kRhy2610[6] = {"BD","SD","CY","HH","TM","RM"};
                    UINT8 masterVol = s_shadowYM2610[c][1][0x01] & 0x3F;
                    float mvScale = (masterVol > 0) ? (masterVol / 63.0f) : 1.0f;
                    for (int ch = 0; ch < 6; ch++) {
                        float& decay = s_ym2610RhyKeyOn[c][ch];
                        decay *= 0.75f;  // per-frame decay (fast for percussion)
                        if (decay < 0.01f) decay = 0.0f;
                        bool  kon = decay > 0.0f;
                        UINT8 vol = s_shadowYM2610[c][1][0x08 + ch] & 0x1F;
                        float lv  = kon ? (vol / 31.0f) * mvScale * decay : 0.0f;
                        add(kRhy2610[ch], 0, lv, IM_COL32(255,160,60,255), kon);
                    }
                }
                {
                    // ADPCM-B
                    bool  kon = (s_shadowYM2610[c][1][0x00] & 0x80) != 0;
                    UINT8 vol = s_shadowYM2610[c][1][0x0B] & 0xFF;
                    float lv  = kon ? (vol / 255.0f) : 0.0f;
                    add("B", 0, lv, IM_COL32(255,200,80,255), kon);
                }
                break;
            }
            case DEVID_SN76496: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
                // 3 Tone channels
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 vol = s_shadowSN76496[c][ch*2+1] & 0x0F;
                    bool on = vol > 0;
                    uint16_t period = s_shadowSN76496[c][ch*2] & 0x03FF;
                    // Treat period 0 as 0x400 to maintain stable oscilloscope width
                    if (period == 0) period = 0x400;
                    int nt = (on && period > 0) ? FrequencyToMIDINote((float)clk / (32.0f * period)) : -1;
                    ChVizState& vs = s_sn76489_viz[c][ch];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    float target = vol / 15.0f;
                    vs.smooth_vol += (target - vs.smooth_vol) * 0.3f;
                    add("T", ch+1, vs.smooth_vol, IM_COL32(160,200,160,255), on, dnt, poff);
                }
                // Noise channel
                {
                    UINT8 nvol = s_shadowSN76496[c][7] & 0x0F;
                    UINT8 nctl = s_shadowSN76496[c][6] & 0x07;
                    bool on = nvol > 0;
                    int nt = -1;
                    if ((nctl & 0x04) && on) { // shift register mode
                        nt = 96; // ~C7
                    } else if (on) {
                        uint16_t period = s_shadowSN76496[c][4] & 0x03FF;
                        if (period > 0)
                            nt = FrequencyToMIDINote((float)clk / (32.0f * period));
                    }
                    float& decay = s_sn76489_noise_viz[c];
                    decay *= 0.75f;
                    if (decay < 0.01f) decay = 0.0f;
                    bool kon = on && decay > 0.0f;
                    float lv = kon ? (nvol / 15.0f) * decay : 0.0f;
                    add("N", 0, lv, IM_COL32(160,160,160,255), kon, nt >= 0 ? nt : 96);
                }
                break;
            }
            case DEVID_YMF262: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 14318180;
                // OPL slot reg offsets: ch->{M_off, C_off} derived from slot_array
                static const int kOPLSlotM[9] = {0x00,0x01,0x02,0x08,0x09,0x0A,0x10,0x11,0x12};
                static const int kOPLSlotC[9] = {0x03,0x04,0x05,0x0B,0x0C,0x0D,0x13,0x14,0x15};
                // OPL3 4op enable: port1 reg 0x04, bits0-2 = ch0-2 pairs, bits3-5 = ch9-11 pairs
                UINT8 fouren = s_shadowYMF262[c][1][0x04];
                for (int p2 = 0; p2 < 2; p2++) for (int ch = 0; ch < 9; ch++) {
                    UINT8 hi  = s_shadowYMF262[c][p2][0xB0 + ch];
                    bool  kon = (hi >> 5) & 1;
                    int nt = kon ? oplNote(s_shadowYMF262[c][p2][0xA0+ch], hi, clk) : -1;
                    // check 4op pairing: low ch=0-2(p2=0) or ch=0-2(p2=1), paired with ch+3
                    int pair_bit = (ch < 3) ? (p2*3 + ch) : -1;
                    bool is4op_low  = (pair_bit >= 0) && ((fouren >> pair_bit) & 1);
                    bool is4op_high = (ch >= 3 && ch <= 5) && ((fouren >> (p2*3 + ch-3)) & 1);
                    UINT8 tl;
                    if (is4op_low) {
                        // low channel of 4op pair: determine carriers from conn
                        int ch_hi = ch + 3;
                        UINT8 con_lo = s_shadowYMF262[c][p2][0xC0 + ch]    & 0x01;
                        UINT8 con_hi = s_shadowYMF262[c][p2][0xC0 + ch_hi] & 0x01;
                        UINT8 conn   = (con_lo << 1) | con_hi;
                        UINT8 tlM_lo = s_shadowYMF262[c][p2][0x40 + kOPLSlotM[ch]]    & 0x3F;
                        UINT8 tlC_lo = s_shadowYMF262[c][p2][0x40 + kOPLSlotC[ch]]    & 0x3F;
                        UINT8 tlM_hi = s_shadowYMF262[c][p2][0x40 + kOPLSlotM[ch_hi]] & 0x3F;
                        UINT8 tlC_hi = s_shadowYMF262[c][p2][0x40 + kOPLSlotC[ch_hi]] & 0x3F;
                        // conn0: C_hi only; conn1: C_lo+C_hi; conn2: M_lo+C_hi; conn3: M_lo+M_hi+C_hi
                        switch (conn) {
                            case 0: tl = tlC_hi; break;
                            case 1: tl = std::min(tlC_lo, tlC_hi); break;
                            case 2: tl = std::min(tlM_lo, tlC_hi); break;
                            case 3: tl = std::min({tlM_lo, tlM_hi, tlC_hi}); break;
                            default: tl = tlC_hi; break;
                        }
                    } else if (is4op_high) {
                        // high channel of 4op pair: skip (rendered as part of low ch)
                        continue;
                    } else {
                        // normal 2op
                        UINT8 con = s_shadowYMF262[c][p2][0xC0 + ch] & 0x01;
                        UINT8 tlC = s_shadowYMF262[c][p2][0x40 + kOPLSlotC[ch]] & 0x3F;
                        UINT8 tlM = s_shadowYMF262[c][p2][0x40 + kOPLSlotM[ch]] & 0x3F;
                        tl = con ? std::min(tlM, tlC) : tlC;
                    }
                    int vi = p2*9+ch;
                    // Apply decay based on keyoff state
                    if (s_ymf262KeyOff[c][vi])
                        s_ymf262_viz[c][vi].decay *= 0.60f;  // fast release
                    else
                        s_ymf262_viz[c][vi].decay *= 0.90f;  // slow sustain
                    if (s_ymf262_viz[c][vi].decay < 0.01f) s_ymf262_viz[c][vi].decay = 0.0f;
                    float poff = updateVizPSG(s_ymf262_viz[c][vi], kon, nt);
                    float lv  = (1.0f - tl / 63.0f) * s_ymf262_viz[c][vi].decay;
                    int dnt = kon ? (int)roundf(s_ymf262_viz[c][vi].start_note) : nt;
                    const char* pfx = (vi >= 9) ? "" : "F";  // 10+ channels: no prefix
                    add(pfx, vi+1, lv, IM_COL32(80,200,80,255), kon, dnt, poff);
                }
                break;
            }
            case DEVID_YM3812:
            case DEVID_YM3526:
            case DEVID_Y8950: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
                const UINT8* r = (dev.type==DEVID_YM3812) ? s_shadowYM3812[c]
                               : (dev.type==DEVID_YM3526) ? s_shadowYM3526[c]
                               : s_shadowY8950[c];
                static const int kOPL2SlotM[9] = {0x00,0x01,0x02,0x08,0x09,0x0A,0x10,0x11,0x12};
                static const int kOPL2SlotC[9] = {0x03,0x04,0x05,0x0B,0x0C,0x0D,0x13,0x14,0x15};
                UINT8 rhythm = r[0xBD];
                bool rhythmMode = (rhythm >> 5) & 1;
                for (int ch = 0; ch < 9; ch++) {
                    if (rhythmMode && ch >= 6) break; // ch6-8 used by rhythm
                    bool kon = (r[0xB0 + ch] >> 5) & 1;
                    int nt = kon ? oplNote(r[0xA0+ch], r[0xB0+ch], clk) : -1;
                    UINT8 con = r[0xC0 + ch] & 0x01;
                    UINT8 tlC = r[0x40 + kOPL2SlotC[ch]] & 0x3F;
                    UINT8 tlM = r[0x40 + kOPL2SlotM[ch]] & 0x3F;
                    UINT8 tl  = con ? std::min(tlM, tlC) : tlC;
                    ChVizState* vizArr = (dev.type==DEVID_YM3812) ? s_ym3812_viz[c]
                                       : (dev.type==DEVID_YM3526) ? s_ym3526_viz[c]
                                       : s_y8950_viz[c];
                    // Apply decay based on keyoff state
                    const bool* keyOffArr = (dev.type==DEVID_YM3812) ? s_ym3812KeyOff[c]
                                          : (dev.type==DEVID_YM3526) ? s_ym3526KeyOff[c]
                                          : s_y8950KeyOff[c];
                    if (keyOffArr[ch])
                        vizArr[ch].decay *= 0.60f;  // fast release
                    else
                        vizArr[ch].decay *= 0.90f;  // slow sustain
                    if (vizArr[ch].decay < 0.01f) vizArr[ch].decay = 0.0f;
                    float poff = updateVizPSG(vizArr[ch], kon, nt);
                    float lv  = (1.0f - tl / 63.0f) * vizArr[ch].decay;
                    int dnt = kon ? (int)roundf(vizArr[ch].start_note) : nt;
                    add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, dnt, poff);
                }
                if (rhythmMode) {
                    // Rhythm channels: BD/SD/TOM/CYM/HH
                    // TL regs: BD=0x53(slot16), SD=0x54(slot17), TOM=0x52(slot15), CYM=0x55(slot18), HH=0x51(slot14)
                    // keyon bits in 0xBD: BD=4,SD=3,TOM=2,CYM=1,HH=0
                    static const UINT8 kRhythmTL[5]  = {0x53, 0x54, 0x52, 0x55, 0x51};
                    static const int   kRhythmBit[5] = {4, 3, 2, 1, 0};
                    static const char* kRhythmName[5] = {"BD","SD","TM","CY","HH"};
                    for (int i = 0; i < 5; i++) {
                        bool kon = (rhythm >> kRhythmBit[i]) & 1;
                        UINT8 tl = r[kRhythmTL[i]] & 0x3F;
                        float lv = kon ? (1.0f - tl / 63.0f) : 0.0f;
                        LevelMeterEntry e;
                        snprintf(e.label, sizeof(e.label), "%s", kRhythmName[i]);
                        snprintf(e.chip_abbrev, sizeof(e.chip_abbrev), "%s", chipAbbrev);
                        e.chip_label[0] = '\0';
                        e.group_start = false;
                        e.level = lv; e.color = IM_COL32(180,120,60,255); e.keyon = kon; e.note = -1; e.devType = dev.type;
                        meters.push_back(e);
                    }
                }
                break;
            }
            case DEVID_YMF271: {
                // YMF271 (OPX): 12 groups × 4 banks = 48 slots.
                // Registers per group: addr = (reg<<4)|nibble, where
                // nibble maps group 0-11 -> {0,1,2,4,5,6,8,9,10,12,13,14}
                // reg 0x0 bit0 = keyon, reg 0x4 = TL(6:0),
                // reg 0x9 = fns_lo, reg 0xa = fns_hi (block=hi>>4, fns_hi=hi&0xF)
                // We read bank 0 (primary operators) for keyon/pitch/volume.
                static const int kYMF271Nibble[12] = {0,1,2,4,5,6,8,9,10,12,13,14};
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 16934400;
                for (int grp = 0; grp < 12; grp++) {
                    int nb  = kYMF271Nibble[grp];
                    UINT8 b0 = s_shadowYMF271[c][0][0x00 | nb]; // keyon byte
                    bool kon = (b0 & 0x01) != 0;
                    // algorithm in bank0 reg 0xC|nb bits3:0; carrier mask S1=bit3,S2=bit2,S3=bit1,S4=bit0
                    UINT8 alg271 = s_shadowYMF271[c][0][0xC0 | nb] & 0x0F;
                    static const UINT8 kYMF271CMask[16] = {
                        0x1,0x1,0x1,0x1, 0x1,0x1, 0x3,0x3,
                        0x9,0x9, 0x7,0x7, 0x7, 0xD, 0xB, 0xF
                    };
                    UINT8 cmask271 = kYMF271CMask[alg271];
                    // S1=bank0,S2=bank1,S3=bank2,S4=bank3
                    UINT8 tl271 = 0x7F;
                    for (int bk = 0; bk < 4; bk++)
                        if ((cmask271 >> (3 - bk)) & 1)
                            tl271 = std::min(tl271, (UINT8)(s_shadowYMF271[c][bk][0x40 | nb] & 0x7F));
                    UINT8 flo = s_shadowYMF271[c][0][0x90 | nb];
                    UINT8 fhi = s_shadowYMF271[c][0][0xA0 | nb];
                    int fns   = ((fhi & 0x0F) << 8) | flo;
                    // YMF271: freq = 2 * fns * pow_table[block] / clock
                    // pow_table[0-15] = {128,256,512,1024,2048,4096,8192,16384,0.5,1,2,4,8,16,32,64}
                    int nt = -1;
                    if (kon && fns > 0) {
                        static const double kYMF271PowTable[16] = {
                            128.0, 256.0, 512.0, 1024.0, 2048.0, 4096.0, 8192.0, 16384.0,
                            0.5,   1.0,   2.0,   4.0,    8.0,    16.0,   32.0,   64.0
                        };
                        int blk = (fhi >> 4) & 0x0F;
                        float freq = (float)(2.0 * fns * kYMF271PowTable[blk] / (double)clk);
                        nt = FrequencyToMIDINote(freq);
                    }
                    // Apply decay based on keyoff state
                    if (s_ymf271KeyOff[c][grp])
                        s_ymf271_viz[c][grp].decay *= 0.60f;  // fast release
                    else
                        s_ymf271_viz[c][grp].decay *= 0.90f;  // slow sustain
                    if (s_ymf271_viz[c][grp].decay < 0.01f) s_ymf271_viz[c][grp].decay = 0.0f;
                    float poff = updateVizPSG(s_ymf271_viz[c][grp], kon, nt);
                    float lev = (1.0f - tl271 / 127.0f) * s_ymf271_viz[c][grp].decay;
                    int dnt = kon ? (int)roundf(s_ymf271_viz[c][grp].start_note) : nt;
                    const char* pfx = (grp >= 9) ? "" : "F";  // 10+ channels: no prefix
                    add(pfx, grp+1, lev, IM_COL32(80,200,80,255), kon, dnt, poff);
                }
                break;
            }
            case DEVID_YMF278B: {
                // YMF278B FM part: OPL3-compatible (linked YMF262 core).
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 14318180;
                static const int kOPL3SlotM[9] = {0x00,0x01,0x02,0x08,0x09,0x0A,0x10,0x11,0x12};
                static const int kOPL3SlotC[9] = {0x03,0x04,0x05,0x0B,0x0C,0x0D,0x13,0x14,0x15};
                UINT8 fouren278 = s_shadowYMF278B_fm[c][1][0x04];
                for (int p2 = 0; p2 < 2; p2++) for (int ch = 0; ch < 9; ch++) {
                    UINT8 hi  = s_shadowYMF278B_fm[c][p2][0xB0 + ch];
                    bool  kon = (hi >> 5) & 1;
                    int nt = kon ? oplNote(s_shadowYMF278B_fm[c][p2][0xA0+ch], hi, clk) : -1;
                    int pair_bit = (ch < 3) ? (p2*3 + ch) : -1;
                    bool is4op_low  = (pair_bit >= 0) && ((fouren278 >> pair_bit) & 1);
                    bool is4op_high = (ch >= 3 && ch <= 5) && ((fouren278 >> (p2*3 + ch-3)) & 1);
                    UINT8 tl;
                    if (is4op_low) {
                        int ch_hi = ch + 3;
                        UINT8 con_lo = s_shadowYMF278B_fm[c][p2][0xC0 + ch]    & 0x01;
                        UINT8 con_hi = s_shadowYMF278B_fm[c][p2][0xC0 + ch_hi] & 0x01;
                        UINT8 conn   = (con_lo << 1) | con_hi;
                        UINT8 tlM_lo = s_shadowYMF278B_fm[c][p2][0x40 + kOPL3SlotM[ch]]    & 0x3F;
                        UINT8 tlC_lo = s_shadowYMF278B_fm[c][p2][0x40 + kOPL3SlotC[ch]]    & 0x3F;
                        UINT8 tlM_hi = s_shadowYMF278B_fm[c][p2][0x40 + kOPL3SlotM[ch_hi]] & 0x3F;
                        UINT8 tlC_hi = s_shadowYMF278B_fm[c][p2][0x40 + kOPL3SlotC[ch_hi]] & 0x3F;
                        switch (conn) {
                            case 0: tl = tlC_hi; break;
                            case 1: tl = std::min(tlC_lo, tlC_hi); break;
                            case 2: tl = std::min(tlM_lo, tlC_hi); break;
                            case 3: tl = std::min({tlM_lo, tlM_hi, tlC_hi}); break;
                            default: tl = tlC_hi; break;
                        }
                    } else if (is4op_high) {
                        continue;
                    } else {
                        UINT8 con = s_shadowYMF278B_fm[c][p2][0xC0 + ch] & 0x01;
                        UINT8 tlC = s_shadowYMF278B_fm[c][p2][0x40 + kOPL3SlotC[ch]] & 0x3F;
                        UINT8 tlM = s_shadowYMF278B_fm[c][p2][0x40 + kOPL3SlotM[ch]] & 0x3F;
                        tl = con ? std::min(tlM, tlC) : tlC;
                    }
                    int vi278 = p2*9+ch;
                    // Apply decay based on keyoff state
                    if (s_ymf278bKeyOff[c][vi278])
                        s_ymf278b_viz[c][vi278].decay *= 0.60f;  // fast release
                    else
                        s_ymf278b_viz[c][vi278].decay *= 0.90f;  // slow sustain
                    if (s_ymf278b_viz[c][vi278].decay < 0.01f) s_ymf278b_viz[c][vi278].decay = 0.0f;
                    float poff = updateVizPSG(s_ymf278b_viz[c][vi278], kon, nt);
                    float lv  = (1.0f - tl / 63.0f) * s_ymf278b_viz[c][vi278].decay;
                    int dnt = kon ? (int)roundf(s_ymf278b_viz[c][vi278].start_note) : nt;
                    const char* pfx = (vi278 >= 9) ? "" : "F";  // 10+ channels: no prefix
                    add(pfx, vi278+1, lv, IM_COL32(80,200,80,255), kon, dnt, poff);
                }
                break;
            }
            case DEVID_AY8910: {
                int c = dev.instance & 1;
                UINT32 ayClock = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 1773400;
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 vol = s_shadowAY8910[c][0x08 + ch] & 0x0F;
                    bool  on  = vol > 0;
                    UINT8 plo = s_shadowAY8910[c][ch*2];
                    UINT8 phi = s_shadowAY8910[c][ch*2+1] & 0x0F;
                    uint16_t period = (phi << 8) | plo;
                    int nt = (on && period > 0) ? FrequencyToMIDINote((float)ayClock / (16.0f * period)) : -1;
                    ChVizState& vs = s_ay8910_viz[c][ch];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    float target = vol / 15.0f;
                    vs.smooth_vol += (target - vs.smooth_vol) * 0.3f;
                    add("S", ch+1, vs.smooth_vol, IM_COL32(100,180,255,255), on, dnt, poff);
                }
                // Envelope frequency channel (bass/low frequency)
                {
                    UINT16 efrq = ((UINT16)s_shadowAY8910[c][0x0C] << 8) | s_shadowAY8910[c][0x0B];
                    UINT8 etype = s_shadowAY8910[c][0x0D] & 0x0F;
                    // Determine if any channel uses envelope as waveform
                    bool envUsedAsTone = false;
                    for (int ch = 0; ch < 3; ch++) {
                        if ((s_shadowAY8910[c][0x08 + ch] & 0x10) && efrq > 0 && efrq < 200)
                            envUsedAsTone = true;
                    }
                    // YASP formula: freq = clock / (16 * envelope_period * steps)
                    // steps=32 for sawtooth (shapes 8,9,11,12,13,15), 64 for triangle (10,14)
                    int steps = 32;
                    if (etype == 10 || etype == 14) steps = 64;
                    bool on = envUsedAsTone && efrq > 0;
                    float eFreq = (efrq > 0) ? (float)ayClock / (8.0f * efrq * steps) : 0.0f;
                    int nt = (on && eFreq > 0.0f) ? FrequencyToMIDINote(eFreq) : -1;
                    ChVizState& vs = s_ay8910_env_viz[c];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    // Volume: max volume among channels using envelope mode (bit4 set)
                    float maxEVol = 0.0f;
                    if (on) {
                        for (int ch = 0; ch < 3; ch++) {
                            if (s_shadowAY8910[c][0x08 + ch] & 0x10) {
                                float v = (s_shadowAY8910[c][0x08 + ch] & 0x0F) / 15.0f;
                                if (v > maxEVol) maxEVol = v;
                            }
                        }
                    }
                    vs.smooth_vol += (on ? (maxEVol - vs.smooth_vol) * 0.3f : (0.0f - vs.smooth_vol) * 0.3f);
                    // Orange color to distinguish from tone channels
                    add("E", 0, vs.smooth_vol, IM_COL32(255,160,60,255), on, dnt, poff);
                }
                // Noise channel
                {
                    UINT8 nfrq_reg = s_shadowAY8910[c][0x06] & 0x1F;
                    int noise_freq = nfrq_reg ? (nfrq_reg << 1) : 2;
                    // Check if any channel has noise enabled (reg7 bits 3-5, active low: 0=on)
                    // and volume > 0
                    UINT8 mix = s_shadowAY8910[c][0x07];
                    bool noise_on = false;
                    for (int ch = 0; ch < 3; ch++) {
                        // noise enable bit is active low: 0 = noise enabled for this channel
                        if (!(mix & (0x08 << ch)) && (s_shadowAY8910[c][0x08 + ch] & 0x0F) > 0) {
                            noise_on = true;
                            break;
                        }
                    }
                    float nFreq = noise_on ? (float)ayClock / (512.0f * noise_freq) : 0.0f;
                    int nt = (noise_on && nFreq > 0.0f) ? FrequencyToMIDINote(nFreq) : -1;
                    ChVizState& vs = s_ay8910_viz[c][3]; // reuse index 3 for noise
                    float poff = updateVizPSG(vs, noise_on, nt);
                    int dnt = noise_on ? (int)roundf(vs.start_note) : nt;
                    // Determine max volume among noise-enabled channels
                    float maxNVol = 0.0f;
                    if (noise_on) {
                        for (int ch = 0; ch < 3; ch++) {
                            // noise enable bit is active low: 0 = noise enabled
                            if (!(mix & (0x08 << ch))) {
                                float v = (s_shadowAY8910[c][0x08 + ch] & 0x0F) / 15.0f;
                                if (v > maxNVol) maxNVol = v;
                            }
                        }
                    }
                    vs.smooth_vol += (noise_on ? (maxNVol - vs.smooth_vol) * 0.3f : (0.0f - vs.smooth_vol) * 0.3f);
                    add("N", 0, vs.smooth_vol, IM_COL32(160,160,160,255), noise_on, dnt, poff);
                }
                break;
            }
            case DEVID_NES_APU: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 1789773;
                // NES: pulse freq = clk / (16*(period+1)), triangle = clk / (32*(period+1))
                UINT8 status = s_shadowNES_APU[c][0x15];
                // Pulse 1 & 2
                for (int ch = 0; ch < 2; ch++) {
                    bool on = (status >> ch) & 1;
                    UINT8 vol = s_shadowNES_APU[c][ch*4] & 0x0F;
                    int nt = -1;
                    if (on) {
                        uint16_t period = ((s_shadowNES_APU[c][ch*4+3] & 0x07) << 8) | s_shadowNES_APU[c][ch*4+2];
                        if (period > 0) nt = FrequencyToMIDINote((float)clk / (16.0f * (period+1)));
                    }
                    ChVizState& vs = s_nes_apu_viz[c][ch];
                    if (s_nes_apuKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    add("C", ch+1, (vol/15.0f) * vs.decay, IM_COL32(100,180,255,255), on, dnt, poff);
                }
                // Triangle
                {
                    bool on = (status >> 2) & 1;
                    bool linear_halt = (s_shadowNES_APU[c][0x08] & 0x7F) != 0;
                    on = on && linear_halt;
                    int nt = -1;
                    if (on) {
                        uint16_t period = ((s_shadowNES_APU[c][0x0B] & 0x07) << 8) | s_shadowNES_APU[c][0x0A];
                        if (period > 0) nt = FrequencyToMIDINote((float)clk / (32.0f * (period+1)));
                    }
                    ChVizState& vs = s_nes_apu_viz[c][2];
                    if (s_nes_apuKeyOff[c][2]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float poff2 = updateVizPSG(vs, on, nt);
                    int dnt2 = on ? (int)roundf(vs.start_note) : nt;
                    add("C", 3, 1.0f * vs.decay, IM_COL32(100,180,255,255), on, dnt2, poff2);
                }
                // Noise
                {
                    bool on = (status >> 3) & 1;
                    UINT8 vol = s_shadowNES_APU[c][0x0C] & 0x0F;
                    bool constVol = (s_shadowNES_APU[c][0x0C] & 0x10) != 0;
                    float lv = on ? (constVol ? vol/15.0f : 0.5f) : 0.0f;
                    add("C", 4, lv, IM_COL32(160,160,160,255), on, -1);
                }
                // DMC
                {
                    bool on = (status >> 4) & 1;
                    UINT8 dmc_vol = s_shadowNES_APU[c][0x11] & 0x7F;
                    float lv = on ? dmc_vol / 127.0f : 0.0f;
                    add("C", 5, lv, IM_COL32(255,160,80,255), on, -1);
                }
                break;
            }
            case DEVID_GB_DMG: {
                int c = dev.instance & 1;
                UINT8 ctrl = s_shadowGB_DMG[c][0x26];
                bool masterOn = (ctrl >> 7) & 1;
                static const int freqLoRegs[] = {0x13, 0x18, 0x1D, -1};
                static const int freqHiRegs[] = {0x14, 0x19, 0x1E, -1};
                static const int volRegs[]    = {0x12, 0x17, 0x1C, 0x21};
                for (int ch = 0; ch < 4; ch++) {
                    bool on = masterOn && ((ctrl >> ch) & 1);
                    UINT8 vol = s_shadowGB_DMG[c][volRegs[ch]] & 0x0F;
                    int nt = -1;
                    if (on && ch < 3) {
                        uint16_t period = ((uint16_t)(s_shadowGB_DMG[c][freqHiRegs[ch]] & 0x07) << 8)
                                        | s_shadowGB_DMG[c][freqLoRegs[ch]];
                        if (period < 2048) nt = FrequencyToMIDINote(524288.0f / (2048.0f - period));
                    }
                    if (ch < 3) {
                        ChVizState& vs = s_gb_dmg_viz[c][ch];
                        if (s_gb_dmgKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                        if (vs.decay < 0.01f) vs.decay = 0.0f;
                        float poff = updateVizPSG(vs, on, nt);
                        int dnt = on ? (int)roundf(vs.start_note) : nt;
                        add("G", ch+1, (vol/15.0f) * vs.decay, IM_COL32(100,200,180,255), on, dnt, poff);
                    } else {
                        add("G", ch+1, on ? vol/15.0f : 0.0f, IM_COL32(160,160,160,255), on, -1);
                    }
                }
                break;
            }
            case DEVID_K051649: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 1789772;
                for (int ch = 0; ch < 5; ch++) {
                    UINT8 vol = s_shadowK051649[c][2][ch] & 0x0F;
                    bool on = vol > 0;
                    uint16_t period = ((uint16_t)(s_shadowK051649[c][1][ch*2+1] & 0x0F) << 8)
                                    | s_shadowK051649[c][1][ch*2];
                    int nt = (on && period > 0) ? FrequencyToMIDINote((float)clk / (8.0f * period)) : -1;
                    ChVizState& vs = s_k051649_viz[c][ch];
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    float target = vol / 15.0f;
                    vs.smooth_vol += (target - vs.smooth_vol) * 0.3f;
                    add("S", ch+1, vs.smooth_vol, IM_COL32(100,180,255,255), on, dnt, poff);
                }
                break;
            }
            case DEVID_C6280: {
                int c = dev.instance & 1;
                UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
                for (int ch = 0; ch < 6; ch++) {
                    // reg[0]=freq_lo, reg[1]=freq_hi(4bit), reg[2]=vol(L/R nibbles), reg[5]=pan
                    uint16_t period = ((uint16_t)(s_shadowC6280[c][ch][1] & 0x0F) << 8) | s_shadowC6280[c][ch][0];
                    UINT8 volreg = s_shadowC6280[c][ch][2];
                    UINT8 volL = (volreg >> 4) & 0x0F;
                    UINT8 volR = volreg & 0x0F;
                    float lv = (volL + volR) / 30.0f;
                    bool on = lv > 0.0f && period > 0;
                    int nt = on ? FrequencyToMIDINote((float)clk / (32.0f * period)) : -1;
                    float poff = updateVizPSG(s_c6280_viz[c][ch], on, nt);
                    int dnt = on ? (int)roundf(s_c6280_viz[c][ch].start_note) : nt;
                    add("H", ch+1, lv * s_c6280_viz[c][ch].decay, IM_COL32(180,100,255,255), on, dnt, poff);
                }
                break;
            }
            case DEVID_SAA1099: {
                int c = dev.instance & 1;
                // 0x1C bit0 = master sound enable
                bool masterOn = (s_shadowSAA1099[c][0x1C] & 0x01) != 0;
                UINT8 freq_en  = s_shadowSAA1099[c][0x14] & 0x3F;
                UINT8 noise_en = s_shadowSAA1099[c][0x15] & 0x3F;
                // Envelope generators: env[0] bit7 enables for ch0/1/4, env[1] bit7 for ch2/3/5
                bool env0_on = (s_shadowSAA1099[c][0x18] & 0x80) != 0;
                bool env1_on = (s_shadowSAA1099[c][0x19] & 0x80) != 0;
                UINT32 saaClock = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 8053400;
                double clk2div512 = (saaClock + 128.0) / 256.0;
                for (int ch = 0; ch < 6; ch++) {
                    UINT8 amp = s_shadowSAA1099[c][0x00 + ch];
                    UINT8 vol = ((amp & 0x0F) + ((amp >> 4) & 0x0F)) / 2;
                    bool ch_freq_on  = masterOn && ((freq_en  >> ch) & 1);
                    bool ch_noise_on = masterOn && ((noise_en >> ch) & 1);
                    bool ch_env_on   = (ch == 0 || ch == 1 || ch == 4) ? env0_on : env1_on;
                    bool on = (ch_freq_on || ch_noise_on) && vol > 0;
                    // SAA1099 freq: clk2div512 * (1<<oct) / (511 - freq_val)
                    UINT8 fval = s_shadowSAA1099[c][0x08 + ch];
                    int oct_reg = ch / 2;
                    int oct = (ch & 1) ? ((s_shadowSAA1099[c][0x10 + oct_reg] >> 4) & 7)
                                       : (s_shadowSAA1099[c][0x10 + oct_reg] & 7);
                    int nt = -1;
                    if (ch_freq_on && fval > 0) {
                        float freq = (float)(clk2div512 * (1 << oct) / (511.0 - (int)fval));
                        nt = FrequencyToMIDINote(freq);
                    }
                    // Color: green=envelope, gray=noise-only, blue=normal tone
                    ImU32 col;
                    if (ch_env_on)
                        col = IM_COL32(80, 220, 100, 255);   // green: envelope (any mode)
                    else if (ch_noise_on && !ch_freq_on)
                        col = IM_COL32(160, 160, 160, 255);  // gray: noise only (matches SN76496 white noise)
                    else
                        col = IM_COL32(100, 160, 255, 255);  // blue: normal tone
                    float poff = updateVizPSG(s_saa1099_viz[c][ch], on, nt);
                    int dnt = on ? (int)roundf(s_saa1099_viz[c][ch].start_note) : nt;
                    add("S", ch+1, vol/15.0f * s_saa1099_viz[c][ch].decay, col, on, dnt, poff);
                }
                break;
            }
            case DEVID_POKEY: {
                int c = dev.instance & 1;
                UINT8 audctl = s_shadowPOKEY[c][0x08];
                (void)audctl;
                for (int ch = 0; ch < 4; ch++) {
                    UINT8 vol = s_shadowPOKEY[c][ch*2+1] & 0x0F;
                    ChVizState& vs = s_pokey_viz[c][ch];
                    if (s_pokeyKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "P";
                    add(pfx, ch+1, (vol/15.0f) * vs.decay, IM_COL32(200,150,80,255), vol > 0);
                }
                break;
            }
            case DEVID_WSWAN: {
                int c = dev.instance & 1;
                UINT8 wsCtrl = s_shadowWSWAN[c][0x10];  // SNDMOD (VGM offset 0x10)
                for (int ch = 0; ch < 4; ch++) {
                    UINT8 vol    = s_shadowWSWAN[c][0x08 + ch] & 0x0F;
                    bool  on     = ((wsCtrl >> ch) & 1) && (vol > 0);
                    UINT16 period = ((UINT16)(s_shadowWSWAN[c][ch*2+1] & 0x07) << 8) | s_shadowWSWAN[c][ch*2];
                    int pdenom   = 2048 - (int)period;
                    int nt = (on && pdenom > 0) ? FrequencyToMIDINote((3072000.0f / 16.0f) / (float)pdenom) : -1;
                    ChVizState& vs = s_wswan_viz[c][ch];
                    if (s_wswanKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float poff = updateVizPSG(vs, on, nt);
                    int dnt = on ? (int)roundf(vs.start_note) : nt;
                    add("W", ch+1, (vol/15.0f) * vs.decay, IM_COL32(150,200,255,255), on, dnt, poff);
                }
                break;
            }
            case DEVID_OKIM6258: {
                int c = dev.instance & 1;
                bool on = (s_shadowOKIM6258[c][0x01] & 0x01) != 0;
                ChVizState& vs = s_okim6258_viz[c][0];
                if (s_okim6258KeyOff[c][0]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                if (vs.decay < 0.01f) vs.decay = 0.0f;
                LevelMeterEntry e;
                snprintf(e.label, sizeof(e.label), "PCM");
                snprintf(e.chip_abbrev, sizeof(e.chip_abbrev), "%s", chipAbbrev);
                snprintf(e.chip_label, sizeof(e.chip_label), "%s", chipLabel);
                e.group_start = true;
                e.level = (on ? 0.8f : 0.0f) * vs.decay; e.color = IM_COL32(255,120,80,255); e.keyon = on; e.devType = dev.type;
                meters.push_back(e);
                break;
            }
            case DEVID_OKIM6295: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 4; ch++) {
                    bool on = (s_shadowOKIM6295[c][ch] & 0x80) != 0;
                    ChVizState& vs = s_okim6295_viz[c][ch];
                    if (s_okim6295KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "O";
                    add(pfx, ch+1, (on ? 0.8f : 0.0f) * vs.decay, IM_COL32(255,120,80,255), on);
                }
                break;
            }
            case DEVID_RF5C68: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 8; ch++) {
                    UINT8 vol = s_shadowRF5C68[c][ch] & 0xFF;
                    ChVizState& vs = s_rf5c68_viz[c][ch];
                    if (s_rf5c68KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "R";
                    add(pfx, ch+1, (vol/255.0f) * vs.decay, IM_COL32(255,160,100,255), vol > 0);
                }
                break;
            }
            case DEVID_YMZ280B: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 8; ch++) {
                    UINT8 ctrl = s_shadowYMZ280B[c][0x00 + ch * 4];
                    bool  on   = (ctrl & 0x80) != 0;
                    UINT8 vol  = s_shadowYMZ280B[c][0x02 + ch * 4] & 0xFF;
                    ChVizState& vs = s_ymz280b_viz[c][ch];
                    if (s_ymz280bKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "Z";
                    add(pfx, ch+1, (on ? vol/255.0f : 0.0f) * vs.decay, IM_COL32(255,140,200,255), on);
                }
                break;
            }
            case DEVID_YMW258: {
                int c = dev.instance & 1;
                // MultiPCM: 28 channels; regs[5] bits6:0 = TL (0=max, 127=silent)
                for (int ch = 0; ch < 28; ch++) {
                    UINT8 kon = s_shadowYMW258[c][ch * 8 + 5];
                    bool  on  = (kon & 0x80) != 0;
                    UINT8 tl  = (kon >> 1) & 0x7F;
                    ChVizState& vs = s_ymw258_viz[c][ch];
                    if (s_ymw258KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    add("M", ch+1, (on ? (1.0f - tl / 127.0f) : 0.0f) * vs.decay, IM_COL32(200,100,255,255), on);
                }
                break;
            }
            case DEVID_uPD7759: {
                int c = dev.instance & 1;
                bool on = (s_shadowUPD7759[c][0] & 0x01) != 0;
                ChVizState& vs = s_upd7759_viz[c][0];
                if (s_upd7759KeyOff[c][0]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                if (vs.decay < 0.01f) vs.decay = 0.0f;
                add("U", 1, (on ? 0.8f : 0.0f) * vs.decay, IM_COL32(255,180,80,255), on);
                break;
            }
            case DEVID_K054539: {
                int c = dev.instance & 1;
                UINT8 kon = s_shadowK054539[c][0][0x14];
                for (int ch = 0; ch < 8; ch++) {
                    bool on = (kon >> ch) & 1;
                    // vol reg: ch*0x20 + 0x03, 0x00=silent, 0x40=max
                    UINT8 vol = s_shadowK054539[c][0][ch * 0x20 + 0x03] & 0x7F;
                    ChVizState& vs = s_k054539_viz[c][ch];
                    if (s_k054539KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "K";
                    add(pfx, ch+1, (on ? (vol / 64.0f) : 0.0f) * vs.decay, IM_COL32(180,220,120,255), on);
                }
                break;
            }
            case DEVID_C140: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 24; ch++) {
                    UINT8 ctrl   = s_shadowC140[c][ch * 16 + 5];
                    bool  on     = (ctrl & 0x80) != 0;
                    UINT8 volR   = s_shadowC140[c][ch * 16 + 0];
                    UINT8 volL   = s_shadowC140[c][ch * 16 + 1];
                    UINT8 vol    = volR > volL ? volR : volL;
                    ChVizState& vs = s_c140_viz[c][ch];
                    if (s_c140KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "C";
                    add(pfx, ch+1, (on ? (vol / 255.0f) : 0.0f) * vs.decay, IM_COL32(120,200,180,255), on);
                }
                break;
            }
            case DEVID_K053260: {
                int c = dev.instance & 1;
                UINT8 kon = s_shadowK053260[c][0x00];
                for (int ch = 0; ch < 4; ch++) {
                    bool  on  = (kon >> ch) & 1;
                    UINT8 vol = s_shadowK053260[c][0x08 + ch * 8 + 7] & 0x7F;
                    ChVizState& vs = s_k053260_viz[c][ch];
                    if (s_k053260KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "K";
                    add(pfx, ch+1, (on ? (vol / 127.0f) : 0.0f) * vs.decay, IM_COL32(180,220,120,255), on);
                }
                break;
            }
            case DEVID_QSOUND: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 16; ch++) {
                    UINT8 vol = s_shadowQSound[c][ch * 2];
                    bool  on  = vol > 0;
                    ChVizState& vs = s_qsound_viz[c][ch];
                    if (s_qsoundKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "Q";  // 10+ channels: no prefix
                    add(pfx, ch+1, (vol/255.0f) * vs.decay, IM_COL32(255,220,80,255), on);
                }
                break;
            }
            case DEVID_SCSP: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 32; ch++) {
                    // word0 high byte = datab[ch*0x20], KEYONB = bit3
                    bool  on  = (s_shadowSCSP[c][ch * 0x20] & 0x08) != 0;
                    // TL = word6 low byte = datab[ch*0x20 + 0x0D], 0xFF=silent, 0x00=max
                    UINT8 tl  = s_shadowSCSP[c][ch * 0x20 + 0x0D];

                    ChVizState& vs = s_scsp_viz[c][ch];
                    if (s_scspKeyOff[c][ch])
                        vs.decay *= 0.60f;  // fast release
                    else
                        vs.decay *= 0.90f;  // slow sustain
                    if (vs.decay < 0.01f) vs.decay = 0.0f;

                    if (vs.key_on_event) {
                        vs.key_on_event = false;
                    }

                    float lv  = on ? (1.0f - tl / 255.0f) * vs.decay : 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "S";
                    add(pfx, ch+1, lv, IM_COL32(255,100,150,255), on && (vs.decay > 0.01f));
                }
                break;
            }
            case DEVID_VBOY_VSU: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 6; ch++) {
                    UINT8 ctrl  = s_shadowVBOY_VSU[c][ch * 0x10];
                    bool  on    = (ctrl & 0x80) != 0;
                    UINT8 lvol  = s_shadowVBOY_VSU[c][ch * 0x10 + 0x04];
                    UINT8 lv_l  = (lvol >> 4) & 0x0F;
                    UINT8 lv_r  = lvol & 0x0F;
                    UINT8 lv_mx = lv_l > lv_r ? lv_l : lv_r;
                    float lv    = on ? (lv_mx / 15.0f) : 0.0f;
                    add("V", ch+1, lv, IM_COL32(200,100,100,255), on);
                }
                break;
            }
            case DEVID_ES5503: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 32; ch++) {
                    UINT8 ctrl = s_shadowES5503[c][ch + 0xA0];
                    bool  on   = !(ctrl & 0x01);
                    UINT8 vol  = s_shadowES5503[c][ch + 0x40];
                    float lv   = on ? (vol / 255.0f) : 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "E";
                    add(pfx, ch+1, lv, IM_COL32(100,255,200,255), on);
                }
                break;
            }
            case DEVID_ES5506: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 32; ch++) {
                    UINT8 vol = s_shadowES5506[c][ch * 4 + 1];
                    bool  on  = vol > 0;
                    ChVizState& vs = s_es5506_viz[c][ch];
                    if (s_es5506KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "E";
                    add(pfx, ch+1, (vol/255.0f) * vs.decay, IM_COL32(100,200,255,255), on);
                }
                break;
            }
            case DEVID_X1_010: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 16; ch++) {
                    UINT8 ctrl = s_shadowX1_010[c][ch * 0x10];
                    bool  on   = (ctrl & 0x01) != 0;
                    UINT8 vol  = s_shadowX1_010[c][ch * 0x10 + 2];
                    ChVizState& vs = s_x1_010_viz[c][ch];
                    if (s_x1_010KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "X";
                    add(pfx, ch+1, (on ? vol/255.0f : 0.0f) * vs.decay, IM_COL32(255,200,100,255), on);
                }
                break;
            }
            case DEVID_C352: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 32; ch++) {
                    // flags word at reg3: addr ch*8+3 → bytes [ch*16+6]/[ch*16+7], BUSY=bit15(high byte bit7)
                    bool  on   = (s_shadowC352[c][ch * 16 + 6] & 0x80) != 0;
                    // vol_f word at reg0: addr ch*8+0 → bytes [ch*16+0](hi)/[ch*16+1](lo)
                    UINT8 volH = s_shadowC352[c][ch * 16 + 0];
                    UINT8 volL = s_shadowC352[c][ch * 16 + 1];
                    UINT8 vol  = volH > volL ? volH : volL;
                    ChVizState& vs = s_c352_viz[c][ch];
                    if (s_c352KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    float lv   = (on ? (vol / 255.0f) : 0.0f) * vs.decay;
                    const char* pfx = (ch >= 9) ? "" : "C";
                    add(pfx, ch+1, lv, IM_COL32(150,180,255,255), on);
                }
                break;
            }
            case DEVID_GA20: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 4; ch++) {
                    UINT8 on_b = s_shadowGA20[c][ch * 8 + 6];
                    bool  on   = on_b != 0;
                    UINT8 vol  = s_shadowGA20[c][ch * 8 + 5];
                    ChVizState& vs = s_ga20_viz[c][ch];
                    if (s_ga20KeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    add("G", ch+1, (on ? (vol / 255.0f) : 0.0f) * vs.decay, IM_COL32(255,160,80,255), on);
                }
                break;
            }
            case DEVID_SEGAPCM: {
                int c = dev.instance & 1;
                for (int ch = 0; ch < 16; ch++) {
                    UINT8 ctrl = s_shadowSEGAPCM[c][ch * 8 + 7];
                    bool  on   = (ctrl & 0x01) == 0;
                    UINT8 vol  = s_shadowSEGAPCM[c][ch * 8 + 6];
                    ChVizState& vs = s_segapcm_viz[c][ch];
                    if (s_segapcmKeyOff[c][ch]) vs.decay *= 0.60f; else vs.decay *= 0.90f;
                    if (vs.decay < 0.01f) vs.decay = 0.0f;
                    const char* pfx = (ch >= 9) ? "" : "P";
                    add(pfx, ch+1, (on ? vol/255.0f : 0.0f) * vs.decay, IM_COL32(255,140,100,255), on);
                }
                break;
            }
            case DEVID_32X_PWM: {
                int c = dev.instance & 1;
                static const char* pwmPfx[2] = {"L", "R"};
                for (int ch = 0; ch < 2; ch++) {
                    UINT16 val = s_shadow32XPWM[c][ch + 2]; // Out_L=reg2, Out_R=reg3
                    bool  on   = (val != 0);
                    float lv   = on ? (val / 4095.0f) : 0.0f;
                    add(pwmPfx[ch], 1, lv, IM_COL32(255,140,100,255), on);
                }
                break;
            }
            default:
                break;
        }
    }
    return meters;
}

void RenderLevelMeterArea() {
    UpdateShadowRegisters();
    std::vector<LevelMeterEntry> meters = BuildLevelMeters();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y - 4;
    if (availH < 40.0f) availH = 40.0f;

    if (meters.empty()) {
        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), s_fileLoaded ? "(no chip data)" : "(no file)");
        ImGui::Dummy(ImVec2(availW, availH));
        return;
    }

    // Compute modizer voice channel offsets per chip group (for oscilloscope)
    if (s_showScope) {
        // Debug: log registered scope slots periodically
        static int s_slotLogFrame = 0;
        if (++s_slotLogFrame % 300 == 1) {
            // VgmLog("[Scope] Registered %d slots:", ::g_scope_chip_count);
            for (int si = 0; si < ::g_scope_chip_count; si++) {
                ScopeChipSlot *ss = scope_find_slot_by_index(si);
                // if (ss) VgmLog("  [%d] name=%s inst=%d ch=%d base=%d", si,
                //     ss->chip_name ? ss->chip_name : "?", ss->chip_inst, ss->num_channels, ss->slot_base);
            }
        }
        for (int i = 0; i < (int)meters.size(); ) {
            // Count channels in this group
            int j = i + 1;
            while (j < (int)meters.size() && !meters[j].group_start) j++;

            // Get chip name and instance from device info
            const auto& devs = GetActiveDevices();
            int devIdx = 0;
            for (int k = 0; k < i; k++) {
                if (meters[k].group_start) devIdx++;
            }
            const char* scopeName = NULL;
            int chipInst = 0;
            if (devIdx < (int)devs.size()) {
                UINT8 chipType = devs[devIdx].type;
                // Use chip instance from device info to distinguish multiple chips (0 or 1 from VGM)
                chipInst = (int)devs[devIdx].instance;
                // Map dev type to scope registration name
                switch (chipType) {
                    case DEVID_SN76496:  scopeName = "SN76489";  break;
                    case DEVID_YM2413:   scopeName = "YM2413";   break;
                    case DEVID_YM2612:   scopeName = "YM2612";   break;
                    case DEVID_YM2151:   scopeName = "YM2151";   break;
                    case DEVID_SEGAPCM:  scopeName = "SEGAPCM";  break;
                    case DEVID_RF5C68:   scopeName = "RF5C68";   break;
                    case DEVID_YM2203:   scopeName = "YM2203";   break;
                    case DEVID_YM2608:   scopeName = "YM2608";   break;
                    case DEVID_YM2610:   scopeName = "YM2610";   break;
                    case DEVID_YM3812:   scopeName = "OPL2";     break;
                    case DEVID_YM3526:   scopeName = "OPL2";     break;
                    case DEVID_Y8950:    scopeName = "Y8950";    break;
                    case DEVID_YMF262:   scopeName = "YMF262";   break;
                    case DEVID_YMF278B:  scopeName = "YMF278B";  break;
                    case DEVID_YMF271:   scopeName = "YMF271";   break;
                    case DEVID_YMZ280B:  scopeName = "YMZ280B";  break;
                    case DEVID_32X_PWM:  scopeName = "32X_PWM";  break;
                    case DEVID_AY8910:   scopeName = "SSG";      break;
                    case DEVID_GB_DMG:   scopeName = "GB_DMG";   break;
                    case DEVID_NES_APU:  scopeName = "NES_APU";  break;
                    case DEVID_YMW258:   scopeName = "MultiPCM"; break;
                    case DEVID_uPD7759:  scopeName = "uPD7759";  break;
                    case DEVID_OKIM6258: scopeName = "OKIM6258"; break;
                    case DEVID_OKIM6295: scopeName = "OKIM6295"; break;
                    case DEVID_K051649:  scopeName = "SCC";      break;
                    case DEVID_K054539:  scopeName = "K054539";  break;
                    case DEVID_C6280:    scopeName = "C6280";    break;
                    case DEVID_C140:     scopeName = "C140";     break;
                    case DEVID_C219:     scopeName = "C219";     break;
                    case DEVID_K053260:  scopeName = "K053260";  break;
                    case DEVID_POKEY:    scopeName = "POKEY";    break;
                    case DEVID_QSOUND:   scopeName = "QSOUND";   break;
                    case DEVID_SCSP:     scopeName = "SCSP";     break;
                    case DEVID_WSWAN:    scopeName = "WSWAN";    break;
                    case DEVID_VBOY_VSU: scopeName = "VBOY_VSU"; break;
                    case DEVID_SAA1099:  scopeName = "SAA1099";  break;
                    case DEVID_ES5503:   scopeName = "ES5503";   break;
                    case DEVID_ES5506:   scopeName = "ES5506";   break;
                    case DEVID_X1_010:   scopeName = "X1_010";   break;
                    case DEVID_C352:     scopeName = "C352";     break;
                    case DEVID_GA20:     scopeName = "GA20";     break;
                    case DEVID_MIKEY:    scopeName = "MIKEY";    break;
                    case DEVID_K007232:  scopeName = "K007232";  break;
                    case DEVID_K005289:  scopeName = "K005289";  break;
                    case DEVID_MSM5205:  scopeName = "MSM5205";  break;
                    default: break;
                }
            }

            // Find scope slot to get the buffer base offset
            int voice_ofs = -1;
            if (scopeName) {
                ScopeChipSlot *slot = scope_find_slot(scopeName, chipInst);
                // Dual-chip fallback: core may only register chip_inst=0
                if (!slot && chipInst > 0)
                    slot = scope_find_slot(scopeName, 0);
                if (slot) voice_ofs = slot->slot_base;
                else {
                    // Slot not found - scope data unavailable for this chip
                    // (This is expected for chips without scope support)
                }
            }

            for (int k = i; k < j; k++) {
                int localCh = k - i;
                if (voice_ofs >= 0) {
                    int totalVoices = GetModizerVoiceCount(
                        devIdx < (int)devs.size() ? devs[devIdx].type : 0);
                    int vc = voice_ofs + localCh;
                    // SSG channels (YM2203: ch3-5, YM2608: ch6-8, YM2610: ch4-6)
                    // have their scope data in a separate "SSG" slot.
                    // Channels after SSG (ADPCM-A, ADPCM-B) must subtract SSG count
                    // to match the core slot's continuous layout.
                    UINT8 chipType = devIdx < (int)devs.size() ? devs[devIdx].type : 0;
                    bool isSSGCh = false;
                    int ssgLocalCh = 0;
                    if (chipType == DEVID_YM2203 && localCh >= 3) {
                        isSSGCh = true; ssgLocalCh = localCh - 3;
                    } else if (chipType == DEVID_YM2608 && localCh >= 6 && localCh < 9) {
                        isSSGCh = true; ssgLocalCh = localCh - 6;
                    } else if (chipType == DEVID_YM2610 && localCh >= 4 && localCh < 7) {
                        isSSGCh = true; ssgLocalCh = localCh - 4;
                    }
                    if (isSSGCh) {
                        ScopeChipSlot *ssgSlot = scope_find_slot("SSG", chipInst);
                        if (!ssgSlot && chipInst > 0)
                            ssgSlot = scope_find_slot("SSG", 0);
                        if (ssgSlot) vc = ssgSlot->slot_base + ssgLocalCh;
                        else vc = -1;
                    } else if (chipType == DEVID_YM2608 && localCh >= 9) {
                        // YM2608: FM(0-5) SSG(6-8 in frontend) ADPCM-A(9-14) ADPCM-B(15)
                        // Core slot: FM(0-5) ADPCM-A(6-11) ADPCM-B(12) — no SSG gap
                        vc = voice_ofs + localCh - 3;
                    } else if (chipType == DEVID_YM2610 && localCh >= 7) {
                        // YM2610: FM(0-3) SSG(4-6 in frontend) ADPCM-A(7-12) ADPCM-B(13)
                        // Core slot: FM(0-3) ADPCM-A(4-9) ADPCM-B(10) — no SSG gap
                        vc = voice_ofs + localCh - 3;
                    }
                    meters[k].voice_ch = (localCh < totalVoices && vc >= 0) ? vc : -1;
                } else {
                    meters[k].voice_ch = -1;
                }
            }
            i = j;
        }
    } else {
        for (auto& m : meters) m.voice_ch = -1;
    }

    // --- Scope mode: show placeholder, actual scope rendering is in RenderScopeArea() ---
    if (s_showScope) {
        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), "(oscilloscope display below)");
        ImGui::Dummy(ImVec2(availW, availH));
        return;
    }

    // --- Level meter mode (original) ---
    const float kChipLabelH = 18.0f;  // height reserved for chip name above bars
    const float kBarGap     = 2.0f;   // gap between bars
    const float kGroupGap   = 10.0f;  // extra gap between chip groups
    const float kGroupPad   = 3.0f;   // padding inside group box
    const float kBarH       = availH - kChipLabelH - 2.0f; // actual bar area height

    int n = (int)meters.size();

    // Compute minimum bar width from widest channel label, cached per loaded file
    static float    s_cachedMinBarW   = 16.0f;
    static int      s_cachedMeterCount = 0;
    static std::string s_cachedForPath;
    if (s_cachedForPath != s_loadedPath || s_cachedMeterCount != n) {
        float maxW = 16.0f;
        for (int i = 0; i < n; i++) {
            // Dynamic padding based on label length:
            // - 1-2 chars (drums like BD, HH, or single-digit channels): +2px
            // - 3 chars (F10, 11, etc.): +3px
            // - 4+ chars: +4px
            size_t len = strlen(meters[i].label);
            float pad = (len <= 2) ? 2.0f : (len == 3) ? 3.0f : 4.0f;
            float tw = ImGui::CalcTextSize(meters[i].label).x + pad;
            if (tw > maxW) maxW = tw;
        }
        s_cachedMinBarW    = maxW;
        s_cachedMeterCount = n;
        s_cachedForPath    = s_loadedPath;
    }
    float kMinBarW = s_cachedMinBarW;
    float kPrefBarW = kMinBarW + 4.0f; // a bit of breathing room

    // Count group separators
    int numGroups = 0;
    for (int i = 0; i < n; i++) if (meters[i].group_start) numGroups++;

    // Calculate total width needed at preferred size
    float totalPref = n * (kPrefBarW + kBarGap) + (numGroups - 1) * kGroupGap;

    // Determine actual bar width: shrink to fit, but never below kMinBarW
    float meterW = kPrefBarW;
    if (totalPref > availW) {
        // Try to fit by shrinking bars
        float fitW = (availW - (numGroups - 1) * kGroupGap - (n - 1) * kBarGap) / n;
        meterW = fitW < kMinBarW ? kMinBarW : fitW;
    }

    // Recalculate total content width with chosen meterW
    float totalW = n * (meterW + kBarGap) + (numGroups - 1) * kGroupGap;
    bool needScroll = totalW > availW;

    // Begin scrollable child if needed
    if (needScroll) {
        ImGui::BeginChild("##levelscroll", ImVec2(availW, availH + 16),
            false, ImGuiWindowFlags_HorizontalScrollbar);
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float barTop = p.y + kChipLabelH;

    // Pre-pass: draw group boxes behind all bars
    {
        float gx = p.x;
        for (int i = 0; i < n; ) {
            if (i > 0) gx += kGroupGap;
            float gxStart = gx - kGroupPad;
            int j = i;
            while (j < n && (j == i || !meters[j].group_start)) {
                gx += meterW + kBarGap;
                j++;
            }
            float gxEnd = gx - kBarGap + kGroupPad;
            draw_list->AddRect(
                ImVec2(gxStart, p.y - 1),
                ImVec2(gxEnd,   barTop + kBarH + kGroupPad),
                IM_COL32(80, 80, 100, 200), 3.0f);
            i = j;
        }
    }

    float curX = p.x;
    for (int i = 0; i < n; i++) {
        const LevelMeterEntry& m = meters[i];

        // Group separator gap (skip before first group)
        if (m.group_start && i > 0) curX += kGroupGap;

        float mx = curX;

        // Chip name label above the first bar of each group
        if (m.group_start && m.chip_label[0] != '\0') {
            draw_list->AddText(ImVec2(mx, p.y + 1),
                IM_COL32(160, 220, 255, 255), m.chip_label);
        }

        // Background
        draw_list->AddRectFilled(
            ImVec2(mx, barTop),
            ImVec2(mx + meterW, barTop + kBarH),
            IM_COL32(25, 25, 25, 255));
        draw_list->AddRect(
            ImVec2(mx, barTop),
            ImVec2(mx + meterW, barTop + kBarH),
            m.group_start ? IM_COL32(100, 100, 120, 255) : IM_COL32(60, 60, 60, 255));

        // Level bar (bottom-up); color dimmed by volume level
        if (m.level > 0.0f) {
            float barFillH = kBarH * m.level;
            float fillTop = barTop + kBarH - barFillH;
            ImU32 col;
            if (m.keyon) {
                // Dim the bar color proportionally to level: remap [0,1] -> [0.4,1]
                float dimF = 0.4f + m.level * 0.6f;
                int r = (int)(((m.color >> IM_COL32_R_SHIFT) & 0xFF) * dimF);
                int g = (int)(((m.color >> IM_COL32_G_SHIFT) & 0xFF) * dimF);
                int b = (int)(((m.color >> IM_COL32_B_SHIFT) & 0xFF) * dimF);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                col = IM_COL32(r, g, b, 255);
            } else {
                // Keyoff / decay: use faded channel color, not gray
                float dimF = 0.25f + m.level * 0.2f;
                int r = (int)(((m.color >> IM_COL32_R_SHIFT) & 0xFF) * dimF);
                int g = (int)(((m.color >> IM_COL32_G_SHIFT) & 0xFF) * dimF);
                int b = (int)(((m.color >> IM_COL32_B_SHIFT) & 0xFF) * dimF);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                col = IM_COL32(r, g, b, 255);
            }
            draw_list->AddRectFilled(
                ImVec2(mx + 1, fillTop),
                ImVec2(mx + meterW - 1, fillTop + barFillH),
                col);
        }

        // Channel label inside bar (top of bar area), show if bar wide enough
        if (meterW >= 10.0f) {
            draw_list->AddText(
                ImVec2(mx + 1, barTop + 2),
                IM_COL32(200, 200, 200, 255), m.label);
        }

        curX += meterW + kBarGap;
    }

    // Advance cursor past the drawn area
    float drawnW = needScroll ? totalW : availW;
    ImGui::Dummy(ImVec2(drawnW, availH));

    if (needScroll) {
        ImGui::EndChild();
    }
}

void RenderScopeArea() {
    // Render oscilloscope area - displays multi-row oscilloscope visualization
    // Only renders when s_showScope is true
    if (!s_showScope) {
        return;
    }

    UpdateShadowRegisters();
    std::vector<LevelMeterEntry> meters = BuildLevelMeters();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    if (meters.empty()) {
        return;
    }

    // Compute modizer voice channel offsets per chip group (for oscilloscope)
    for (int i = 0; i < (int)meters.size(); ) {
        // Count channels in this group
        int j = i + 1;
        while (j < (int)meters.size() && !meters[j].group_start) j++;

        // Get chip name and instance from device info
        const auto& devs = GetActiveDevices();
        int devIdx = 0;
        for (int k = 0; k < i; k++) {
            if (meters[k].group_start) devIdx++;
        }
        const char* scopeName = NULL;
        int chipInst = 0;
        if (devIdx < (int)devs.size()) {
            UINT8 chipType = devs[devIdx].type;
            // Use chip instance from device info to distinguish multiple chips (0 or 1 from VGM)
            chipInst = (int)devs[devIdx].instance;
            // Map dev type to scope registration name
            switch (chipType) {
                case DEVID_SN76496:  scopeName = "SN76489";  break;
                case DEVID_YM2413:   scopeName = "YM2413";   break;
                case DEVID_YM2612:   scopeName = "YM2612";   break;
                case DEVID_YM2151:   scopeName = "YM2151";   break;
                case DEVID_SEGAPCM:  scopeName = "SEGAPCM";  break;
                case DEVID_RF5C68:   scopeName = "RF5C68";   break;
                case DEVID_YM2203:   scopeName = "YM2203";   break;
                case DEVID_YM2608:   scopeName = "YM2608";   break;
                case DEVID_YM2610:   scopeName = "YM2610";   break;
                case DEVID_YM3812:   scopeName = "OPL2";     break;
                case DEVID_YM3526:   scopeName = "OPL2";     break;
                case DEVID_Y8950:    scopeName = "Y8950";    break;
                case DEVID_YMF262:   scopeName = "YMF262";   break;
                case DEVID_YMF278B:  scopeName = "YMF278B";  break;
                case DEVID_YMF271:   scopeName = "YMF271";   break;
                case DEVID_YMZ280B:  scopeName = "YMZ280B";  break;
                case DEVID_32X_PWM:  scopeName = "32X_PWM";  break;
                case DEVID_AY8910:   scopeName = "SSG";      break;
                case DEVID_GB_DMG:   scopeName = "GB_DMG";   break;
                case DEVID_NES_APU:  scopeName = "NES_APU";  break;
                case DEVID_YMW258:   scopeName = "MultiPCM"; break;
                case DEVID_uPD7759:  scopeName = "uPD7759";  break;
                case DEVID_OKIM6258: scopeName = "OKIM6258"; break;
                case DEVID_OKIM6295: scopeName = "OKIM6295"; break;
                case DEVID_K051649:  scopeName = "SCC";      break;
                case DEVID_K054539:  scopeName = "K054539";  break;
                case DEVID_C6280:    scopeName = "C6280";    break;
                case DEVID_C140:     scopeName = "C140";     break;
                case DEVID_C219:     scopeName = "C219";     break;
                case DEVID_K053260:  scopeName = "K053260";  break;
                case DEVID_POKEY:    scopeName = "POKEY";    break;
                case DEVID_QSOUND:   scopeName = "QSOUND";   break;
                case DEVID_SCSP:     scopeName = "SCSP";     break;
                case DEVID_WSWAN:    scopeName = "WSWAN";    break;
                case DEVID_VBOY_VSU: scopeName = "VBOY_VSU"; break;
                case DEVID_SAA1099:  scopeName = "SAA1099";  break;
                case DEVID_ES5503:   scopeName = "ES5503";   break;
                case DEVID_ES5506:   scopeName = "ES5506";   break;
                case DEVID_X1_010:   scopeName = "X1_010";   break;
                case DEVID_C352:     scopeName = "C352";     break;
                case DEVID_GA20:     scopeName = "GA20";     break;
                case DEVID_MIKEY:    scopeName = "MIKEY";    break;
                case DEVID_K007232:  scopeName = "K007232";  break;
                case DEVID_K005289:  scopeName = "K005289";  break;
                case DEVID_MSM5205:  scopeName = "MSM5205";  break;
                default: break;
            }
        }

        // Find scope slot to get the buffer base offset
        int voice_ofs = -1;
        if (scopeName) {
            ScopeChipSlot *slot = scope_find_slot(scopeName, chipInst);
            // Dual-chip fallback: core may only register chip_inst=0
            if (!slot && chipInst > 0)
                slot = scope_find_slot(scopeName, 0);
            if (slot) voice_ofs = slot->slot_base;
        }

        for (int k = i; k < j; k++) {
            int localCh = k - i;
            if (voice_ofs >= 0) {
                int totalVoices = GetModizerVoiceCount(
                    devIdx < (int)devs.size() ? devs[devIdx].type : 0);
                int vc = voice_ofs + localCh;
                UINT8 chipType = devIdx < (int)devs.size() ? devs[devIdx].type : 0;
                bool isSSGCh = false;
                int ssgLocalCh = 0;
                if (chipType == DEVID_YM2203 && localCh >= 3) {
                    isSSGCh = true; ssgLocalCh = localCh - 3;
                } else if (chipType == DEVID_YM2608 && localCh >= 6 && localCh < 9) {
                    isSSGCh = true; ssgLocalCh = localCh - 6;
                } else if (chipType == DEVID_YM2610 && localCh >= 4 && localCh < 7) {
                    isSSGCh = true; ssgLocalCh = localCh - 4;
                }
                if (isSSGCh) {
                    ScopeChipSlot *ssgSlot = scope_find_slot("SSG", chipInst);
                    if (!ssgSlot && chipInst > 0)
                        ssgSlot = scope_find_slot("SSG", 0);
                    if (ssgSlot) vc = ssgSlot->slot_base + ssgLocalCh;
                    else vc = -1;
                } else if (chipType == DEVID_YM2608 && localCh >= 9) {
                    vc = voice_ofs + localCh - 3;
                } else if (chipType == DEVID_YM2610 && localCh >= 7) {
                    vc = voice_ofs + localCh - 3;
                } else if (chipType == DEVID_YM2413 && localCh >= 9) {
                    // YM2413: FM channels 0-8, Rhythm channels 9-13
                    // Rhythm channels map to voice buffer indices 9-13
                    vc = voice_ofs + localCh;
                }
                meters[k].voice_ch = (localCh < totalVoices && vc >= 0) ? vc : -1;
            } else {
                meters[k].voice_ch = -1;
            }
        }
        i = j;
    }

    // Include channels with scope data for oscilloscope display
    // Channels without scope data (voice_ch == -1) are excluded from scope
    // but still appear in piano keyboard and level meter (meters is used elsewhere)
    std::vector<LevelMeterEntry> scopeMeters;
    for (const auto& m : meters) {
        if (m.voice_ch >= 0)
            scopeMeters.push_back(m);
    }

    // Debug: log YM2413 rhythm channels buffer data
    for (int idx = 0; idx < (int)scopeMeters.size(); idx++) {
        const auto& m = scopeMeters[idx];
        if (m.label[0] == 'B' || m.label[0] == 'H' || m.label[0] == 'S' ||
            (m.label[0] == 'T' && m.label[1] == 'M') || m.label[0] == 'C') {
            // Rhythm channel - check actual buffer data
            if (m.voice_ch >= 0 && m.voice_ch < SOUND_MAXVOICES_BUFFER_FX && m_voice_buff[m.voice_ch]) {
                int64_t samples = (int)m_voice_current_ptr[m.voice_ch];
                if (samples > 0) {
                    int idx_pos = ((int)samples - 1) & 0xFFF;
                    signed char last_sample = m_voice_buff[m.voice_ch][idx_pos];
                    // Debug output removed
                }
            }
        }
    }

    int n = (int)scopeMeters.size();
    if (n == 0) {
        return;
    }

    const float kChipLabelH = 18.0f;
    const float kBarGap     = 2.0f;
    const float kGroupGap   = 10.0f;
    const float kGroupPad   = 3.0f;

    // Max width per row before wrapping
    float maxWidth = 990.0f;

    // Build chip groups and calculate layout
    struct ChipGroup {
        int startIdx;
        int endIdx;
        int channelCount;
        float width;
    };
    std::vector<ChipGroup> groups;

    for (int i = 0; i < (int)scopeMeters.size(); ) {
        int j = i + 1;
        while (j < (int)scopeMeters.size() && !scopeMeters[j].group_start) j++;
        float chipW = s_scopeChipSettings[scopeMeters[i].devType].width;
        float grpWidth = (j - i) * (chipW + kBarGap) - kBarGap;
        groups.push_back({i, j, j - i, grpWidth});
        i = j;
    }

    // Distribute groups into rows - keep each group intact, multiple groups per row if width permits
    struct RowGroup {
        int groupIdx;
        int startCh;   // channel offset within group (0 for full group)
        int endCh;     // end channel within group (groupCount for full group)
        float xOffset;
    };
    struct Row {
        std::vector<RowGroup> rowGroups;
        float width;
    };
    std::vector<Row> rows;

    for (int g = 0; g < (int)groups.size(); g++) {
        const ChipGroup& grp = groups[g];
        float grpWithGap = grp.width + kGroupGap;

        // If this single group exceeds maxWidth, split it across multiple rows
        if (grp.width > maxWidth) {
            // Calculate how many rows this group needs
            UINT8 devType = scopeMeters[grp.startIdx].devType;
            float chWidth = s_scopeChipSettings[devType].width + kBarGap;
            int maxChPerRow = (int)((maxWidth + kBarGap) / chWidth);
            if (maxChPerRow < 1) maxChPerRow = 1;

            int chDone = 0;
            while (chDone < grp.channelCount) {
                int chInThisRow = std::min(maxChPerRow, grp.channelCount - chDone);
                if (rows.empty() || rows.back().width + chInThisRow * chWidth > maxWidth) {
                    rows.push_back({});
                }
                float xOffset = rows.back().width;
                if (!rows.back().rowGroups.empty()) xOffset += kGroupGap;
                rows.back().rowGroups.push_back({g, chDone, chDone + chInThisRow, xOffset});
                rows.back().width = xOffset + chInThisRow * chWidth - kBarGap;
                chDone += chInThisRow;
            }
        } else {
            // Normal case: group fits in one row
            bool needsNewRow = rows.empty() ||
                              (rows.back().width + grpWithGap > maxWidth);

            if (needsNewRow) {
                rows.push_back({});
            }

            float xOffset = rows.back().width;
            if (!rows.back().rowGroups.empty()) xOffset += kGroupGap;
            rows.back().rowGroups.push_back({g, 0, grp.channelCount, xOffset});
            rows.back().width = xOffset + grp.width;
        }
    }

    int numRows = (int)rows.size();
    float totalRowH = numRows * (s_scopeHeight + kChipLabelH + kBarGap);

    // Create scrollable background container that can hold multiple rows
    ImGui::BeginChild("##scopeBackground", ImVec2(availW, availH),
        false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    // Draw scopes row by row
    for (int row = 0; row < numRows; row++) {
        float rowY = p.y + row * (s_scopeHeight + kChipLabelH + kBarGap);
        float scopeTop = rowY + kChipLabelH;
        const Row& currentRow = rows[row];

        // Draw group boxes for this row
        for (const RowGroup& rg : currentRow.rowGroups) {
            const ChipGroup& grp = groups[rg.groupIdx];
            float xStart = p.x + rg.xOffset - kGroupPad;
            float xEnd = p.x + rg.xOffset + grp.width + kGroupPad;
            draw_list->AddRect(
                ImVec2(xStart, rowY - 1),
                ImVec2(xEnd, scopeTop + s_scopeHeight + kGroupPad),
                IM_COL32(80, 80, 100, 200), 3.0f);
        }

        // Draw channels in this row
        for (const RowGroup& rg : currentRow.rowGroups) {
            const ChipGroup& grp = groups[rg.groupIdx];
            float curX = p.x + rg.xOffset;

            for (int chOffset = rg.startCh; chOffset < rg.endCh; chOffset++) {
                int chIdx = grp.startIdx + chOffset;
                const LevelMeterEntry& m = scopeMeters[chIdx];
                float mx = curX;

                // Draw chip label at start of each group segment
                if (chOffset == rg.startCh && m.chip_label[0] != '\0') {
                    draw_list->AddText(ImVec2(mx, rowY + 1),
                        IM_COL32(160, 220, 255, 255), m.chip_label);
                }

                const auto& cs = s_scopeChipSettings[m.devType];
                float chipW = cs.width;
                s_scope.DrawChannel(m.voice_ch, draw_list, mx, scopeTop, chipW, s_scopeHeight, cs.amplitude, m.color, m.keyon, m.level,
                    cs.samples, cs.offset,
                    cs.search_window, cs.edge_align, false, cs.ac_mode, cs.legacy_mode);

                // Channel label inside scope
                if (chipW >= 10.0f) {
                    draw_list->AddText(
                        ImVec2(mx + 1, scopeTop + 2),
                        IM_COL32(200, 200, 200, 255), m.label);
                }

                curX += chipW + kBarGap;
            }
        }
    }

    ImGui::Dummy(ImVec2(availW, totalRowH));
    ImGui::EndChild();
}

void RenderPianoArea() {
    // Ensure shadow registers are up-to-date before reading chip state
    UpdateShadowRegisters();

    // Collect active notes from current chip state via BuildLevelMeters
    std::vector<LevelMeterEntry> meters;
    if (s_fileLoaded && !s_loading) meters = BuildLevelMeters();

    // Build note->channel info (collect all active channels per note)
    struct NoteEntry { ImU32 color; float level; char tag[12]; }; // tag = "SAA\nS1"
    std::vector<NoteEntry> noteEntries[128];
    bool  noteActive[128] = {};
    bool  noteKeyoff[128] = {};   // true = recently released, show faded color
    ImU32 noteColor[128]  = {};
    float noteLevel[128]  = {};
    float notePitchOffset[128] = {}; // fractional semitone offset for vibrato indicator
    float noteTremolo[128]     = {}; // AMS tremolo [-1, 1] for vertical indicator
    {
        for (const auto& m : meters) {
            if (m.keyon && m.note >= 0 && m.note < 128) {
                noteActive[m.note] = true;
                if (m.keyoff) noteKeyoff[m.note] = true;
                // Always assign color from first (or highest-level) channel, even if level=0
                if (noteColor[m.note] == 0 || m.level > noteLevel[m.note]) {
                    noteLevel[m.note] = m.level;
                    noteColor[m.note] = m.color;
                    notePitchOffset[m.note] = m.pitch_offset;
                }
                if (fabsf(m.tremolo) > fabsf(noteTremolo[m.note]))
                    noteTremolo[m.note] = m.tremolo;
                // Check if same chip already has an entry on this note
                bool chipSeen = false;
                for (const auto& ex : noteEntries[m.note]) {
                    const char* nl = strchr(ex.tag, '\n');
                    int l1 = nl ? (int)(nl - ex.tag) : (int)strlen(ex.tag);
                    if (l1 == (int)strlen(m.chip_abbrev) &&
                        strncmp(ex.tag, m.chip_abbrev, l1) == 0) { chipSeen = true; break; }
                }
                NoteEntry ne;
                ne.color = m.color; ne.level = m.level;
                // Only show chip abbrev on first channel of this chip at this note
                if (chipSeen)
                    snprintf(ne.tag, sizeof(ne.tag), "\n%s", m.label);
                else
                    snprintf(ne.tag, sizeof(ne.tag), "%s\n%s", m.chip_abbrev, m.label);
                noteEntries[m.note].push_back(ne);
            }
        }
    }
    // Color debug log (throttled: log every 30 frames when any active note exists)
    {
        static int s_colorLogFrame = 0;
        s_colorLogFrame++;
        bool hasActive = false;
        for (int n = 0; n < 128; n++) if (noteActive[n]) { hasActive = true; break; }
        if (hasActive && s_colorLogFrame % 30 == 0) {
            for (int n = 0; n < 128; n++) {
                if (noteActive[n]) {
                    int r = (noteColor[n] >> IM_COL32_R_SHIFT) & 0xFF;
                    int g = (noteColor[n] >> IM_COL32_G_SHIFT) & 0xFF;
                    int b = (noteColor[n] >> IM_COL32_B_SHIFT) & 0xFF;
                    // VgmLog("[Piano] note=%d keyoff=%d color=(%d,%d,%d) level=%.3f\n",
                    //     n, noteKeyoff[n] ? 1 : 0, r, g, b, noteLevel[n]);
                }
            }
        }
    }
    // Helper: blend color with white/black key base by level
    // Minimum blend factor 0.55 so low-volume keys still show clear channel color
    // keyoff: use moderate blend (0.35..0.55) for visible but lighter channel color
    auto blendKey = [](ImU32 col, float lv, bool isBlack, bool isKeyOff = false) -> ImU32 {
        float blendLv;
        if (isKeyOff)
            blendLv = 0.35f + lv * 0.2f;  // remap [0,1] -> [0.35,0.55] — light tint, no gray
        else
            blendLv = 0.55f + lv * 0.45f;  // remap [0,1] -> [0.55,1]
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

    // Display range: C1 (MIDI 24) to B7 (MIDI 107) = 7 octaves
    const int kMinNote = 24;
    const int kMaxNote = 107;
    static const bool kIsBlack[] = {false,true,false,true,false,false,true,false,true,false,true,false};

    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) if (!kIsBlack[n % 12]) numWhiteKeys++;

    float whiteKeyWidth = availW / (float)numWhiteKeys;
    if (whiteKeyWidth < 6.0f) whiteKeyWidth = 6.0f;
    float blackKeyWidth = whiteKeyWidth * 0.65f;

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (kIsBlack[n % 12]) continue;
        float x = p.x + wkIdx * whiteKeyWidth;
        ImU32 fillCol = noteActive[n] ? blendKey(noteColor[n], noteLevel[n], false, noteKeyoff[n]) : IM_COL32(255,255,255,255);
        draw_list->AddRectFilled(ImVec2(x, p.y), ImVec2(x+whiteKeyWidth-1, p.y+whiteKeyHeight), fillCol);
        draw_list->AddRect(ImVec2(x, p.y), ImVec2(x+whiteKeyWidth, p.y+whiteKeyHeight), IM_COL32(80,80,80,255));
        if (n % 12 == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "C%d", (n / 12) - 1);
            ImVec2 tsz = ImGui::CalcTextSize(buf);
            float noteNameY = p.y + whiteKeyHeight - tsz.y - 2;
            draw_list->AddText(ImVec2(x+2, noteNameY), IM_COL32(0,0,0,180), buf);
        }
        // Draw channel labels on active white keys, stacked above note name
        if (s_showPianoLabels && noteActive[n] && !noteEntries[n].empty()) {
            float lineH = s_pianoLabelFontSize;
            // Note name occupies bottom: lineH + 2px. Labels go above it.
            // Compute base Y for bottom-most label line (just above note name area)
            float baseLabelY = p.y + whiteKeyHeight - lineH - 2 - (float)noteEntries[n].size() * lineH - s_pianoLabelOffsetY;
            // Clamp so labels don't go above key top
            if (baseLabelY < p.y + 1) baseLabelY = p.y + 1;
            ImFont* lblFont = ImGui::GetFont();
            for (int li = 0; li < (int)noteEntries[n].size(); li++) {
                const NoteEntry& ne = noteEntries[n][li];
                float ly = baseLabelY + li * lineH;
                // Split tag at '\n' into two parts (chip + ch)
                const char* nl = strchr(ne.tag, '\n');
                char line1[12] = "", line2[8] = "";
                if (nl) {
                    int l1 = (int)(nl - ne.tag);
                    if (l1 > 11) l1 = 11;
                    strncpy(line1, ne.tag, l1); line1[l1] = '\0';
                    snprintf(line2, sizeof(line2), "%s", nl+1);
                } else {
                    snprintf(line1, sizeof(line1), "%s", ne.tag);
                }
                // Determine text color: dark on light keys, light on dark keys
                ImU32 tc = IM_COL32(0,0,0,220);
                // Draw chip name and channel on consecutive lines if space allows
                if (whiteKeyWidth >= 12.0f) {
                    draw_list->AddText(lblFont, s_pianoLabelFontSize, ImVec2(x+1, ly),         tc, line1);
                    draw_list->AddText(lblFont, s_pianoLabelFontSize, ImVec2(x+1, ly + lineH), tc, line2);
                } else {
                    // Narrow key: just draw channel label
                    draw_list->AddText(lblFont, s_pianoLabelFontSize, ImVec2(x+1, ly), tc, line2);
                }
            }
        }
        wkIdx++;
    }

    // Pass 2: black keys (drawn on top)
    wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!kIsBlack[n % 12]) { wkIdx++; continue; }
        float x = p.x + (wkIdx - 1) * whiteKeyWidth + whiteKeyWidth - blackKeyWidth * 0.5f;
        ImU32 fillCol = noteActive[n] ? blendKey(noteColor[n], noteLevel[n], true, noteKeyoff[n]) : IM_COL32(20,20,20,255);
        draw_list->AddRectFilled(ImVec2(x, p.y), ImVec2(x+blackKeyWidth, p.y+blackKeyHeight), fillCol);
        draw_list->AddRect(ImVec2(x, p.y), ImVec2(x+blackKeyWidth, p.y+blackKeyHeight), IM_COL32(0,0,0,255));
        // Draw channel labels on active black keys (inverted colors)
        if (s_showPianoLabels && noteActive[n] && !noteEntries[n].empty()) {
            float lineH = s_pianoLabelFontSize;
            float baseLabelY = p.y + blackKeyHeight - (float)noteEntries[n].size() * lineH - s_pianoLabelOffsetY;
            if (baseLabelY < p.y + 1) baseLabelY = p.y + 1;
            ImU32 tc = IM_COL32(255,255,255,220);
            ImFont* lblFont = ImGui::GetFont();
            for (int li = 0; li < (int)noteEntries[n].size(); li++) {
                const NoteEntry& ne = noteEntries[n][li];
                float ly = baseLabelY + li * lineH;
                const char* nl = strchr(ne.tag, '\n');
                char line1[12] = "", line2[8] = "";
                if (nl) {
                    int l1 = (int)(nl - ne.tag);
                    if (l1 > 11) l1 = 11;
                    strncpy(line1, ne.tag, l1); line1[l1] = '\0';
                    snprintf(line2, sizeof(line2), "%s", nl+1);
                } else {
                    snprintf(line1, sizeof(line1), "%s", ne.tag);
                }
                if (blackKeyWidth >= 12.0f) {
                    draw_list->AddText(lblFont, s_pianoLabelFontSize, ImVec2(x+1, ly),         tc, line1);
                    draw_list->AddText(lblFont, s_pianoLabelFontSize, ImVec2(x+1, ly + lineH), tc, line2);
                } else {
                    draw_list->AddText(lblFont, s_pianoLabelFontSize, ImVec2(x+1, ly), tc, line2);
                }
            }
        }
    }

    // Pass 3: LFO indicators — PMS (horizontal shake) and AMS (vertical pulse)
    {
        // Build key center-x table (integer notes)
        float keyX[128] = {};
        {
            int wi = 0;
            for (int n = kMinNote; n <= kMaxNote; n++) {
                if (!kIsBlack[n % 12]) {
                    keyX[n] = p.x + wi * whiteKeyWidth + whiteKeyWidth * 0.5f;
                    wi++;
                } else {
                    keyX[n] = p.x + (wi - 1) * whiteKeyWidth + whiteKeyWidth - blackKeyWidth * 0.5f + blackKeyWidth * 0.5f;
                }
            }
        }
        auto noteToX = [&](float fnote) -> float {
            int n0 = (int)floorf(fnote);
            int n1 = n0 + 1;
            float frac = fnote - (float)n0;
            float x0 = (n0 >= kMinNote && n0 <= kMaxNote) ? keyX[n0] : keyX[kMinNote];
            float x1 = (n1 >= kMinNote && n1 <= kMaxNote) ? keyX[n1] : keyX[kMaxNote];
            return x0 + (x1 - x0) * frac;
        };
        auto noteToH = [&](int note) -> float {
            if (note < 0) note = 0;
            if (note > 127) note = 127;
            return kIsBlack[note % 12] ? blackKeyHeight : whiteKeyHeight;
        };
        for (int n = kMinNote; n <= kMaxNote; n++) {
            if (!noteActive[n]) continue;
            float poff = notePitchOffset[n];
            float trem = noteTremolo[n];
            if (poff >  1.0f) poff =  1.0f;
            if (poff < -1.0f) poff = -1.0f;
            bool hasPMS = fabsf(poff) >= 0.02f;
            bool hasAMS = fabsf(trem) >= 0.02f;
            if (!hasPMS && !hasAMS) continue;
            ImU32 base_col = noteColor[n];
            float alpha = fmaxf(0.5f, noteLevel[n]);
            ImVec4 cv = ImGui::ColorConvertU32ToFloat4(base_col);
            cv.w = alpha;
            ImU32 col = ImGui::ColorConvertFloat4ToU32(cv);
            float hl_w = whiteKeyWidth * 0.3f;
            float kh = noteToH(n);
            // PMS: horizontal shake indicator (shifted left/right from note center)
            if (hasPMS) {
                float fnote_target = (float)n + poff;
                float highlight_x  = noteToX(fnote_target);
                draw_list->AddRectFilled(
                    ImVec2(highlight_x - hl_w * 0.5f, p.y),
                    ImVec2(highlight_x + hl_w * 0.5f, p.y + kh),
                    col);
            }
            // AMS: vertical pulse indicator (pulsing bar at top of key)
            if (hasAMS) {
                float pulse_h = kh * 0.15f * fabsf(trem); // height proportional to tremolo depth
                float cx = keyX[n];
                draw_list->AddRectFilled(
                    ImVec2(cx - hl_w * 0.5f, p.y),
                    ImVec2(cx + hl_w * 0.5f, p.y + pulse_h + 2.0f),
                    col);
            }
        }
    }

    ImGui::Dummy(ImVec2((float)numWhiteKeys * whiteKeyWidth, whiteKeyHeight));
}

// Helper: render a 16-column hex register grid for a flat 256-byte chip
static void RenderRegGrid(const char* tableId, const UINT8* regs, int numRows) {
    if (ImGui::BeginTable(tableId, 17,
            ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 32);
        for (int c = 0; c < 16; c++) {
            char buf[4]; snprintf(buf, sizeof(buf), "+%X", c);
            ImGui::TableSetupColumn(buf, ImGuiTableColumnFlags_WidthFixed, 30);
        }
        ImGui::TableHeadersRow();
        for (int row = 0; row < numRows; row++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%02X", row << 4);
            for (int col = 0; col < 16; col++) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%02X", regs[(row << 4) | col]);
            }
        }
        ImGui::EndTable();
    }
}

void RenderStatusArea() {
    UpdateShadowRegisters();

    ImGui::TextColored(ImVec4(0.4f,1.0f,0.8f,1.0f), "Chip Register State");
    ImGui::Separator();

    if (!s_fileLoaded) {
        ImGui::TextDisabled("(no file loaded)");
        return;
    }

    // Determine which chips are present by checking if any shadow reg is non-zero
    // or by consulting PlayerA device info
    PlayerBase* pBase = s_player.GetPlayer();
    if (!pBase) { ImGui::TextDisabled("(player not ready)"); return; }

    std::vector<PLR_DEV_INFO> devList;
    pBase->GetSongDeviceInfo(devList);

    if (devList.empty()) {
        ImGui::TextDisabled("(no chip info)");
        return;
    }

    const char* note_names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    (void)note_names;

    for (const auto& dev : devList) {
        char hdr[80];
        snprintf(hdr, sizeof(hdr), "%s  (ID %02X)", "", dev.type);

        // Build human-readable chip name
        const char* chipName = "Unknown";
        switch (dev.type) {
            case DEVID_YM2612:  chipName = "YM2612 (OPN2)"; break;
            case DEVID_YM2151:  chipName = "YM2151 (OPM)"; break;
            case DEVID_YM2413:  chipName = "YM2413 (OPLL)"; break;
            case DEVID_YM2203:  chipName = "YM2203 (OPN)"; break;
            case DEVID_YM2608:  chipName = "YM2608 (OPNA)"; break;
            case DEVID_YM2610:  chipName = "YM2610 (OPNB)"; break;
            case DEVID_YMF262:  chipName = "YMF262 (OPL3)"; break;
            case DEVID_YM3812:  chipName = "YM3812 (OPL2)"; break;
            case DEVID_YM3526:  chipName = "YM3526 (OPL1)"; break;
            case DEVID_Y8950:   chipName = "Y8950 (OPL MSX)"; break;
            case DEVID_YMF271:  chipName = "YMF271 (OPX)"; break;
            case DEVID_YMF278B: chipName = "YMF278B (OPL4)"; break;
            case DEVID_SN76496: chipName = "SN76496 (DCSG)"; break;
            case DEVID_AY8910:  chipName = "AY8910 (PSG)"; break;
            case DEVID_C6280:   chipName = "C6280 (HuC6280)"; break;
            case DEVID_SAA1099: chipName = "SAA1099"; break;
            case DEVID_POKEY:   chipName = "POKEY"; break;
            case DEVID_NES_APU: chipName = "NES APU (2A03)"; break;
            case DEVID_GB_DMG:  chipName = "GB DMG APU"; break;
            case DEVID_OKIM6258: chipName = "OKIM6258"; break;
            case DEVID_OKIM6295: chipName = "OKIM6295"; break;
            case DEVID_RF5C68:  chipName = "RF5C68"; break;
            case DEVID_K051649: chipName = "K051649 (SCC)"; break;
            case DEVID_WSWAN:   chipName = "WonderSwan"; break;
            case DEVID_YMZ280B: chipName = "YMZ280B (ADPCM)"; break;
            case DEVID_YMW258:  chipName = "YMW258 (MultiPCM)"; break;
            case DEVID_uPD7759: chipName = "uPD7759 (ADPCM)"; break;
            case DEVID_K054539: chipName = "K054539 (PCM)"; break;
            case DEVID_C140:    chipName = "C140 (PCM)"; break;
            case DEVID_K053260: chipName = "K053260 (PCM)"; break;
            case DEVID_QSOUND:  chipName = "QSound"; break;
            case DEVID_SCSP:    chipName = "SCSP (YMF292)"; break;
            case DEVID_VBOY_VSU:chipName = "VirtualBoy VSU"; break;
            case DEVID_ES5503:  chipName = "ES5503 DOC"; break;
            case DEVID_ES5506:  chipName = "ES5506 OTTO"; break;
            case DEVID_X1_010:  chipName = "Seta X1-010"; break;
            case DEVID_C352:    chipName = "C352 (PCM)"; break;
            case DEVID_GA20:    chipName = "Irem GA20"; break;
            case DEVID_SEGAPCM: chipName = "Sega PCM"; break;
            case DEVID_32X_PWM: chipName = "32X PWM"; break;
        }
        snprintf(hdr, sizeof(hdr), "%s", chipName);

        if (!ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        switch (dev.type) {
            case DEVID_YM2612: {
                int c = dev.instance & 1;
                static const int  kOPN2SlotOff[4] = {0x00, 0x08, 0x04, 0x0C};
                static const char* kOPN2OpName[4] = {"M1","M2","C1","C2"};
                static const char* pan_str[]       = {"--"," R"," L","LR"};
                static const char* note_names[]    = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                for (int ch = 0; ch < 6; ch++) {
                    int port = (ch < 3) ? 0 : 1;
                    int off  = ch % 3;
                    const UINT8* r = s_shadowYM2612[c][port];
                    UINT8 b4  = r[0xA4 + off]; UINT8 b0 = r[0xA0 + off];
                    int fnum  = ((b4 & 0x07) << 8) | b0;
                    int block = (b4 >> 3) & 0x07;
                    int pan   = (r[0xB4 + off] >> 6) & 0x03;
                    int algo  = r[0xB0 + off] & 0x07;
                    int fb    = (r[0xB0 + off] >> 3) & 0x07;
                    int pms   = r[0xB4 + off] & 0x07;
                    int ams   = (r[0xB4 + off] >> 4) & 0x03;
                    bool keyon = s_ym2612KeyOn[c][ch];
                    bool keyoff = s_ym2612KeyOff[c][ch];
                    int nt = (keyon && fnum > 0) ? FrequencyToMIDINote(
                        (float)fnum * 7670454.0f / (float)(144 * (1 << (21 - block)))) : -1;
                    if (nt >= 0) nt -= 12;  // YM2612: shift down 1 octave for display
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "CH%d##2612ch%d_%d", ch+1, ch, c);
                    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_DefaultOpen;
                    bool nodeOpen = ImGui::TreeNodeEx(chLabel, nodeFlags);
                    if (nodeOpen) {
                        // Channel info row in fixed-width table — no jitter
                        char noteBuf2612[8];
                        if (keyon && nt >= 0)
                            snprintf(noteBuf2612, sizeof(noteBuf2612), "%s%d", note_names[nt%12], nt/12-1);
                        else if (keyon)
                            snprintf(noteBuf2612, sizeof(noteBuf2612), "F%03X/%d", fnum, block);
                        else
                            snprintf(noteBuf2612, sizeof(noteBuf2612), "---");
                        // Status text and color
                        const char* statusText;
                        ImU32 statusCol;
                        if (keyon && !keyoff) { statusText = "ON";  statusCol = IM_COL32(80,200,80,255); }
                        else if (keyon && keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                        else if (keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                        else { statusText = "--"; statusCol = IM_COL32(100,100,100,255); }
                        ImVec4 infoCol = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        if (ImGui::BeginTable("##2612info", 8, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("AL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("FB",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableSetupColumn("PMS", ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableSetupColumn("AMS", ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(
                                ImVec4(((statusCol>>0)&0xFF)/255.f,((statusCol>>8)&0xFF)/255.f,((statusCol>>16)&0xFF)/255.f,1),
                                "%s", statusText);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol, "%s", noteBuf2612);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol, "%d", algo);
                            ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoCol, "%d", fb);
                            ImGui::TableSetColumnIndex(4); ImGui::TextColored(infoCol, "%s", pan_str[pan]);
                            ImGui::TableSetColumnIndex(5); ImGui::TextColored(infoCol, "%d", pms);
                            ImGui::TableSetColumnIndex(6); ImGui::TextColored(infoCol, "%d", ams);
                            ImGui::EndTable();
                        }
                        if (ImGui::BeginTable("##2612ops", 11,
                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("D1R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                            ImGui::TableSetupColumn("D2R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                            ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                            ImGui::TableSetupColumn("KS",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("DT",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("AMS", ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableHeadersRow();
                            for (int op = 0; op < 4; op++) {
                                int s    = kOPN2SlotOff[op] + off;
                                UINT8 ar  = r[0x50 + s] & 0x1F;
                                UINT8 d1r = r[0x60 + s] & 0x1F;
                                UINT8 d2r = r[0x70 + s] & 0x1F;
                                UINT8 rr  = r[0x80 + s] & 0x0F;
                                UINT8 sl  = (r[0x80 + s] >> 4) & 0x0F;
                                UINT8 tl  = r[0x40 + s] & 0x7F;
                                UINT8 ks  = (r[0x50 + s] >> 6) & 0x03;
                                UINT8 ml  = r[0x30 + s] & 0x0F;
                                UINT8 dt  = (r[0x30 + s] >> 4) & 0x07;
                                UINT8 ams = (r[0x60 + s] >> 7) & 0x01;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);  ImGui::Text("%s", kOPN2OpName[op]);
                                ImGui::TableSetColumnIndex(1);  ImGui::Text("%d", ar);
                                ImGui::TableSetColumnIndex(2);  ImGui::Text("%d", d1r);
                                ImGui::TableSetColumnIndex(3);  ImGui::Text("%d", d2r);
                                ImGui::TableSetColumnIndex(4);  ImGui::Text("%d", rr);
                                ImGui::TableSetColumnIndex(5);  ImGui::Text("%d", sl);
                                ImGui::TableSetColumnIndex(6);  ImGui::Text("%d", tl);
                                ImGui::TableSetColumnIndex(7);  ImGui::Text("%d", ks);
                                ImGui::TableSetColumnIndex(8);  ImGui::Text("%d", ml);
                                ImGui::TableSetColumnIndex(9);  ImGui::Text("%d", dt);
                                ImGui::TableSetColumnIndex(10); ImGui::Text("%d", ams);
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("Port 0 Regs##2612p0")) {
                    RenderRegGrid("ym2612p0", s_shadowYM2612[c][0], 16);
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Port 1 Regs##2612p1")) {
                    RenderRegGrid("ym2612p1", s_shadowYM2612[c][1], 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YM2151: {
                int c = dev.instance & 1;
                // YM2151 (OPM): 8 channels x 4 OP
                // OP slot index = opOff[op] + ch  (M1=0..7, M2=8..15, C1=16..23, C2=24..31)
                static const char* kOPMOpName[4] = {"M1","M2","C1","C2"};
                static const int   kOPMOpOff[4]  = {0, 8, 16, 24};
                // KC note decode: bits6:4=octave, bits3:0=semitone (0-13, skip 2&6)
                static const int kOPMKcToSemi[16] = {0,-1,0,1,2,-1,2,3,4,5,-1,5,6,7,-1,7}; // -1=invalid
                static const char* kOPMNoteName[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                static const char* pan_str2151[] = {"--"," R"," L","LR"};
                for (int ch = 0; ch < 8; ch++) {
                    UINT8 kc    = s_shadowYM2151[c][0x28 + ch];
                    bool  keyon = s_ym2151KeyOn[c][ch];
                    bool  keyoff = s_ym2151KeyOff[c][ch];
                    UINT8 alFb  = s_shadowYM2151[c][0x20 + ch];
                    int   al    = alFb & 0x07;
                    int   fb    = (alFb >> 3) & 0x07;
                    int   pan   = (alFb >> 6) & 0x03;
                    // PMS/AMS from register $38+$ch
                    UINT8 pmsAms = s_shadowYM2151[c][0x38 + ch];
                    int   pms    = (pmsAms >> 4) & 0x07;
                    int   ams    = pmsAms & 0x03;
                    // decode note name from KC (+2 octaves to match piano display)
                    int oct  = (kc >> 4) & 0x07;
                    int semi = kOPMKcToSemi[kc & 0x0F];
                    char noteStr[8];
                    if (keyon && semi >= 0) {
                        int dispOct = oct + 2;
                        snprintf(noteStr, sizeof(noteStr), "%s%d", kOPMNoteName[semi], dispOct);
                    } else if (keyon)
                        snprintf(noteStr, sizeof(noteStr), "KC%02X", kc);
                    else
                        snprintf(noteStr, sizeof(noteStr), "---");
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "CH%d##ym2151ch%d_%d", ch+1, ch, c);
                    ImGuiTreeNodeFlags nodeFlags2151 = ImGuiTreeNodeFlags_DefaultOpen;
                    bool nodeOpen2151 = ImGui::TreeNodeEx(chLabel, nodeFlags2151);
                    if (nodeOpen2151) {
                        // Channel info row in fixed-width table — no jitter
                        ImVec4 infoCol2151 = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        const char* statusText2151;
                        ImU32 statusCol2151;
                        if (keyon && !keyoff) { statusText2151 = "ON";  statusCol2151 = IM_COL32(80,200,80,255); }
                        else if (keyon || keyoff) { statusText2151 = "REL"; statusCol2151 = IM_COL32(255,200,80,255); }
                        else { statusText2151 = "--"; statusCol2151 = IM_COL32(100,100,100,255); }
                        if (ImGui::BeginTable("##2151info", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("AL",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableSetupColumn("PMS",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableSetupColumn("AMS",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(
                                ImVec4(((statusCol2151>>0)&0xFF)/255.f,((statusCol2151>>8)&0xFF)/255.f,((statusCol2151>>16)&0xFF)/255.f,1),
                                "%s", statusText2151);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol2151, "%s", noteStr);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol2151, "%d", al);
                            ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoCol2151, "%d", fb);
                            ImGui::TableSetColumnIndex(4); ImGui::TextColored(infoCol2151, "%s", pan_str2151[pan]);
                            ImGui::TableSetColumnIndex(5); ImGui::TextColored(infoCol2151, "%d", pms);
                            ImGui::TableSetColumnIndex(6); ImGui::TextColored(infoCol2151, "%d", ams);
                            ImGui::EndTable();
                        }
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##opmtbl", 11, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("D1R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableSetupColumn("D2R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("KS",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("DT",  ImGuiTableColumnFlags_WidthFixed, 38.f);
                                ImGui::TableSetupColumn("AMS", ImGuiTableColumnFlags_WidthFixed, 32.f);
                                ImGui::TableHeadersRow();
                                for (int op = 0; op < 4; op++) {
                                    int s    = kOPMOpOff[op] + ch;
                                    UINT8 ks  = (s_shadowYM2151[c][0x80 + s] >> 6) & 0x03;
                                    UINT8 ar  =  s_shadowYM2151[c][0x80 + s] & 0x1F;
                                    UINT8 ams =  (s_shadowYM2151[c][0xA0 + s] >> 7) & 0x01;
                                    UINT8 d1r =  s_shadowYM2151[c][0xA0 + s] & 0x1F;
                                    UINT8 dt2 =  (s_shadowYM2151[c][0xC0 + s] >> 6) & 0x03;
                                    UINT8 d2r =  s_shadowYM2151[c][0xC0 + s] & 0x1F;
                                    UINT8 rr  =  s_shadowYM2151[c][0xE0 + s] & 0x0F;
                                    UINT8 sl  =  (s_shadowYM2151[c][0xE0 + s] >> 4) & 0x0F;
                                    UINT8 tl  =  s_shadowYM2151[c][0x60 + s] & 0x7F;
                                    UINT8 dt1 =  (s_shadowYM2151[c][0x40 + s] >> 4) & 0x07;
                                    UINT8 ml  =  s_shadowYM2151[c][0x40 + s] & 0x0F;
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);  ImGui::Text("%s", kOPMOpName[op]);
                                    ImGui::TableSetColumnIndex(1);  ImGui::Text("%d", ar);
                                    ImGui::TableSetColumnIndex(2);  ImGui::Text("%d", d1r);
                                    ImGui::TableSetColumnIndex(3);  ImGui::Text("%d", d2r);
                                    ImGui::TableSetColumnIndex(4);  ImGui::Text("%d", rr);
                                    ImGui::TableSetColumnIndex(5);  ImGui::Text("%d", sl);
                                    ImGui::TableSetColumnIndex(6);  ImGui::Text("%d", tl);
                                    ImGui::TableSetColumnIndex(7);  ImGui::Text("%d", ks);
                                    ImGui::TableSetColumnIndex(8);  ImGui::Text("%d", ml);
                                    ImGui::TableSetColumnIndex(9);  ImGui::Text("%d/%d", dt1, dt2);
                                    ImGui::TableSetColumnIndex(10); ImGui::Text("%d", ams);
                                }
                                ImGui::EndTable();
                            }
                        ImGui::TreePop();
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##ym2151")) {
                    RenderRegGrid("ym2151regs", s_shadowYM2151[c], 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YM2413: {
                int c = dev.instance & 1;
                static const char* kOPLLPatch[16] = {
                    "Custom","Bell","Guitar","Piano","Flute",
                    "Clarinet","Oboe","Trumpet","Organ","Horn",
                    "Synth","Harp","Vibraphone","SynBass","AcBass","ElecBass"
                };
                static const char* kOPLLNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                // OPLL built-in patch ROM: [patch][op][TL,FB,AR,DR,SL,RR,KL,ML,AM,VIB]
                // patch 0 = custom (from regs), patches 1-15 from YM2413 datasheet ROM
                // layout per op: {TL,FB,AR,DR,SL,RR,KL,ML,AM,VIB}
                static const UINT8 kOPLLRom[16][2][10] = {
                    // Custom (placeholder, will be filled from regs)
                    {{0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0}},
                    // 1 Bell
                    {{0,5,15,7, 4,0,0,13,0,0},{0,0,15,8, 5,6,0,1,0,0}},
                    // 2 Guitar
                    {{3,5,12,8, 4,4,0, 1,0,0},{0,0,15,8, 4,4,0,1,0,0}},
                    // 3 Piano
                    {{0,5,15,9, 6,7,0, 1,0,0},{0,0,15,8, 5,6,0,1,0,0}},
                    // 4 Flute
                    {{1,5,15,8, 4,0,0, 9,1,1},{0,0,13,6, 3,0,0,2,0,0}},
                    // 5 Clarinet
                    {{3,5, 9,8, 6,4,0, 1,0,0},{0,0,15,8, 4,4,0,1,0,0}},
                    // 6 Oboe
                    {{0,5,15,7, 5,6,0, 1,0,0},{0,0,14,8, 5,6,0,1,0,0}},
                    // 7 Trumpet
                    {{0,5,15,8, 5,6,0, 1,0,0},{0,0,15,8, 5,6,0,1,0,0}},
                    // 8 Organ
                    {{0,5,15,9, 5,0,0, 1,0,0},{0,0,15,8, 5,0,0,1,0,0}},
                    // 9 Horn
                    {{0,3,12,6, 7,7,0, 1,0,0},{0,0,15,8, 5,6,0,1,0,0}},
                    // 10 Synth
                    {{0,5,15,7, 5,6,0,13,0,0},{0,0,15,7, 4,7,1,1,0,0}},
                    // 11 Harp
                    {{0,0,15,9, 8,7,0, 9,0,0},{0,0,12,6, 6,6,0,1,0,0}},
                    // 12 Vibraphone
                    {{0,0,15,4, 4,0,0, 9,1,0},{0,0,15,8, 5,0,0,2,0,1}},
                    // 13 SynBass
                    {{0,0,12,6, 7,5,0, 9,0,0},{0,0,15,8, 4,6,0,1,0,0}},
                    // 14 AcBass
                    {{0,0,15,8, 8,6,0, 9,0,0},{0,0,15,9, 7,6,0,1,0,0}},
                    // 15 ElecBass
                    {{0,0,15,7, 6,5,0, 9,0,0},{0,0,15,8, 4,5,0,1,0,0}},
                };
                UINT8 rhythmReg = s_shadowYM2413[c][0x0E];
                bool  rhythmMode = (rhythmReg >> 5) & 1;
                ImGui::TextDisabled("-- FM --");
                for (int ch = 0; ch < 9; ch++) {
                    UINT8 lo   = s_shadowYM2413[c][0x10 + ch];
                    UINT8 hi   = s_shadowYM2413[c][0x20 + ch];
                    UINT8 inst = s_shadowYM2413[c][0x30 + ch];
                    int fnum   = ((hi & 0x01) << 8) | lo;
                    int block  = (hi >> 1) & 0x07;
                    int patch  = (inst >> 4) & 0x0F;
                    int vol    = inst & 0x0F;
                    bool keyon = ((hi >> 5) & 1) || (fnum > 0 && vol < 15);
                    int nt2413 = (keyon && fnum > 0) ? FrequencyToMIDINote(
                        (float)fnum * 49716.0f / (float)(1 << (19 - block))) : -1;
                    bool isRhCh = rhythmMode && ch >= 6;
                    char chLabel[32];
                    if (isRhCh) {
                        static const char* kRhChName[3] = {"BD","HH+SD","TOM+CYM"};
                        snprintf(chLabel, sizeof(chLabel), "CH%d [%s]##ym2413ch%d",
                            ch+1, kRhChName[ch-6], ch);
                    } else {
                        snprintf(chLabel, sizeof(chLabel), "CH%d##ym2413ch%d", ch+1, ch);
                    }
                    ImGuiTreeNodeFlags nodeFlags2413 = ImGuiTreeNodeFlags_DefaultOpen;
                    bool nodeOpen2413 = ImGui::TreeNodeEx(chLabel, nodeFlags2413);
                    if (nodeOpen2413) {
                        if (!isRhCh) {
                            // Channel info row in fixed-width table
                            char noteBuf2413[8];
                            if (keyon && nt2413 >= 0)
                                snprintf(noteBuf2413, sizeof(noteBuf2413), "%s%d", kOPLLNote[nt2413%12], nt2413/12-1);
                            else if (keyon)
                                snprintf(noteBuf2413, sizeof(noteBuf2413), "F%03X/%d", fnum, block);
                            else
                                snprintf(noteBuf2413, sizeof(noteBuf2413), "---");
                            ImVec4 infoCol2413 = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                            if (ImGui::BeginTable("##ym2413info", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                                ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthFixed, 68.f);
                                ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableHeadersRow();
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::TextColored(infoCol2413, "%s", noteBuf2413);
                                ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol2413, "%s", kOPLLPatch[patch]);
                                ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol2413, "%d", vol);
                                ImGui::EndTable();
                            }
                            ImGui::Spacing();
                        }
                        // Resolve patch params: custom from regs, built-in from ROM
                        UINT8 pTL[2], pFB[2], pAR[2], pDR[2], pSL[2], pRR[2], pKL[2], pML[2], pAM[2], pVIB[2];
                        if (patch == 0) {
                            UINT8 r0=s_shadowYM2413[c][0x00],r1=s_shadowYM2413[c][0x01];
                            UINT8 r2=s_shadowYM2413[c][0x02],r3=s_shadowYM2413[c][0x03];
                            UINT8 r4=s_shadowYM2413[c][0x04],r5=s_shadowYM2413[c][0x05];
                            UINT8 r6=s_shadowYM2413[c][0x06],r7=s_shadowYM2413[c][0x07];
                            pAM[0]=(r0>>7)&1; pVIB[0]=(r0>>6)&1; pML[0]=r0&0xF; pKL[0]=(r2>>6)&3;
                            pAM[1]=(r1>>7)&1; pVIB[1]=(r1>>6)&1; pML[1]=r1&0xF; pKL[1]=(r3>>6)&3;
                            pTL[0]=r2&0x3F; pFB[0]=(r3>>1)&7;
                            pTL[1]=0;       pFB[1]=0;
                            pAR[0]=r4>>4; pDR[0]=r4&0xF; pAR[1]=r5>>4; pDR[1]=r5&0xF;
                            pSL[0]=r6>>4; pRR[0]=r6&0xF; pSL[1]=r7>>4; pRR[1]=r7&0xF;
                        } else {
                            for (int op=0;op<2;op++) {
                                pTL[op]=kOPLLRom[patch][op][0]; pFB[op]=kOPLLRom[patch][op][1];
                                pAR[op]=kOPLLRom[patch][op][2]; pDR[op]=kOPLLRom[patch][op][3];
                                pSL[op]=kOPLLRom[patch][op][4]; pRR[op]=kOPLLRom[patch][op][5];
                                pKL[op]=kOPLLRom[patch][op][6]; pML[op]=kOPLLRom[patch][op][7];
                                pAM[op]=kOPLLRom[patch][op][8]; pVIB[op]=kOPLLRom[patch][op][9];
                            }
                        }
                        if (ImGui::BeginTable("##oplltbl", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("OP",     ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("TL",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("FB",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("AR",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("DR",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("SL",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("RR",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("KL",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("ML",     ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("AM/VIB", ImGuiTableColumnFlags_WidthFixed, 46.f);
                            ImGui::TableHeadersRow();
                            static const char* opName2413[2] = {"MOD","CAR"};
                            for (int op = 0; op < 2; op++) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", opName2413[op]);
                                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", pTL[op]);
                                ImGui::TableSetColumnIndex(2); ImGui::Text("%d", pFB[op]);
                                ImGui::TableSetColumnIndex(3); ImGui::Text("%d", pAR[op]);
                                ImGui::TableSetColumnIndex(4); ImGui::Text("%d", pDR[op]);
                                ImGui::TableSetColumnIndex(5); ImGui::Text("%d", pSL[op]);
                                ImGui::TableSetColumnIndex(6); ImGui::Text("%d", pRR[op]);
                                ImGui::TableSetColumnIndex(7); ImGui::Text("%d", pKL[op]);
                                ImGui::TableSetColumnIndex(8); ImGui::Text("%d", pML[op]);
                                ImGui::TableSetColumnIndex(9); ImGui::Text("%d/%d", pAM[op], pVIB[op]);
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }
                }
                if (rhythmMode) {
                    // Rhythm channels: BD/HH/SD/TOM/CYM
                    // reg 0x0E bits: 4=BD,3=SD,2=TOM,1=CYM,0=HH
                    // vol: reg 0x36 bits7:4=BD, reg 0x37 bits7:4=HH bits3:0=SD
                    //      reg 0x38 bits7:4=TOM bits3:0=CYM
                    static const char* kOPLLRhName[5] = {"BD","HH","SD","TM","CY"};
                    static const int   kOPLLRhBit[5]  = {4, 0, 3, 2, 1};
                    ImGui::Spacing();
                    ImGui::TextDisabled("-- Rhythm --");
                    UINT8 bdVol  = (s_shadowYM2413[c][0x36] >> 4) & 0x0F;
                    UINT8 hhVol  = (s_shadowYM2413[c][0x37] >> 4) & 0x0F;
                    UINT8 sdVol  =  s_shadowYM2413[c][0x37] & 0x0F;
                    UINT8 tomVol = (s_shadowYM2413[c][0x38] >> 4) & 0x0F;
                    UINT8 cymVol  =  s_shadowYM2413[c][0x38] & 0x0F;
                    UINT8 rhVols[5] = {bdVol, hhVol, sdVol, tomVol, cymVol};
                    if (ImGui::BeginTable("##ym2413rhythm", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Inst", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < 5; i++) {
                            bool kon = (rhythmReg >> kOPLLRhBit[i]) & 1;
                            ImVec4 col = kon ? ImVec4(1.0f,0.8f,0.4f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "%s", kOPLLRhName[i]);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", kon ? "[ON]" : "[--]");
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%d", rhVols[i]);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##ym2413")) {
                    RenderRegGrid("ym2413regs", s_shadowYM2413[c], 8);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YM2203: {
                int c = dev.instance & 1;
                // OPN slot offsets: M1/M2/C1/C2
                static const int   kOPNSlotOff[4]  = {0, 8, 4, 12};
                static const char* kOPNOpName[4]   = {"M1","M2","C1","C2"};
                static const char* kOPNNote[12]     = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 b4   = s_shadowYM2203[c][0xA4 + ch];
                    UINT8 b0   = s_shadowYM2203[c][0xA0 + ch];
                    int   fnum = ((b4 & 0x07) << 8) | b0;
                    int   blk  = (b4 >> 3) & 0x07;
                    bool  kon  = s_ym2203KeyOn[c][ch];
                    UINT8 alFb = s_shadowYM2203[c][0xB0 + ch];
                    int   al   = alFb & 0x07;
                    int   fb   = (alFb >> 3) & 0x07;
                    int   nt2203 = (kon && fnum > 0) ? FrequencyToMIDINote(
                        (float)fnum * 7670454.0f / (float)(144 * (1 << (21 - blk)))) : -1;
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "FM%d##ym2203ch%d_%d", ch+1, ch, c);
                    ImGuiTreeNodeFlags nodeFlags2203 = ImGuiTreeNodeFlags_DefaultOpen;
                    bool nodeOpen2203 = ImGui::TreeNodeEx(chLabel, nodeFlags2203);
                    if (nodeOpen2203) {
                        // Channel info row in fixed-width table — no jitter
                        char noteBuf2203[8];
                        if (kon && nt2203 >= 0)
                            snprintf(noteBuf2203, sizeof(noteBuf2203), "%s%d", kOPNNote[nt2203%12], nt2203/12-1);
                        else if (kon)
                            snprintf(noteBuf2203, sizeof(noteBuf2203), "F%03X/%d", fnum, blk);
                        else
                            snprintf(noteBuf2203, sizeof(noteBuf2203), "---");
                        ImVec4 infoCol2203 = kon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        if (ImGui::BeginTable("##2203info", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("AL",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(infoCol2203, "%s", noteBuf2203);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol2203, "%d", al);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol2203, "%d", fb);
                            ImGui::EndTable();
                        }
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##opntbl2203", 11, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("D1R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                            ImGui::TableSetupColumn("D2R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                            ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("KS",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("DT",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("AMS", ImGuiTableColumnFlags_WidthFixed, 30.f);
                            ImGui::TableHeadersRow();
                            for (int op = 0; op < 4; op++) {
                                int s    = kOPNSlotOff[op] + ch;
                                UINT8 ks  = (s_shadowYM2203[c][0x50 + s] >> 6) & 0x03;
                                UINT8 ar  =  s_shadowYM2203[c][0x50 + s] & 0x1F;
                                UINT8 ams =  (s_shadowYM2203[c][0x60 + s] >> 7) & 0x01;
                                UINT8 d1r =  s_shadowYM2203[c][0x60 + s] & 0x1F;
                                UINT8 d2r =  s_shadowYM2203[c][0x70 + s] & 0x1F;
                                UINT8 rr  =  s_shadowYM2203[c][0x80 + s] & 0x0F;
                                UINT8 sl  =  (s_shadowYM2203[c][0x80 + s] >> 4) & 0x0F;
                                UINT8 tl  =  s_shadowYM2203[c][0x40 + s] & 0x7F;
                                UINT8 dt  =  (s_shadowYM2203[c][0x30 + s] >> 4) & 0x07;
                                UINT8 ml  =  s_shadowYM2203[c][0x30 + s] & 0x0F;
                                ImVec4 rowCol = kon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);  ImGui::TextColored(rowCol, "%s", kOPNOpName[op]);
                                ImGui::TableSetColumnIndex(1);  ImGui::TextColored(rowCol, "%d", ar);
                                ImGui::TableSetColumnIndex(2);  ImGui::TextColored(rowCol, "%d", d1r);
                                ImGui::TableSetColumnIndex(3);  ImGui::TextColored(rowCol, "%d", d2r);
                                ImGui::TableSetColumnIndex(4);  ImGui::TextColored(rowCol, "%d", rr);
                                ImGui::TableSetColumnIndex(5);  ImGui::TextColored(rowCol, "%d", sl);
                                ImGui::TableSetColumnIndex(6);  ImGui::TextColored(rowCol, "%d", tl);
                                ImGui::TableSetColumnIndex(7);  ImGui::TextColored(rowCol, "%d", ks);
                                ImGui::TableSetColumnIndex(8);  ImGui::TextColored(rowCol, "%d", ml);
                                ImGui::TableSetColumnIndex(9);  ImGui::TextColored(rowCol, "%d", dt);
                                ImGui::TableSetColumnIndex(10); ImGui::TextColored(rowCol, "%d", ams);
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::Spacing();
                ImGui::TextDisabled("-- SSG --");
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 lo  = s_shadowYM2203[c][ch * 2];
                    UINT8 hi  = s_shadowYM2203[c][ch * 2 + 1] & 0x0F;
                    UINT16 period = ((UINT16)hi << 8) | lo;
                    UINT8 mix = s_shadowYM2203[c][0x07];
                    bool tone_on  = !((mix >> ch) & 1);
                    bool noise_on = !((mix >> (ch + 3)) & 1);
                    UINT8 vol = s_shadowYM2203[c][0x08 + ch] & 0x1F;
                    ImGui::Text("SSG%d Per=%04X Vol=%02X %s%s",
                        ch+1, period, vol,
                        tone_on  ? "T" : "-",
                        noise_on ? "N" : "-");
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##ym2203")) {
                    RenderRegGrid("ym2203regs", s_shadowYM2203[c], 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YM2608:
            case DEVID_YM2610: {
                int c = dev.instance & 1;
                const UINT8 (*regs)[0x100] = (dev.type == DEVID_YM2608)
                    ? s_shadowYM2608[c] : s_shadowYM2610[c];
                const char* suffix = (dev.type == DEVID_YM2608) ? "2608" : "2610";
                bool is2610 = (dev.type == DEVID_YM2610);
                // YM2608: 6 FM channels; YM2610: 4 FM channels (ch1-3 + ch4 only)
                int fmChCount = is2610 ? 4 : 6;
                const char* pan_str[] = {"--"," R"," L","LR"};
                static const int   kOPN2SlotOff2[4]  = {0, 8, 4, 12};
                static const char* kOPN2OpName2[4]   = {"M1","M2","C1","C2"};
                static const char* kOPN2Note26xx[12]  = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                ImGui::TextDisabled("-- FM --");
                for (int ch = 0; ch < fmChCount; ch++) {
                    int port = (ch < 3) ? 0 : 1;
                    int off  = ch % 3;
                    UINT8 b4  = regs[port][0xA4 + off];
                    UINT8 b0  = regs[port][0xA0 + off];
                    UINT8 b4b = regs[port][0xB4 + off];
                    bool keyon = is2610 ? s_ym2610KeyOn[c][ch] : s_ym2608KeyOn[c][ch];
                    int fnum  = ((b4 & 0x07) << 8) | b0;
                    int block = (b4 >> 3) & 0x07;
                    int pan   = (b4b >> 6) & 0x03;
                    int al    = regs[port][0xB0 + off] & 0x07;
                    int fb    = (regs[port][0xB0 + off] >> 3) & 0x07;
                    int nt26xx = (keyon && fnum > 0) ? FrequencyToMIDINote(
                        (float)fnum * 7670454.0f / (float)(144 * (1 << (21 - block)))) : -1;
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "FM%d##%sch%d_%d", ch+1, suffix, ch, c);
                    ImGuiTreeNodeFlags nodeFlags26xx = ImGuiTreeNodeFlags_DefaultOpen;
                    bool nodeOpen26xx = ImGui::TreeNodeEx(chLabel, nodeFlags26xx);
                    if (nodeOpen26xx) {
                        // Channel info row in fixed-width table — no jitter
                        char noteBuf26xx[8];
                        if (keyon && nt26xx >= 0)
                            snprintf(noteBuf26xx, sizeof(noteBuf26xx), "%s%d", kOPN2Note26xx[nt26xx%12], nt26xx/12-1);
                        else if (keyon)
                            snprintf(noteBuf26xx, sizeof(noteBuf26xx), "F%03X/%d", fnum, block);
                        else
                            snprintf(noteBuf26xx, sizeof(noteBuf26xx), "---");
                        ImVec4 infoCol26xx = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        if (ImGui::BeginTable("##26xxinfo", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("AL",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(infoCol26xx, "%s", noteBuf26xx);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol26xx, "%d", al);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol26xx, "%d", fb);
                            ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoCol26xx, "%s", pan_str[pan]);
                            ImGui::EndTable();
                        }
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##opntbl26xx", 11, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("D1R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableSetupColumn("D2R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("KS",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("DT",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("AMS", ImGuiTableColumnFlags_WidthFixed, 32.f);
                                ImGui::TableHeadersRow();
                                for (int op = 0; op < 4; op++) {
                                    int s    = kOPN2SlotOff2[op] + off;
                                    UINT8 ks  = (regs[port][0x50 + s] >> 6) & 0x03;
                                    UINT8 ar  =  regs[port][0x50 + s] & 0x1F;
                                    UINT8 ams =  (regs[port][0x60 + s] >> 7) & 0x01;
                                    UINT8 d1r =  regs[port][0x60 + s] & 0x1F;
                                    UINT8 d2r =  regs[port][0x70 + s] & 0x1F;
                                    UINT8 rr  =  regs[port][0x80 + s] & 0x0F;
                                    UINT8 sl  =  (regs[port][0x80 + s] >> 4) & 0x0F;
                                    UINT8 tl  =  regs[port][0x40 + s] & 0x7F;
                                    UINT8 dt  =  (regs[port][0x30 + s] >> 4) & 0x07;
                                    UINT8 ml  =  regs[port][0x30 + s] & 0x0F;
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);  ImGui::Text("%s", kOPN2OpName2[op]);
                                    ImGui::TableSetColumnIndex(1);  ImGui::Text("%d", ar);
                                    ImGui::TableSetColumnIndex(2);  ImGui::Text("%d", d1r);
                                    ImGui::TableSetColumnIndex(3);  ImGui::Text("%d", d2r);
                                    ImGui::TableSetColumnIndex(4);  ImGui::Text("%d", rr);
                                    ImGui::TableSetColumnIndex(5);  ImGui::Text("%d", sl);
                                    ImGui::TableSetColumnIndex(6);  ImGui::Text("%d", tl);
                                    ImGui::TableSetColumnIndex(7);  ImGui::Text("%d", ks);
                                    ImGui::TableSetColumnIndex(8);  ImGui::Text("%d", ml);
                                    ImGui::TableSetColumnIndex(9);  ImGui::Text("%d", dt);
                                    ImGui::TableSetColumnIndex(10); ImGui::Text("%d", ams);
                                }
                                ImGui::EndTable();
                            }
                        ImGui::TreePop();
                    }
                }
                // SSG (AY8910 embedded) - regs stored in chip's own shadow port0 (0x00-0x0F)
                ImGui::Spacing();
                ImGui::TextDisabled("-- SSG (AY8910) --");
                for (int ch = 0; ch < 3; ch++) {
                    UINT8 lo  = regs[0][ch * 2];
                    UINT8 hi  = regs[0][ch * 2 + 1] & 0x0F;
                    UINT16 period = ((UINT16)hi << 8) | lo;
                    UINT8 mix = regs[0][0x07];
                    bool tone_on  = !((mix >> ch) & 1);
                    bool noise_on = !((mix >> (ch + 3)) & 1);
                    UINT8 vol = regs[0][0x08 + ch] & 0x0F;
                    ImGui::Text("SSG%d Per=%04X Vol=%X %s%s",
                        ch+1, period, vol,
                        tone_on  ? "T" : "-",
                        noise_on ? "N" : "-");
                }
                // Rhythm (ADPCM-A, 6 fixed percussion channels)
                ImGui::Spacing();
                ImGui::TextDisabled("-- Rhythm --");
                {
                    static const char* kRhyName[6] = {"BD","SD","CY","HH","TM","RM"};
                    static const char* adpcmA_pan[] = {"--"," R"," L","LR"};
                    int inst = dev.instance & 1;
                    // YM2608: ADPCM-A regs in port0 at 0x10-0x3f; YM2610: port1 at 0x00-0x2f
                    int rp = is2610 ? 1 : 0;
                    int rb = is2610 ? 0x00 : 0x10;  // base offset
                    UINT8 masterVol = regs[rp][rb + 0x01] & 0x3F;
                    if (ImGui::BeginTable("##26xxrhythm", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Inst", ImGuiTableColumnFlags_WidthFixed, 28.f);
                        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                        ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 52.f);
                        ImGui::TableHeadersRow();
                        for (int ch = 0; ch < 6; ch++) {
                            bool  kon  = is2610 ? s_ym2610RhyKeyOn[inst][ch] : s_ym2608RhyKeyOn[inst][ch];
                            UINT8 panv = regs[rp][rb + 0x08 + ch];
                            UINT8 vol  = panv & 0x1F;
                            UINT8 pan  = (panv >> 6) & 0x03;
                            UINT32 startA = ((UINT32)regs[rp][rb + 0x18 + ch] << 8) | regs[rp][rb + 0x10 + ch];
                            UINT32 stopA  = ((UINT32)regs[rp][rb + 0x28 + ch] << 8) | regs[rp][rb + 0x20 + ch];
                            ImVec4 colA = kon ? ImVec4(1.0f,0.7f,0.3f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                            char addrBuf[16];
                            snprintf(addrBuf, sizeof(addrBuf), "%04X-%04X", startA, stopA);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(colA, "%s", kRhyName[ch]);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(colA, "%s", kon ? "[ON]" : "[--]");
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(colA, "%d", vol);
                            ImGui::TableSetColumnIndex(3); ImGui::TextColored(colA, "%s", adpcmA_pan[pan]);
                            ImGui::TableSetColumnIndex(4); ImGui::TextColored(colA, "%s", addrBuf);
                        }
                        ImGui::EndTable();
                    }
                }
                // ADPCM-B (1 channel)
                ImGui::Spacing();
                ImGui::TextDisabled("-- ADPCM-B --");
                {
                    UINT8 ctrl  = regs[1][0x00];
                    bool  bkon  = (ctrl & 0x80) != 0;
                    UINT8 lvl   = regs[1][0x0B] & 0xFF;
                    UINT32 startB = ((UINT32)regs[1][0x04] << 8) | regs[1][0x03];
                    UINT32 stopB  = ((UINT32)regs[1][0x06] << 8) | regs[1][0x05];
                    UINT16 delta  = ((UINT16)regs[1][0x10] << 8) | regs[1][0x09];
                    ImVec4 colB = bkon ? ImVec4(0.4f,1.0f,0.6f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                    if (ImGui::BeginTable("##26xxadpcmb", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 28.f);
                        ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 52.f);
                        ImGui::TableSetupColumn("Delt", ImGuiTableColumnFlags_WidthFixed, 36.f);
                        ImGui::TableHeadersRow();
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(colB, "%s", bkon ? "[ON]" : "[--]");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(colB, "%d", lvl);
                        char addrBuf[16];
                        snprintf(addrBuf, sizeof(addrBuf), "%04X-%04X", startB, stopB);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(colB, "%s", addrBuf);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(colB, "%04X", delta);
                        ImGui::EndTable();
                    }
                }
                ImGui::Spacing();
                char treeId[32];
                snprintf(treeId, sizeof(treeId), "Port 0##%s", suffix);
                if (ImGui::TreeNode(treeId)) {
                    char tblId[32]; snprintf(tblId, sizeof(tblId), "tbl_%s_p0", suffix);
                    RenderRegGrid(tblId, regs[0], 16);
                    ImGui::TreePop();
                }
                snprintf(treeId, sizeof(treeId), "Port 1##%s", suffix);
                if (ImGui::TreeNode(treeId)) {
                    char tblId[32]; snprintf(tblId, sizeof(tblId), "tbl_%s_p1", suffix);
                    RenderRegGrid(tblId, regs[1], 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YMF262: {
                int c = dev.instance & 1;
                static const int   kOPL3SlotM[9] = {0,1,2,8,9,10,16,17,18};
                static const int   kOPL3SlotC[9] = {3,4,5,11,12,13,19,20,21};
                auto opl3SlotOff = [](int slot) { return (slot%6) + 8*(slot/6); };
                static const char* pan_str4[]    = {"--"," R"," L","LR"};
                static const char* kOPL3Note[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                // Read 4-op enable register (port 1, 0x04)
                UINT8 fouren = s_shadowYMF262[c][1][0x04];
                // 4-op pairing bits:
                // bit 0: ch0+ch3 (p2=0), bit 1: ch1+ch4 (p2=0), bit 2: ch2+ch5 (p2=0)
                // bit 3: ch9+ch12 (p2=1), bit 4: ch10+ch13 (p2=1), bit 5: ch11+ch14 (p2=1)
                for (int p = 0; p < 2; p++) {
                    const UINT8* r = s_shadowYMF262[c][p];
                    for (int ch = 0; ch < 9; ch++) {
                        int chIdx  = p*9 + ch;
                        // Check if this is a 4-op high channel (attached)
                        int pair_bit = (ch < 3) ? (p*3 + ch) : -1;
                        bool is4op_low  = (pair_bit >= 0) && ((fouren >> pair_bit) & 1);
                        bool is4op_high = (ch >= 3 && ch <= 5) && ((fouren >> (p*3 + ch-3)) & 1);
                        // Skip high channels - they'll be rendered as part of low channel
                        if (is4op_high)
                            continue;
                        UINT8 lo   = r[0xA0 + ch];
                        UINT8 hi   = r[0xB0 + ch];
                        UINT8 c0   = r[0xC0 + ch];
                        int fnum   = ((hi & 0x03) << 8) | lo;
                        int block  = (hi >> 2) & 0x07;
                        bool keyon = s_ymf262KeyOn[c][chIdx];
                        bool keyoff = s_ymf262KeyOff[c][chIdx];
                        int pan    = (c0 >> 4) & 0x03;
                        int fb     = (c0 >> 1) & 0x07;
                        int cnt    = c0 & 0x01;
                        int smOff  = opl3SlotOff(kOPL3SlotM[ch]);
                        int scOff  = opl3SlotOff(kOPL3SlotC[ch]);
                        int nt3 = (keyon && fnum > 0) ? FrequencyToMIDINote(
                            49716.0f * (float)fnum / (float)(1 << (20 - block))) : -1;
                        char chLabel[64];
                        if (is4op_low) {
                            // 4-op mode: show both channels in label
                            int ch_hi = ch + 3;
                            int chIdx_hi = p*9 + ch_hi;
                            snprintf(chLabel, sizeof(chLabel), "CH%d+%d (4OP)##opl3ch%d_%d", chIdx+1, chIdx_hi+1, chIdx, c);
                        } else {
                            snprintf(chLabel, sizeof(chLabel), "CH%d##opl3ch%d_%d", chIdx+1, chIdx, c);
                        }
                        ImGuiTreeNodeFlags opl3flags = ImGuiTreeNodeFlags_DefaultOpen;
                        bool opl3open = ImGui::TreeNodeEx(chLabel, opl3flags);
                        if (opl3open) {
                            // Channel info row in fixed-width table
                            char noteBufOpl3[8];
                            if (keyon && nt3 >= 0)
                                snprintf(noteBufOpl3, sizeof(noteBufOpl3), "%s%d", kOPL3Note[nt3%12], nt3/12-1);
                            else if (keyon)
                                snprintf(noteBufOpl3, sizeof(noteBufOpl3), "F%03X/%d", fnum, block);
                            else
                                snprintf(noteBufOpl3, sizeof(noteBufOpl3), "---");
                            // Status text and color
                            const char* statusText;
                            ImU32 statusCol;
                            if (keyon && !keyoff) { statusText = "ON";  statusCol = IM_COL32(80,200,80,255); }
                            else if (keyon && keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                            else if (keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                            else { statusText = "--"; statusCol = IM_COL32(100,100,100,255); }
                            ImVec4 infoColOpl3 = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                            if (ImGui::BeginTable("##opl3info", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                                ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("CN",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                                ImGui::TableHeadersRow();
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::TextColored(
                                    ImVec4(((statusCol>>0)&0xFF)/255.f,((statusCol>>8)&0xFF)/255.f,((statusCol>>16)&0xFF)/255.f,1),
                                    "%s", statusText);
                                ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoColOpl3, "%s", noteBufOpl3);
                                ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoColOpl3, "%d", fb);
                                ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoColOpl3, "%d", cnt);
                                ImGui::TableSetColumnIndex(4); ImGui::TextColored(infoColOpl3, "%s", pan_str4[pan]);
                                ImGui::EndTable();
                            }
                            ImGui::Spacing();
                            if (ImGui::BeginTable("##opl3ops", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                    ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                    ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("DR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("KL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("AM",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                    ImGui::TableSetupColumn("VIB", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                    ImGui::TableHeadersRow();
                                    // Low channel operators
                                    for (int oi = 0; oi < 2; oi++) {
                                        int sOff = oi == 0 ? smOff : scOff;
                                        UINT8 reg20 = r[0x20 + sOff];
                                        UINT8 reg40 = r[0x40 + sOff];
                                        UINT8 reg60 = r[0x60 + sOff];
                                        UINT8 reg80 = r[0x80 + sOff];
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0); ImGui::Text("%s", oi==0?"MOD":"CAR");
                                        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", reg60>>4);
                                        ImGui::TableSetColumnIndex(2); ImGui::Text("%d", reg60&0xF);
                                        ImGui::TableSetColumnIndex(3); ImGui::Text("%d", reg80&0xF);
                                        ImGui::TableSetColumnIndex(4); ImGui::Text("%d", reg80>>4);
                                        ImGui::TableSetColumnIndex(5); ImGui::Text("%d", reg40&0x3F);
                                        ImGui::TableSetColumnIndex(6); ImGui::Text("%d", reg40>>6);
                                        ImGui::TableSetColumnIndex(7); ImGui::Text("%d", reg20&0xF);
                                        ImGui::TableSetColumnIndex(8); ImGui::Text("%d", (reg20>>7)&1);
                                        ImGui::TableSetColumnIndex(9); ImGui::Text("%d", (reg20>>6)&1);
                                    }
                                    // High channel operators (only in 4-op mode)
                                    if (is4op_low) {
                                        int ch_hi = ch + 3;
                                        int smOff_hi = opl3SlotOff(kOPL3SlotM[ch_hi]);
                                        int scOff_hi = opl3SlotOff(kOPL3SlotC[ch_hi]);
                                        UINT8 c0_hi = r[0xC0 + ch_hi];
                                        int fb_hi = (c0_hi >> 1) & 0x07;
                                        int cnt_hi = c0_hi & 0x01;
                                        for (int oi = 0; oi < 2; oi++) {
                                            int sOff = oi == 0 ? smOff_hi : scOff_hi;
                                            UINT8 reg20 = r[0x20 + sOff];
                                            UINT8 reg40 = r[0x40 + sOff];
                                            UINT8 reg60 = r[0x60 + sOff];
                                            UINT8 reg80 = r[0x80 + sOff];
                                            ImGui::TableNextRow();
                                            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", oi==0?"MOD2":"CAR2");
                                            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", reg60>>4);
                                            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", reg60&0xF);
                                            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", reg80&0xF);
                                            ImGui::TableSetColumnIndex(4); ImGui::Text("%d", reg80>>4);
                                            ImGui::TableSetColumnIndex(5); ImGui::Text("%d", reg40&0x3F);
                                            ImGui::TableSetColumnIndex(6); ImGui::Text("%d", reg40>>6);
                                            ImGui::TableSetColumnIndex(7); ImGui::Text("%d", reg20&0xF);
                                            ImGui::TableSetColumnIndex(8); ImGui::Text("%d", (reg20>>7)&1);
                                            ImGui::TableSetColumnIndex(9); ImGui::Text("%d", (reg20>>6)&1);
                                        }
                                        // Show 4-op connection mode
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("4OP");
                                        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("FB=%d", fb_hi);
                                        ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("CNT=%d", cnt_hi);
                                    }
                                    ImGui::EndTable();
                                }
                            ImGui::TreePop();
                        }
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("Port 0##opl3")) {
                    RenderRegGrid("opl3p0", s_shadowYMF262[c][0], 16);
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Port 1##opl3")) {
                    RenderRegGrid("opl3p1", s_shadowYMF262[c][1], 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YM3812:
            case DEVID_YM3526: {
                int c = dev.instance & 1;
                const UINT8* r = (dev.type == DEVID_YM3812) ? s_shadowYM3812[c] : s_shadowYM3526[c];
                const char* suffix2 = (dev.type == DEVID_YM3812) ? "ym3812" : "ym3526";
                const bool* keyOnArr  = (dev.type == DEVID_YM3812) ? s_ym3812KeyOn[c] : s_ym3526KeyOn[c];
                static const int   kOPL2SlotM2[9] = {0,1,2,8,9,10,16,17,18};
                static const int   kOPL2SlotC2[9] = {3,4,5,11,12,13,19,20,21};
                auto opl2SlotOff = [](int slot) { return (slot%6) + 8*(slot/6); };
                static const char* kOPL2Note[12]  = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                UINT8 rhythm    = r[0xBD];
                bool rhythmMode = (rhythm >> 5) & 1;
                int maxCh = rhythmMode ? 6 : 9;
                for (int ch = 0; ch < maxCh; ch++) {
                    UINT8 lo   = r[0xA0 + ch];
                    UINT8 hi   = r[0xB0 + ch];
                    int fnum   = ((hi & 0x03) << 8) | lo;
                    int block  = (hi >> 2) & 0x07;
                    bool keyon = keyOnArr[ch];
                    UINT8 fb_cn = r[0xC0 + ch];
                    int fb     = (fb_cn >> 1) & 0x07;
                    int cnt    = fb_cn & 0x01;
                    int smOff  = opl2SlotOff(kOPL2SlotM2[ch]);
                    int scOff  = opl2SlotOff(kOPL2SlotC2[ch]);
                    int nt2opl = (keyon && fnum > 0) ? FrequencyToMIDINote(
                        49716.0f * (float)fnum / (float)(1 << (20 - block))) : -1;
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "CH%d##%sch%d", ch+1, suffix2, ch);
                    ImGuiTreeNodeFlags opl2flags = ImGuiTreeNodeFlags_DefaultOpen;
                    bool opl2open = ImGui::TreeNodeEx(chLabel, opl2flags);
                    if (opl2open) {
                        // Channel info row in fixed-width table
                        char noteBufOpl2[8];
                        if (keyon && nt2opl >= 0)
                            snprintf(noteBufOpl2, sizeof(noteBufOpl2), "%s%d", kOPL2Note[nt2opl%12], nt2opl/12-1);
                        else if (keyon)
                            snprintf(noteBufOpl2, sizeof(noteBufOpl2), "F%03X/%d", fnum, block);
                        else
                            snprintf(noteBufOpl2, sizeof(noteBufOpl2), "---");
                        // Status text and color
                        const bool* keyOffArr = (dev.type == DEVID_YM3812) ? s_ym3812KeyOff[c] : s_ym3526KeyOff[c];
                        bool keyoff = keyOffArr[ch];
                        const char* statusText;
                        ImU32 statusCol;
                        if (keyon && !keyoff) { statusText = "ON";  statusCol = IM_COL32(80,200,80,255); }
                        else if (keyon && keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                        else if (keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                        else { statusText = "--"; statusCol = IM_COL32(100,100,100,255); }
                        ImVec4 infoColOpl2 = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        if (ImGui::BeginTable("##opl2info", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("CN",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(
                                ImVec4(((statusCol>>0)&0xFF)/255.f,((statusCol>>8)&0xFF)/255.f,((statusCol>>16)&0xFF)/255.f,1),
                                "%s", statusText);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoColOpl2, "%s", noteBufOpl2);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoColOpl2, "%d", fb);
                            ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoColOpl2, "%d", cnt);
                            ImGui::EndTable();
                        }
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##opl2ops", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("DR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("KL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("AM",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("VIB", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableHeadersRow();
                                for (int oi = 0; oi < 2; oi++) {
                                    int sOff    = oi == 0 ? smOff : scOff;
                                    UINT8 reg20 = r[0x20 + sOff];
                                    UINT8 reg40 = r[0x40 + sOff];
                                    UINT8 reg60 = r[0x60 + sOff];
                                    UINT8 reg80 = r[0x80 + sOff];
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", oi==0?"MOD":"CAR");
                                    ImGui::TableSetColumnIndex(1); ImGui::Text("%d", reg60>>4);
                                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", reg60&0xF);
                                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", reg80&0xF);
                                    ImGui::TableSetColumnIndex(4); ImGui::Text("%d", reg80>>4);
                                    ImGui::TableSetColumnIndex(5); ImGui::Text("%d", reg40&0x3F);
                                    ImGui::TableSetColumnIndex(6); ImGui::Text("%d", reg40>>6);
                                    ImGui::TableSetColumnIndex(7); ImGui::Text("%d", reg20&0xF);
                                    ImGui::TableSetColumnIndex(8); ImGui::Text("%d", (reg20>>7)&1);
                                    ImGui::TableSetColumnIndex(9); ImGui::Text("%d", (reg20>>6)&1);
                                }
                                ImGui::EndTable();
                            }
                        ImGui::TreePop();
                    }
                }
                if (rhythmMode) {
                    static const char* kRhNames2[5] = {"BD","SD","TM","CY","HH"};
                    static const int   kRhBit2[5]   = {4,3,2,1,0};
                    static const int   kRhSlotTL[5] = {0x10,0x14,0x12,0x15,0x11};
                    ImGui::Spacing();
                    ImGui::TextDisabled("-- Rhythm --");
                    if (ImGui::BeginTable("##ym3812rhythm", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Inst", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableSetupColumn("TL",   ImGuiTableColumnFlags_WidthFixed, 30.f);
                        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < 5; i++) {
                            bool kon = (rhythm >> kRhBit2[i]) & 1;
                            UINT8 tl = r[0x40 + kRhSlotTL[i]] & 0x3F;
                            ImVec4 col2 = kon ? ImVec4(1.0f,0.8f,0.4f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(col2, "%s", kRhNames2[i]);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(col2, "%d", tl);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(col2, "%s", kon?"ON":"--");
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::Spacing();
                char regTreeId[32]; snprintf(regTreeId, sizeof(regTreeId), "All Regs##%s", suffix2);
                char regTblId[32];  snprintf(regTblId,  sizeof(regTblId),  "%sregs", suffix2);
                if (ImGui::TreeNode(regTreeId)) {
                    RenderRegGrid(regTblId, r, 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_AY8910: {
                int c = dev.instance & 1;
                UINT32 ayClk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 1789773;
                static const char* kAYNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                UINT8 mix = s_shadowAY8910[c][0x07];
                if (ImGui::BeginTable("##ay8910tbl", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Note",  ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Per",   ImGuiTableColumnFlags_WidthFixed, 44.f);
                    ImGui::TableSetupColumn("Vol",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Env",   ImGuiTableColumnFlags_WidthFixed, 52.f);
                    ImGui::TableSetupColumn("Tone",  ImGuiTableColumnFlags_WidthFixed, 38.f);
                    ImGui::TableSetupColumn("Noise", ImGuiTableColumnFlags_WidthFixed, 38.f);
                    ImGui::TableHeadersRow();
                    // Envelope shapes from ay8910.c (R13: C AtAlH, bits 0-3)
                    static const char* kEnvShape[8] = {
                        "\\___","/___","\\\\\\\\","\\___","\\/\\/","\\````","////","/````"
                    };
                    UINT8 etype = s_shadowAY8910[c][0x0D] & 0x0F;
                    for (int ch = 0; ch < 3; ch++) {
                        UINT16 period  = ((UINT16)(s_shadowAY8910[c][ch*2+1] & 0x0F) << 8) | s_shadowAY8910[c][ch*2];
                        UINT8  vol     = s_shadowAY8910[c][8 + ch] & 0x1F;
                        bool tone_on   = !((mix >> ch) & 1);
                        bool noise_on  = !((mix >> (ch + 3)) & 1);
                        bool env_on    = (vol & 0x10) != 0;
                        float freq     = (tone_on && period > 0) ? (float)ayClk / (16.0f * period) : 0.0f;
                        int ntAY       = (tone_on && freq > 0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteAY[8] = "---";
                        if (ntAY >= 0) snprintf(noteAY, sizeof(noteAY), "%s%d", kAYNote[ntAY%12], ntAY/12-1);
                        ImVec4 col = (tone_on || noise_on) ? ImVec4(0.4f,1.0f,0.6f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%c", 'A'+ch);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", noteAY);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%04X", period);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", vol&0x0F);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%s", env_on ? kEnvShape[etype & 7] : "--");
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "%s", tone_on?"ON":"--");
                        ImGui::TableSetColumnIndex(6); ImGui::TextColored(col, "%s", noise_on?"ON":"--");
                    }
                    ImGui::EndTable();
                }
                // Noise / Envelope info table
                {
                    UINT8 nfrq = s_shadowAY8910[c][0x06] & 0x1F;
                    UINT16 efrq = ((UINT16)s_shadowAY8910[c][0x0C] << 8) | s_shadowAY8910[c][0x0B];
                    UINT8 etype = s_shadowAY8910[c][0x0D] & 0x0F;
                    bool envHold  = (etype >> 3) & 1;
                    bool envAlt   = (etype >> 2) & 1;
                    bool envAtt   = (etype >> 1) & 1;
                    bool envCont  = etype & 1;
                    if (ImGui::BeginTable("##ay8910info", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Item",   ImGuiTableColumnFlags_WidthFixed, 60.f);
                        ImGui::TableSetupColumn("Dec",    ImGuiTableColumnFlags_WidthFixed, 50.f);
                        ImGui::TableSetupColumn("Hex",    ImGuiTableColumnFlags_WidthFixed, 44.f);
                        ImGui::TableSetupColumn("Info",   ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();
                        // Noise frequency
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("Noise");
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", nfrq);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%02X", nfrq);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("N/%d", 1 << (nfrq & 7));
                        // Envelope frequency
                        {
                            int envSteps = 32;
                            if (etype == 10 || etype == 14) envSteps = 64;
                            float eFreq = (efrq > 0) ? (float)ayClk / (8.0f * efrq * envSteps) : 0.0f;
                            int ntEnv = (eFreq > 0.0f) ? FrequencyToMIDINote(eFreq) : -1;
                            char noteEnv[8] = "---";
                            if (ntEnv >= 0) snprintf(noteEnv, sizeof(noteEnv), "%s%d", kAYNote[ntEnv%12], ntEnv/12-1);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::Text("Env Freq");
                            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", noteEnv);
                            ImGui::TableSetColumnIndex(2); ImGui::Text("%04X", efrq);
                            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f Hz", eFreq);
                        }
                        // Envelope type
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("Env Type");
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", etype);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%X", etype);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%s%s%s%s",
                            envAtt?"ATT ":"", envAlt?"ALT ":"", envCont?"CONT ":"", envHold?"HOLD":"");
                        ImGui::EndTable();
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##ay8910")) {
                    RenderRegGrid("ay8910regs", s_shadowAY8910[c], 1);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_SN76496: {
                int c = dev.instance & 1;
                UINT32 snClk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
                static const char* sn_noise_str[] = {"N/512","N/1024","N/2048","Tone3"};
                static const char* kSNNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                // Tone channels table
                if (ImGui::BeginTable("##sn76496tone", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed, 48.f);
                    ImGui::TableSetupColumn("Atten", ImGuiTableColumnFlags_WidthFixed, 36.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 3; ch++) {
                        uint16_t period = s_shadowSN76496[c][ch*2] & 0x03FF;
                        UINT8    vol    = s_shadowSN76496[c][ch*2+1] & 0x0F;
                        bool on = (vol < 15);
                        float freq = (on && period > 0) ? (float)snClk / (32.0f * period) : 0.0f;
                        int ntSN = (on && freq > 0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteSN[8] = "---";
                        if (ntSN >= 0) snprintf(noteSN, sizeof(noteSN), "%s%d", kSNNote[ntSN%12], ntSN/12-1);
                        ImVec4 col = on ? ImVec4(1.0f,0.85f,0.4f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Tone%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", noteSN);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%03X", period);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", vol);
                    }
                    ImGui::EndTable();
                }
                // Noise channel table
                {
                    UINT8 nctl  = s_shadowSN76496[c][6] & 0x07;
                    UINT8 nvol  = s_shadowSN76496[c][7] & 0x0F;
                    int ntype   = nctl & 0x03;
                    int nfb     = (nctl >> 2) & 1;
                    bool non = (nvol < 15);
                    ImVec4 ncol = non ? ImVec4(0.7f,0.7f,0.7f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                    if (ImGui::BeginTable("##sn76496noise", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Src",   ImGuiTableColumnFlags_WidthFixed, 52.f);
                        ImGui::TableSetupColumn("FB",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                        ImGui::TableSetupColumn("Atten", ImGuiTableColumnFlags_WidthFixed, 36.f);
                        ImGui::TableHeadersRow();
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(ncol, "%s", sn_noise_str[ntype & 3]);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(ncol, "%d", nfb);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(ncol, "%X", nvol);
                        ImGui::EndTable();
                    }
                }
                break;
            }
            case DEVID_Y8950: {
                int c = dev.instance & 1;
                static const int   kY8950SlotM[9]  = {0,1,2,8,9,10,16,17,18};
                static const int   kY8950SlotC[9]  = {3,4,5,11,12,13,19,20,21};
                auto y8950SlotOff = [](int s) { return (s % 6) + 8 * (s / 6); };
                static const char* kY8950Note[12]  = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                UINT8 y8950_rhythm = s_shadowY8950[c][0xBD];
                bool  y8950_rm    = (y8950_rhythm >> 5) & 1;
                int   maxCh8950   = y8950_rm ? 6 : 9;
                for (int ch = 0; ch < maxCh8950; ch++) {
                    UINT8 lo   = s_shadowY8950[c][0xA0 + ch];
                    UINT8 hi   = s_shadowY8950[c][0xB0 + ch];
                    int fnum   = ((hi & 0x03) << 8) | lo;
                    int block  = (hi >> 2) & 0x07;
                    bool keyon = s_y8950KeyOn[c][ch];
                    bool keyoff = s_y8950KeyOff[c][ch];
                    UINT8 fb_cn = s_shadowY8950[c][0xC0 + ch];
                    int fb     = (fb_cn >> 1) & 0x07;
                    int cnt    = fb_cn & 0x01;
                    int smOff  = y8950SlotOff(kY8950SlotM[ch]);
                    int scOff  = y8950SlotOff(kY8950SlotC[ch]);
                    int nt8950 = (keyon && fnum > 0) ? FrequencyToMIDINote(
                        49716.0f * (float)fnum / (float)(1 << (20 - block))) : -1;
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "CH%d##y8950ch%d", ch+1, ch);
                    ImGuiTreeNodeFlags y8950flags = ImGuiTreeNodeFlags_DefaultOpen;
                    bool y8950open = ImGui::TreeNodeEx(chLabel, y8950flags);
                    if (y8950open) {
                        // Channel info row in fixed-width table
                        char noteBufY8950[8];
                        if (keyon && nt8950 >= 0)
                            snprintf(noteBufY8950, sizeof(noteBufY8950), "%s%d", kY8950Note[nt8950%12], nt8950/12-1);
                        else if (keyon)
                            snprintf(noteBufY8950, sizeof(noteBufY8950), "F%03X/%d", fnum, block);
                        else
                            snprintf(noteBufY8950, sizeof(noteBufY8950), "---");
                        // Status text and color
                        const char* statusText;
                        ImU32 statusCol;
                        if (keyon && !keyoff) { statusText = "ON";  statusCol = IM_COL32(80,200,80,255); }
                        else if (keyon && keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                        else if (keyoff) { statusText = "REL"; statusCol = IM_COL32(255,200,80,255); }
                        else { statusText = "--"; statusCol = IM_COL32(100,100,100,255); }
                        ImVec4 infoColY8950 = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        if (ImGui::BeginTable("##y8950info", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 28.f);
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("CN",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(
                                ImVec4(((statusCol>>0)&0xFF)/255.f,((statusCol>>8)&0xFF)/255.f,((statusCol>>16)&0xFF)/255.f,1),
                                "%s", statusText);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoColY8950, "%s", noteBufY8950);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoColY8950, "%d", fb);
                            ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoColY8950, "%d", cnt);
                            ImGui::EndTable();
                        }
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##y8950ops", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("DR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("KL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("AM",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("VIB", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableHeadersRow();
                                for (int oi = 0; oi < 2; oi++) {
                                    int sOff    = oi == 0 ? smOff : scOff;
                                    UINT8 reg20 = s_shadowY8950[c][0x20 + sOff];
                                    UINT8 reg40 = s_shadowY8950[c][0x40 + sOff];
                                    UINT8 reg60 = s_shadowY8950[c][0x60 + sOff];
                                    UINT8 reg80 = s_shadowY8950[c][0x80 + sOff];
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", oi==0?"MOD":"CAR");
                                    ImGui::TableSetColumnIndex(1); ImGui::Text("%d", reg60>>4);
                                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", reg60&0xF);
                                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", reg80&0xF);
                                    ImGui::TableSetColumnIndex(4); ImGui::Text("%d", reg80>>4);
                                    ImGui::TableSetColumnIndex(5); ImGui::Text("%d", reg40&0x3F);
                                    ImGui::TableSetColumnIndex(6); ImGui::Text("%d", reg40>>6);
                                    ImGui::TableSetColumnIndex(7); ImGui::Text("%d", reg20&0xF);
                                    ImGui::TableSetColumnIndex(8); ImGui::Text("%d", (reg20>>7)&1);
                                    ImGui::TableSetColumnIndex(9); ImGui::Text("%d", (reg20>>6)&1);
                                }
                                ImGui::EndTable();
                            }
                        ImGui::TreePop();
                    }
                }
                if (y8950_rm) {
                    static const char* kY8950RhName[5] = {"BD","SD","TM","CY","HH"};
                    static const int   kY8950RhBit[5]  = {4,3,2,1,0};
                    static const int   kY8950RhSlot[5] = {0x10,0x14,0x12,0x15,0x11};
                    ImGui::Spacing();
                    ImGui::TextDisabled("-- Rhythm --");
                    if (ImGui::BeginTable("##y8950rhythm", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Inst", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableSetupColumn("TL",   ImGuiTableColumnFlags_WidthFixed, 30.f);
                        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < 5; i++) {
                            bool kon = (y8950_rhythm >> kY8950RhBit[i]) & 1;
                            UINT8 tl = s_shadowY8950[c][0x40 + kY8950RhSlot[i]] & 0x3F;
                            ImVec4 col2 = kon ? ImVec4(1.0f,0.8f,0.4f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(col2, "%s", kY8950RhName[i]);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(col2, "%d", tl);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(col2, "%s", kon?"ON":"--");
                        }
                        ImGui::EndTable();
                    }
                }
                // ADPCM
                ImGui::Spacing();
                ImGui::TextDisabled("-- ADPCM --");
                {
                    UINT8 ctrl  = s_shadowY8950[c][0x07];
                    bool  akon  = (ctrl & 0x20) != 0;
                    UINT8 vol   = s_shadowY8950[c][0x0B] & 0xFF;
                    UINT32 startAddr = ((UINT32)s_shadowY8950[c][0x04] << 8) | s_shadowY8950[c][0x03];
                    UINT32 stopAddr  = ((UINT32)s_shadowY8950[c][0x06] << 8) | s_shadowY8950[c][0x05];
                    if (ImGui::BeginTable("##y8950adpcm", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Stat",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                        ImGui::TableSetupColumn("Vol",   ImGuiTableColumnFlags_WidthFixed, 34.f);
                        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 44.f);
                        ImGui::TableSetupColumn("Stop",  ImGuiTableColumnFlags_WidthFixed, 44.f);
                        ImGui::TableHeadersRow();
                        ImVec4 acol = akon ? ImVec4(1.0f,1.0f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(acol, "%s", akon?"ON":"--");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(acol, "%02X", vol);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(acol, "%04X", startAddr);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(acol, "%04X", stopAddr);
                        ImGui::EndTable();
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##y8950")) {
                    RenderRegGrid("y8950regs", s_shadowY8950[c], 16);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_YMF271: {
                int c = dev.instance & 1;
                // YMF271 (OPX): 12 channels x 4 slots
                // nibble maps group 0-11 -> {0,1,2,4,5,6,8,9,10,12,13,14}
                static const int   kYMF271Nibble[12] = {0,1,2,4,5,6,8,9,10,12,13,14};
                static const char* kYMF271Note[12]   = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                static const double kYMF271Pow[16]   = {128,256,512,1024,2048,4096,8192,16384,0.5,1,2,4,8,16,32,64};
                static const char*  kYMF271SlotName[4] = {"S1","S2","S3","S4"};
                UINT32 clk271 = 16934400;
                for (int grp = 0; grp < 12; grp++) {
                    int nb    = kYMF271Nibble[grp];
                    UINT8 b0  = s_shadowYMF271[c][0][0x00 | nb];
                    bool  kon = (b0 & 0x01) != 0;
                    UINT8 alg = s_shadowYMF271[c][0][0xC0 | nb] & 0x0F;
                    int   pan = (s_shadowYMF271[c][0][0xC0 | nb] >> 4) & 0x03;
                    UINT8 flo = s_shadowYMF271[c][0][0x90 | nb];
                    UINT8 fhi = s_shadowYMF271[c][0][0xA0 | nb];
                    int   blk = (fhi >> 4) & 0x0F;
                    int   fns = ((fhi & 0x0F) << 8) | flo;
                    static const char* pan271[] = {"--"," R"," L","LR"};
                    float freq271 = (kon && fns > 0) ?
                        (float)(fns * kYMF271Pow[blk] / (double)clk271 * 2.0 * clk271 / 512.0) : 0.0f;
                    int nt271 = (kon && freq271 > 0.0f) ? FrequencyToMIDINote(freq271) : -1;
                    char noteStr271[8] = "------";
                    if (nt271 >= 0) snprintf(noteStr271, sizeof(noteStr271), "%s%d", kYMF271Note[nt271%12], nt271/12-1);
                    char chLabel[32];
                    snprintf(chLabel, sizeof(chLabel), "CH%d##ymf271grp%d", grp+1, grp);
                    ImGuiTreeNodeFlags f271 = ImGuiTreeNodeFlags_DefaultOpen;
                    bool open271 = ImGui::TreeNodeEx(chLabel, f271);
                    if (open271) {
                        // Channel info row in fixed-width table
                        ImVec4 infoCol271 = kon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        if (ImGui::BeginTable("##ymf271info", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                            ImGui::TableSetupColumn("AL",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                            ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                            ImGui::TableHeadersRow();
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextColored(infoCol271, "%s", noteStr271);
                            ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoCol271, "%d", alg);
                            ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoCol271, "%s", pan271[pan]);
                            ImGui::EndTable();
                        }
                        ImGui::Spacing();
                        if (ImGui::BeginTable("##ymf271ops", 8, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("D1R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableSetupColumn("D2R", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableHeadersRow();
                                for (int bk = 0; bk < 4; bk++) {
                                    UINT8 r40 = s_shadowYMF271[c][bk][0x40 | nb]; // TL
                                    UINT8 r50 = s_shadowYMF271[c][bk][0x50 | nb]; // AR/D1R
                                    UINT8 r60 = s_shadowYMF271[c][bk][0x60 | nb]; // D2R/RR
                                    UINT8 r70 = s_shadowYMF271[c][bk][0x70 | nb]; // SL/ML
                                    UINT8 tl  = r40 & 0x7F;
                                    UINT8 ar  = (r50 >> 4) & 0x0F;
                                    UINT8 d1r = r50 & 0x0F;
                                    UINT8 d2r = (r60 >> 4) & 0x0F;
                                    UINT8 rr  = r60 & 0x0F;
                                    UINT8 sl  = (r70 >> 4) & 0x0F;
                                    UINT8 ml  = r70 & 0x0F;
                                    ImVec4 rowCol = kon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::TextColored(rowCol, "%s", kYMF271SlotName[bk]);
                                    ImGui::TableSetColumnIndex(1); ImGui::TextColored(rowCol, "%d", ar);
                                    ImGui::TableSetColumnIndex(2); ImGui::TextColored(rowCol, "%d", d1r);
                                    ImGui::TableSetColumnIndex(3); ImGui::TextColored(rowCol, "%d", d2r);
                                    ImGui::TableSetColumnIndex(4); ImGui::TextColored(rowCol, "%d", rr);
                                    ImGui::TableSetColumnIndex(5); ImGui::TextColored(rowCol, "%d", sl);
                                    ImGui::TableSetColumnIndex(6); ImGui::TextColored(rowCol, "%d", tl);
                                    ImGui::TableSetColumnIndex(7); ImGui::TextColored(rowCol, "%d", ml);
                                }
                                ImGui::EndTable();
                            }
                        ImGui::TreePop();
                    }
                }
                ImGui::Spacing();
                for (int bk = 0; bk < 4; bk++) {
                    char treeId[32]; snprintf(treeId, sizeof(treeId), "Bank %d Regs##ymf271b%d", bk, bk);
                    char tblId[32];  snprintf(tblId,  sizeof(tblId),  "ymf271b%dregs", bk);
                    if (ImGui::TreeNode(treeId)) {
                        RenderRegGrid(tblId, s_shadowYMF271[c][bk], 16);
                        ImGui::TreePop();
                    }
                }
                break;
            }
            case DEVID_YMF278B: {
                int c = dev.instance & 1;
                static const int   kW_SlotM[9] = {0,1,2,8,9,10,16,17,18};
                static const int   kW_SlotC[9] = {3,4,5,11,12,13,19,20,21};
                auto wSlotOff = [](int slot) { return (slot%6) + 8*(slot/6); };
                static const char* kW_Pan[]  = {"--"," R"," L","LR"};
                static const char* kW_Note[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                // FM part: OPL3-identical register layout
                for (int p = 0; p < 2; p++) {
                    const UINT8* r = s_shadowYMF278B_fm[c][p];
                    for (int ch = 0; ch < 9; ch++) {
                        int chIdx  = p*9 + ch;
                        UINT8 lo   = r[0xA0 + ch];
                        UINT8 hi   = r[0xB0 + ch];
                        UINT8 c0   = r[0xC0 + ch];
                        int fnum   = ((hi & 0x03) << 8) | lo;
                        int block  = (hi >> 2) & 0x07;
                        bool keyon = (hi >> 5) & 1;
                        int pan    = (c0 >> 4) & 0x03;
                        int fb     = (c0 >> 1) & 0x07;
                        int cnt    = c0 & 0x01;
                        int smOff  = wSlotOff(kW_SlotM[ch]);
                        int scOff  = wSlotOff(kW_SlotC[ch]);
                        int nt = (keyon && fnum > 0) ? FrequencyToMIDINote(
                            49716.0f * (float)fnum / (float)(1 << (20 - block))) : -1;
                        char chLabel[32];
                        snprintf(chLabel, sizeof(chLabel), "CH%d##w278ch%d_%d", chIdx+1, chIdx, c);
                        ImGuiTreeNodeFlags wflags = ImGuiTreeNodeFlags_DefaultOpen;
                        bool wopen = ImGui::TreeNodeEx(chLabel, wflags);
                        if (wopen) {
                            // Channel info row in fixed-width table
                            char noteBufW278[8];
                            if (keyon && nt >= 0)
                                snprintf(noteBufW278, sizeof(noteBufW278), "%s%d", kW_Note[nt%12], nt/12-1);
                            else if (keyon)
                                snprintf(noteBufW278, sizeof(noteBufW278), "F%03X/%d", fnum, block);
                            else
                                snprintf(noteBufW278, sizeof(noteBufW278), "---");
                            ImVec4 infoColW278 = keyon ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                            if (ImGui::BeginTable("##w278info", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                                ImGui::TableSetupColumn("FB",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("CN",   ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                                ImGui::TableHeadersRow();
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::TextColored(infoColW278, "%s", noteBufW278);
                                ImGui::TableSetColumnIndex(1); ImGui::TextColored(infoColW278, "%d", fb);
                                ImGui::TableSetColumnIndex(2); ImGui::TextColored(infoColW278, "%d", cnt);
                                ImGui::TableSetColumnIndex(3); ImGui::TextColored(infoColW278, "%s", kW_Pan[pan]);
                                ImGui::EndTable();
                            }
                            ImGui::Spacing();
                            if (ImGui::BeginTable("##w278ops", 10, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                                ImGui::TableSetupColumn("OP",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("AR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("DR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("RR",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("SL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("KL",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("ML",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("AM",  ImGuiTableColumnFlags_WidthFixed, 26.f);
                                ImGui::TableSetupColumn("VIB", ImGuiTableColumnFlags_WidthFixed, 30.f);
                                ImGui::TableHeadersRow();
                                for (int oi = 0; oi < 2; oi++) {
                                    int sOff    = oi == 0 ? smOff : scOff;
                                    UINT8 reg20 = r[0x20 + sOff];
                                    UINT8 reg40 = r[0x40 + sOff];
                                    UINT8 reg60 = r[0x60 + sOff];
                                    UINT8 reg80 = r[0x80 + sOff];
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", oi==0?"MOD":"CAR");
                                    ImGui::TableSetColumnIndex(1); ImGui::Text("%d", reg60>>4);
                                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", reg60&0xF);
                                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", reg80&0xF);
                                    ImGui::TableSetColumnIndex(4); ImGui::Text("%d", reg80>>4);
                                    ImGui::TableSetColumnIndex(5); ImGui::Text("%d", reg40&0x3F);
                                    ImGui::TableSetColumnIndex(6); ImGui::Text("%d", reg40>>6);
                                    ImGui::TableSetColumnIndex(7); ImGui::Text("%d", reg20&0xF);
                                    ImGui::TableSetColumnIndex(8); ImGui::Text("%d", (reg20>>7)&1);
                                    ImGui::TableSetColumnIndex(9); ImGui::Text("%d", (reg20>>6)&1);
                                }
                                ImGui::EndTable();
                            }
                            ImGui::TreePop();
                        }
                    }
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("FM Port 0##ymf278b")) {
                    RenderRegGrid("ymf278bfm0", s_shadowYMF278B_fm[c][0], 16);
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("FM Port 1##ymf278b")) {
                    RenderRegGrid("ymf278bfm1", s_shadowYMF278B_fm[c][1], 16);
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("PCM Regs##ymf278b")) {
                    RenderRegGrid("ymf278bpcm", s_shadowYMF278B_pcm[c], 8);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_C6280: {
                int c = dev.instance & 1;
                UINT32 hucClk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
                static const char* kHuCNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                if (ImGui::BeginTable("##c6280tbl", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Per",  ImGuiTableColumnFlags_WidthFixed, 44.f);
                    ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed, 24.f);
                    ImGui::TableSetupColumn("R",    ImGuiTableColumnFlags_WidthFixed, 24.f);
                    ImGui::TableSetupColumn("Pan",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 6; ch++) {
                        uint16_t period = ((uint16_t)(s_shadowC6280[c][ch][1] & 0x0F) << 8) | s_shadowC6280[c][ch][0];
                        UINT8 volreg = s_shadowC6280[c][ch][2];
                        UINT8 volL   = (volreg >> 4) & 0x0F;
                        UINT8 volR   = volreg & 0x0F;
                        UINT8 pan    = s_shadowC6280[c][ch][5];
                        bool  on     = (volL > 0 || volR > 0) && period > 0;
                        float freq   = (on && period > 0) ? (float)hucClk / (32.0f * period) : 0.0f;
                        int ntHuC = (on && freq > 0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteHuC[8] = "---";
                        if (ntHuC >= 0) snprintf(noteHuC, sizeof(noteHuC), "%s%d", kHuCNote[ntHuC%12], ntHuC/12-1);
                        ImVec4 col = on ? ImVec4(0.7f,0.5f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Ch%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", noteHuC);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%03X", period);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", volL);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%X", volR);
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "%02X", pan);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_SAA1099: {
                int c = dev.instance & 1;
                UINT32 saaClk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 8000000;
                static const char* kSAANote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                if (ImGui::BeginTable("##saa1099tbl", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",  ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Fr",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Oct",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed, 24.f);
                    ImGui::TableSetupColumn("R",    ImGuiTableColumnFlags_WidthFixed, 24.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 6; ch++) {
                        UINT8 amp     = s_shadowSAA1099[c][0x00 + ch];
                        UINT8 freqReg = s_shadowSAA1099[c][0x08 + ch];
                        int oct_reg   = ch / 2;
                        int oct       = (ch & 1) ? ((s_shadowSAA1099[c][0x10 + oct_reg] >> 4) & 7)
                                                 : (s_shadowSAA1099[c][0x10 + oct_reg] & 7);
                        UINT8 ampL    = (amp >> 4) & 0xF;
                        UINT8 ampR    = amp & 0xF;
                        bool on       = (ampL > 0 || ampR > 0);
                        int denom     = 511 - (int)freqReg;
                        float freqHz  = (on && denom > 0) ? (float)saaClk / 256.0f * (float)(1 << oct) / (float)denom : 0.0f;
                        int ntSAA     = (on && freqHz > 0.0f) ? FrequencyToMIDINote(freqHz) : -1;
                        char noteSAA[8] = "---";
                        if (ntSAA >= 0) snprintf(noteSAA, sizeof(noteSAA), "%s%d", kSAANote[ntSAA%12], ntSAA/12-1);
                        ImVec4 col = on ? ImVec4(0.6f,0.9f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", noteSAA);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", freqReg);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%d", oct);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%X", ampL);
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "%X", ampR);
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##saa1099")) {
                    RenderRegGrid("saa1099regs", s_shadowSAA1099[c], 2);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_POKEY: {
                int c = dev.instance & 1;
                if (ImGui::BeginTable("##pokeytbl", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 36.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 4; ch++) {
                        UINT8 freq = s_shadowPOKEY[c][ch * 2];
                        UINT8 ctrl = s_shadowPOKEY[c][ch * 2 + 1];
                        UINT8 vol  = ctrl & 0x0F;
                        bool  on   = (vol > 0);
                        UINT8 dist = (ctrl >> 5) & 0x07;
                        ImVec4 col = on ? ImVec4(1.0f,0.75f,0.3f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", freq);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", vol);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%X", dist);
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                if (ImGui::TreeNode("All Regs##pokey")) {
                    RenderRegGrid("pokeyregs", s_shadowPOKEY[c], 1);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_NES_APU: {
                int c = dev.instance & 1;
                UINT32 clk = 1789773;
                UINT8 status = s_shadowNES_APU[c][0x15];
                static const char* kDuty[] = {"12%","25%","50%","75%"};
                static const char* kNoiseFreq[] = {"4","8","16","32","64","96","128","160","202","254","380","508","762","1016","2034","4068"};
                static const char* kNESNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                if (ImGui::BeginTable("##nesaputbl", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Duty", ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Extra", ImGuiTableColumnFlags_WidthFixed, 80.f);
                    ImGui::TableHeadersRow();
                    // Pulse 1 & 2
                    for (int ch = 0; ch < 2; ch++) {
                        bool on = (status >> ch) & 1;
                        UINT8 r0 = s_shadowNES_APU[c][ch*4+0];
                        UINT8 r2 = s_shadowNES_APU[c][ch*4+2];
                        UINT8 r3 = s_shadowNES_APU[c][ch*4+3];
                        UINT8 duty = (r0 >> 6) & 0x03;
                        bool const_vol = (r0 >> 4) & 1;
                        UINT8 vol = r0 & 0x0F;
                        uint16_t period = ((r3 & 0x07) << 8) | r2;
                        float freq = (period > 0) ? (float)clk / (16.0f * (period+1)) : 0.0f;
                        int ntNES = (on && freq > 0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteNES[8] = "---";
                        if (ntNES >= 0) snprintf(noteNES, sizeof(noteNES), "%s%d", kNESNote[ntNES%12], ntNES/12-1);
                        ImVec4 col = on ? ImVec4(0.4f,0.8f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "P%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%s", noteNES);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s", kDuty[duty]);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%X%s", vol, const_vol?"/C":"/E");
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "Per=%03X", period);
                    }
                    // Triangle
                    {
                        bool on = (status >> 2) & 1;
                        UINT8 r8 = s_shadowNES_APU[c][0x08];
                        UINT8 rA = s_shadowNES_APU[c][0x0A];
                        UINT8 rB = s_shadowNES_APU[c][0x0B];
                        bool halt = (r8 >> 7) & 1;
                        UINT8 linear = r8 & 0x7F;
                        uint16_t period = ((rB & 0x07) << 8) | rA;
                        float freq = (period > 0) ? (float)clk / (32.0f * (period+1)) : 0.0f;
                        int ntTri = (on && linear>0 && freq>0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteTri[8] = "---";
                        if (ntTri >= 0) snprintf(noteTri, sizeof(noteTri), "%s%d", kNESNote[ntTri%12], ntTri/12-1);
                        ImVec4 col = (on && linear>0) ? ImVec4(0.4f,0.8f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Tri");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", (on&&linear>0)?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%s", noteTri);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "---");
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "L%02X%s", linear, halt?"/H":"");
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "Per=%03X", period);
                    }
                    // Noise
                    {
                        bool on = (status >> 3) & 1;
                        UINT8 rC = s_shadowNES_APU[c][0x0C];
                        UINT8 rE = s_shadowNES_APU[c][0x0E];
                        bool const_vol = (rC >> 4) & 1;
                        UINT8 vol = rC & 0x0F;
                        bool mode = (rE >> 7) & 1;
                        UINT8 nfreq = rE & 0x0F;
                        ImVec4 col = on ? ImVec4(0.6f,0.6f,0.6f,1.0f) : ImVec4(0.4f,0.4f,0.4f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Noi");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "---");
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s", mode?"Short":"Long");
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%X%s", vol, const_vol?"/C":"/E");
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "%s", kNoiseFreq[nfreq]);
                    }
                    // DMC
                    {
                        bool on = (status >> 4) & 1;
                        UINT8 r10 = s_shadowNES_APU[c][0x10];
                        UINT8 r11 = s_shadowNES_APU[c][0x11];
                        UINT8 r12 = s_shadowNES_APU[c][0x12];
                        UINT8 r13 = s_shadowNES_APU[c][0x13];
                        UINT8 rate = r10 & 0x0F;
                        bool loop = (r10 >> 6) & 1;
                        bool irq  = (r10 >> 7) & 1;
                        UINT32 addr = 0xC000 + (UINT32)r12 * 64;
                        UINT32 len  = (UINT32)r13 * 16 + 1;
                        ImVec4 col = on ? ImVec4(1.0f,0.6f,0.3f,1.0f) : ImVec4(0.4f,0.4f,0.4f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "DMC");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "---");
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s%s", loop?"/L":"", irq?"/I":"");
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "R%X O%02X", rate, r11 & 0x7F);
                        ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "A%04X L%d", addr, len);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Status=%02X", status);
                break;
            }
            case DEVID_GB_DMG: {
                int c = dev.instance & 1;
                UINT8 ctrl = s_shadowGB_DMG[c][0x26];
                bool masterOn = (ctrl >> 7) & 1;
                static const char* kDutyStr[] = {"12.5%","25%","50%","75%"};
                static const char* kDMGNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                if (ImGui::BeginTable("##gbdmgtbl", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Duty", ImGuiTableColumnFlags_WidthFixed, 36.f);
                    ImGui::TableSetupColumn("Extra", ImGuiTableColumnFlags_WidthFixed, 80.f);
                    ImGui::TableHeadersRow();
                    // Pulse1
                    {
                        UINT8 nr11 = s_shadowGB_DMG[c][0x11];
                        UINT8 nr12 = s_shadowGB_DMG[c][0x12];
                        UINT8 nr13 = s_shadowGB_DMG[c][0x13];
                        UINT8 nr14 = s_shadowGB_DMG[c][0x14];
                        bool on = masterOn && ((ctrl >> 0) & 1);
                        uint16_t period = ((uint16_t)(nr14 & 0x07) << 8) | nr13;
                        UINT8 duty = (nr11 >> 6) & 0x03;
                        UINT8 vol = (nr12 >> 4) & 0x0F;
                        float freqSq1 = (period < 2048) ? 524288.0f / (float)(2048 - period) : 0.0f;
                        int ntSq1 = (on && freqSq1 > 0.0f) ? FrequencyToMIDINote(freqSq1) : -1;
                        char noteSq1[8] = "---";
                        if (ntSq1 >= 0) snprintf(noteSq1, sizeof(noteSq1), "%s%d", kDMGNote[ntSq1%12], ntSq1/12-1);
                        ImVec4 col = on ? ImVec4(0.4f,0.8f,1.0f,1.0f) : ImVec4(0.4f,0.4f,0.4f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Sq1");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%s", noteSq1);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s", kDutyStr[duty]);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "V%X P%03X", vol, period);
                    }
                    // Pulse2
                    {
                        UINT8 nr21 = s_shadowGB_DMG[c][0x16];
                        UINT8 nr22 = s_shadowGB_DMG[c][0x17];
                        UINT8 nr23 = s_shadowGB_DMG[c][0x18];
                        UINT8 nr24 = s_shadowGB_DMG[c][0x19];
                        bool on = masterOn && ((ctrl >> 1) & 1);
                        uint16_t period = ((uint16_t)(nr24 & 0x07) << 8) | nr23;
                        UINT8 duty = (nr21 >> 6) & 0x03;
                        UINT8 vol = (nr22 >> 4) & 0x0F;
                        float freqSq2 = (period < 2048) ? 524288.0f / (float)(2048 - period) : 0.0f;
                        int ntSq2 = (on && freqSq2 > 0.0f) ? FrequencyToMIDINote(freqSq2) : -1;
                        char noteSq2[8] = "---";
                        if (ntSq2 >= 0) snprintf(noteSq2, sizeof(noteSq2), "%s%d", kDMGNote[ntSq2%12], ntSq2/12-1);
                        ImVec4 col = on ? ImVec4(0.4f,0.8f,1.0f,1.0f) : ImVec4(0.4f,0.4f,0.4f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Sq2");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%s", noteSq2);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s", kDutyStr[duty]);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "V%X P%03X", vol, period);
                    }
                    // Wave
                    {
                        UINT8 nr30 = s_shadowGB_DMG[c][0x1A];
                        UINT8 nr31 = s_shadowGB_DMG[c][0x1B];
                        UINT8 nr32 = s_shadowGB_DMG[c][0x1C];
                        UINT8 nr33 = s_shadowGB_DMG[c][0x1D];
                        UINT8 nr34 = s_shadowGB_DMG[c][0x1E];
                        bool on = masterOn && ((ctrl >> 2) & 1);
                        bool dac = (nr30 >> 7) & 1;
                        uint16_t period = ((uint16_t)(nr34 & 0x07) << 8) | nr33;
                        static const char* kWaveVol[] = {"0%","100%","50%","25%"};
                        UINT8 vol = (nr32 >> 5) & 0x03;
                        ImVec4 col = on ? ImVec4(0.6f,1.0f,0.6f,1.0f) : ImVec4(0.4f,0.4f,0.4f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Wav");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "---");
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "DAC%d", dac);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%s P%03X L%d", kWaveVol[vol], period, nr31);
                    }
                    // Noise
                    {
                        UINT8 nr42 = s_shadowGB_DMG[c][0x21];
                        UINT8 nr43 = s_shadowGB_DMG[c][0x22];
                        bool on = masterOn && ((ctrl >> 3) & 1);
                        UINT8 vol = (nr42 >> 4) & 0x0F;
                        UINT8 clk_div = nr43 & 0x07;
                        UINT8 clk_shift = (nr43 >> 4) & 0x0F;
                        bool short_mode = (nr43 >> 3) & 1;
                        ImVec4 col = on ? ImVec4(0.7f,0.7f,0.7f,1.0f) : ImVec4(0.4f,0.4f,0.4f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "Noi");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "---");
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%s", short_mode?"7bit":"15bit");
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "V%X D%d S%d", vol, clk_div, clk_shift);
                    }
                    ImGui::EndTable();
                }
                ImGui::Text("Master=%02X  Pan=%02X  On=%02X",
                    s_shadowGB_DMG[c][0x24], s_shadowGB_DMG[c][0x25], ctrl);
                break;
            }
            case DEVID_OKIM6258: {
                int c = dev.instance & 1;
                if (ImGui::BeginTable("##okim6258tbl", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("Ctrl",       ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Start/Stop", ImGuiTableColumnFlags_WidthFixed, 60.f);
                    ImGui::TableHeadersRow();
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%02X", s_shadowOKIM6258[c][0x00]);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_shadowOKIM6258[c][0x01]);
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_OKIM6295: {
                int c = dev.instance & 1;
                if (ImGui::BeginTable("##okim6295tbl", 1, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 70.f);
                    ImGui::TableHeadersRow();
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%02X", s_shadowOKIM6295[c][0]);
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_RF5C68: {
                int c = dev.instance & 1;
                if (ImGui::BeginTable("##rf5c68tbl", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("Bank", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("En",   ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableHeadersRow();
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%02X", s_shadowRF5C68[c][0x07]);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%02X", s_shadowRF5C68[c][0x08]);
                    ImGui::EndTable();
                }
                if (ImGui::TreeNode("All Regs##rf5c68")) {
                    RenderRegGrid("rf5c68regs", s_shadowRF5C68[c], 2);
                    ImGui::TreePop();
                }
                break;
            }
            case DEVID_K051649: {
                int c = dev.instance & 1;
                UINT32 sccClk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 1789772;
                static const char* kSCCNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                UINT8 sccKeyon = s_shadowK051649[c][3][0x00];
                if (ImGui::BeginTable("##k051649tbl", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Per",  ImGuiTableColumnFlags_WidthFixed, 44.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 5; ch++) {
                        UINT16 period = ((UINT16)(s_shadowK051649[c][1][ch*2+1] & 0x0F) << 8)
                                      | s_shadowK051649[c][1][ch*2];
                        UINT8  vol    = s_shadowK051649[c][2][ch] & 0x0F;
                        bool   keyon  = (sccKeyon >> ch) & 1;
                        float  freq   = (keyon && period > 0) ? (float)sccClk / (8.0f * period) : 0.0f;
                        int ntSCC = (keyon && freq > 0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteSCC[8] = "---";
                        if (ntSCC >= 0) snprintf(noteSCC, sizeof(noteSCC), "%s%d", kSCCNote[ntSCC%12], ntSCC/12-1);
                        ImVec4 col = keyon ? ImVec4(0.4f,0.7f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", keyon?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%s", noteSCC);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%04X", period);
                        ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_WSWAN: {
                int c = dev.instance & 1;
                static const char* kWSNote[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                UINT8 wsCtrl = s_shadowWSWAN[c][0x10];  // SNDMOD (VGM offset 0x10)
                if (ImGui::BeginTable("##wswantbl", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Per",  ImGuiTableColumnFlags_WidthFixed, 44.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 4; ch++) {
                        UINT16 period = ((UINT16)(s_shadowWSWAN[c][ch*2+1] & 0x07) << 8) | s_shadowWSWAN[c][ch*2];
                        UINT8  vol    = s_shadowWSWAN[c][0x08 + ch] & 0x0F;
                        bool   on     = ((wsCtrl >> ch) & 1) && (vol > 0);
                        int    pdenom = 2048 - (int)period;
                        float  freq   = (on && pdenom > 0) ? (3072000.0f / 16.0f) / (float)pdenom : 0.0f;
                        int ntWS = (on && freq > 0.0f) ? FrequencyToMIDINote(freq) : -1;
                        char noteWS[8] = "---";
                        if (ntWS >= 0) snprintf(noteWS, sizeof(noteWS), "%s%d", kWSNote[ntWS%12], ntWS/12-1);
                        ImVec4 col = on ? ImVec4(0.6f,0.8f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", noteWS);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%03X", period);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", vol);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Ctrl=%02X Out=%02X", wsCtrl, s_shadowWSWAN[c][0x11]);  // SNDOUT at VGM offset 0x11
                break;
            }
            case DEVID_YMZ280B: {
                int c = dev.instance & 1;
                UINT32 zclk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 16934400;
                ImGui::TextDisabled("-- YMZ280B ADPCM (8 ch) --");
                if (ImGui::BeginTable("##ymz280btbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 36.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 80.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 8; ch++) {
                        UINT8 ctrl  = s_shadowYMZ280B[c][ch * 4 + 0];
                        bool  on    = (ctrl & 0x80) != 0;
                        UINT8 vol   = s_shadowYMZ280B[c][ch * 4 + 2] & 0xFF;
                        UINT8 freqr = s_shadowYMZ280B[c][ch * 4 + 1] & 0xFF;
                        // freq = (freqr + 1) * clk / 256 / 1024
                        float freq  = on ? ((freqr + 1.0f) * zclk / 262144.0f) : 0.0f;
                        ImVec4 col = on ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d %s", ch+1, on?"ON":"--");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%d", vol);
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%.0f Hz", freq);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_YMW258: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- MultiPCM (28 ch) --");
                if (ImGui::BeginTable("##ymw258tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",  ImGuiTableColumnFlags_WidthFixed, 36.f);
                    ImGui::TableSetupColumn("TL",  ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 80.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 28; ch++) {
                        UINT8 kon = s_shadowYMW258[c][ch * 8 + 5];
                        bool  on  = (kon & 0x80) != 0;
                        UINT8 tl  = (kon >> 1) & 0x7F;
                        float lv  = on ? (1.0f - tl / 127.0f) : 0.0f;
                        ImVec4 col = on ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d %s", ch+1, on?"ON":"--");
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%d", tl);
                        ImGui::TableSetColumnIndex(2); ImGui::ProgressBar(lv, ImVec2(-1,0));
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_uPD7759: {
                int c = dev.instance & 1;
                bool on = (s_shadowUPD7759[c][0] & 0x01) != 0;
                if (ImGui::BeginTable("##upd7759tbl", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableHeadersRow();
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("CH1");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%s", on?"ON":"--");
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_K054539: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- K054539 PCM (8 ch) --");
                UINT8 kon = s_shadowK054539[c][0][0x14];
                if (ImGui::BeginTable("##k054539tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 8; ch++) {
                        bool  on  = (kon >> ch) & 1;
                        UINT8 vol = s_shadowK054539[c][0][ch * 0x20 + 0x03] & 0x7F;
                        ImVec4 col = on ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_C140: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- C140 PCM (24 ch) --");
                if (ImGui::BeginTable("##c140tbl", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("R",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 24; ch++) {
                        UINT8 ctrl = s_shadowC140[c][ch * 16 + 5];
                        bool  on   = (ctrl & 0x80) != 0;
                        UINT8 volR = s_shadowC140[c][ch * 16 + 0];
                        UINT8 volL = s_shadowC140[c][ch * 16 + 1];
                        ImVec4 col = on ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", volL);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%02X", volR);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_K053260: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- K053260 PCM (4 ch) --");
                UINT8 kon = s_shadowK053260[c][0x00];
                if (ImGui::BeginTable("##k053260tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 4; ch++) {
                        bool  on  = (kon >> ch) & 1;
                        UINT8 vol = s_shadowK053260[c][ch * 8 + 0x08] & 0xFF;
                        ImVec4 col = on ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_QSOUND: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- QSound (16 ch) --");
                if (ImGui::BeginTable("##qsoundtbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 16; ch++) {
                        UINT8 vol = s_shadowQSound[c][ch * 2];
                        bool on = (vol > 0);
                        ImVec4 col = on ? ImVec4(1.0f,0.7f,0.3f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_SCSP: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- SCSP/YMF292 (32 ch) --");
                if (ImGui::BeginTable("##scsptbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("TL",   ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 32; ch++) {
                        bool  on  = (s_shadowSCSP[c][ch * 0x20] & 0x08) != 0;
                        UINT8 tl  = s_shadowSCSP[c][ch * 0x20 + 0x0D];
                        ImVec4 col = on ? ImVec4(0.5f,1.0f,0.8f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", tl);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_VBOY_VSU: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- VirtualBoy VSU (6 ch) --");
                if (ImGui::BeginTable("##vboyvsutbl", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("R",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 6; ch++) {
                        UINT8 ctrl  = s_shadowVBOY_VSU[c][ch * 0x10];
                        bool  on    = (ctrl & 0x80) != 0;
                        UINT8 volLR = s_shadowVBOY_VSU[c][ch * 0x10 + 1];
                        UINT8 volL  = (volLR >> 4) & 0x0F;
                        UINT8 volR  = volLR & 0x0F;
                        ImVec4 col  = on ? ImVec4(1.0f,0.5f,0.5f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%X", volL);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", volR);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_ES5503: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- ES5503 DOC (32 osc) --");
                if (ImGui::BeginTable("##es5503tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("OSC",  ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 32; ch++) {
                        UINT8 ctrl = s_shadowES5503[c][ch + 0xA0];
                        bool  on   = !(ctrl & 0x01);
                        UINT8 vol  = s_shadowES5503[c][ch + 0x80];
                        ImVec4 col = on ? ImVec4(0.9f,0.7f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "OSC%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_ES5506: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- ES5506 OTTO (32 ch) --");
                if (ImGui::BeginTable("##es5506tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 32; ch++) {
                        UINT8 vol = s_shadowES5506[c][ch * 4 + 1];
                        bool  on  = (vol > 0);
                        ImVec4 col = on ? ImVec4(0.9f,0.7f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_X1_010: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- Seta X1-010 (16 ch) --");
                if (ImGui::BeginTable("##x1010tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 16; ch++) {
                        UINT8 ctrl = s_shadowX1_010[c][ch * 0x10];
                        bool  on   = (ctrl & 0x01) != 0;
                        UINT8 vol  = s_shadowX1_010[c][ch * 0x10 + 2];
                        ImVec4 col = on ? ImVec4(1.0f,0.8f,0.5f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_C352: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- C352 PCM (32 ch) --");
                if (ImGui::BeginTable("##c352tbl", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("R",    ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 32; ch++) {
                        UINT8 ctrl = s_shadowC352[c][ch * 16 + 2];
                        bool  on   = (ctrl & 0x40) != 0;
                        UINT8 volL = s_shadowC352[c][ch * 16 + 4];
                        UINT8 volR = s_shadowC352[c][ch * 16 + 5];
                        ImVec4 col = on ? ImVec4(0.8f,0.9f,1.0f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", volL);
                        ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%02X", volR);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_GA20: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- Irem GA20 (4 ch) --");
                if (ImGui::BeginTable("##ga20tbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 4; ch++) {
                        UINT8 on_b = s_shadowGA20[c][ch * 8 + 6];
                        bool  on   = (on_b != 0);
                        UINT8 vol  = s_shadowGA20[c][ch * 8 + 5];
                        ImVec4 col = on ? ImVec4(1.0f,0.7f,0.5f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_SEGAPCM: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- Sega PCM (16 ch) --");
                if (ImGui::BeginTable("##segapcmtbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("CH",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthFixed, 32.f);
                    ImGui::TableSetupColumn("Vol",  ImGuiTableColumnFlags_WidthFixed, 34.f);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 16; ch++) {
                        UINT8 ctrl = s_shadowSEGAPCM[c][ch * 8 + 7];
                        bool  on   = (ctrl & 0x01) == 0;
                        UINT8 vol  = s_shadowSEGAPCM[c][ch * 8 + 6];
                        ImVec4 col = on ? ImVec4(0.6f,1.0f,0.7f,1.0f) : ImVec4(0.5f,0.5f,0.5f,1.0f);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%02d", ch+1);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", on?"ON":"--");
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%02X", vol);
                    }
                    ImGui::EndTable();
                }
                break;
            }
            case DEVID_32X_PWM: {
                int c = dev.instance & 1;
                ImGui::TextDisabled("-- 32X PWM --");
                static const char* pwmRegName[5] = {"Int","Cycle","Out_L","Out_R","Mono"};
                if (ImGui::BeginTable("##32xpwtbl", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("Reg",  ImGuiTableColumnFlags_WidthFixed, 50.f);
                    ImGui::TableSetupColumn("Hex",  ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 60.f);
                    ImGui::TableHeadersRow();
                    for (int r = 0; r < 5; r++) {
                        UINT16 val = s_shadow32XPWM[c][r];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", pwmRegName[r]);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%03X", val);
                        ImGui::TableSetColumnIndex(2);
                        if (r == 1 && val > 0) ImGui::TextDisabled("=%d", val+1);
                        else ImGui::TextDisabled("");
                    }
                    ImGui::EndTable();
                }
                break;
            }
            default: {
                ImGui::TextDisabled("(no display for chip type %02X)", dev.type);
                break;
            }
        }
        ImGui::Spacing();
    }
}

void RenderFileBrowserPanel() {
    RenderInline();
}

void RenderLogPanel() {
    // ===== VGM Debug Log =====
    if (ImGui::CollapsingHeader("VGM Debug Log##log", nullptr, 0)) {
        ImGui::Checkbox("Auto-scroll##vgmlog", &s_vgmLogAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##vgmlog")) {
            s_vgmLog.clear(); s_vgmLogDisplay[0] = '\0'; s_vgmLogLastSize = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All##vgmlog")) {
            ImGui::SetClipboardText(s_vgmLog.c_str());
        }
        size_t copyLen = s_vgmLog.size() < sizeof(s_vgmLogDisplay)-1 ? s_vgmLog.size() : sizeof(s_vgmLogDisplay)-1;
        memcpy(s_vgmLogDisplay, s_vgmLog.c_str(), copyLen);
        s_vgmLogDisplay[copyLen] = '\0';
        bool changed = (s_vgmLog.size() != s_vgmLogLastSize);
        s_vgmLogLastSize = s_vgmLog.size();
        if (s_vgmLogAutoScroll && changed) s_vgmLogScrollToBottom = true;
        float logH = 150;
        ImGui::BeginChild("VgmDebugLogRegion", ImVec2(0, logH), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 tsz = ImGui::CalcTextSize(s_vgmLogDisplay, NULL, false, -1.0f);
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float minH  = ImGui::GetContentRegionAvail().y;
        float inH   = (tsz.y > minH) ? tsz.y + lineH * 2 : minH;
        ImGui::InputTextMultiline("##VgmLogText", s_vgmLogDisplay, sizeof(s_vgmLogDisplay),
            ImVec2(-1, inH), ImGuiInputTextFlags_ReadOnly);
        if (s_vgmLogScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            s_vgmLogScrollToBottom = false;
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // ===== VGM Folder History =====
    bool prevHistoryCollapsed = s_vgmHistoryCollapsed;
    ImGui::SetNextItemAllowOverlap();
    bool historyOpen = ImGui::TreeNodeEx("VGM Folder History##hist",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_vgmHistoryCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
    if (historyOpen) s_vgmHistoryCollapsed = false;
    else s_vgmHistoryCollapsed = true;
    if (s_vgmHistoryCollapsed != prevHistoryCollapsed) SaveConfig();

    // Title bar mini controls
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Clear##VgmHistClear")) {
        s_folderHistory.clear();
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All");
    ImGui::SameLine();
    if (s_histSortMode == 0) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f,0.6f,1.0f,1.0f)); }
    if (ImGui::SmallButton(s_histSortMode == 0 ? "Time##histSortTime" : "Time##histSortTime")) {
        s_histSortMode = 0;
    }
    if (s_histSortMode == 0) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by time");
    ImGui::SameLine();
    if (s_histSortMode == 1) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f,0.6f,1.0f,1.0f)); }
    if (ImGui::SmallButton(s_histSortMode == 1 ? "Freq##histSortFreq" : "Freq##histSortFreq")) {
        s_histSortMode = 1;
    }
    if (s_histSortMode == 1) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##histFilter", "Filter...", s_histFilter, sizeof(s_histFilter));

    if (historyOpen) {
        // Build sorted/filtered/deduplicated list: {folderName, firstIndex, fileCount}
        struct HistEntry { std::string name; int idx; int fileCount; };
        std::vector<HistEntry> entries;
        std::set<std::string> seen;
        for (int i = 0; i < (int)s_folderHistory.size(); i++) {
            size_t lastSlash = s_folderHistory[i].find_last_of("\\/");
            std::string folderName = (lastSlash != std::string::npos) ? s_folderHistory[i].substr(lastSlash + 1) : s_folderHistory[i];
            // Apply filter
            if (s_histFilter[0] != '\0') {
                std::string lowerName = folderName;
                std::string lowerFilter = s_histFilter;
                for (auto& c : lowerName) c = tolower(c);
                for (auto& c : lowerFilter) c = tolower(c);
                if (lowerName.find(lowerFilter) == std::string::npos) continue;
            }
            // Deduplicate: keep first occurrence (most recent)
            if (seen.insert(folderName).second) {
                // Count .vgm/.vgz files in the directory
                int fileCount = 0;
                const char* exts[] = { "\\*.vgm", "\\*.vgz", "\\*.VGM", "\\*.VGZ" };
                for (int e = 0; e < 4; e++) {
                    std::string searchPath = s_folderHistory[i] + exts[e];
                    std::wstring wSearchPath = MidiPlayer::UTF8ToWide(searchPath);
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
                    if (h != INVALID_HANDLE_VALUE) {
                        do {
                            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                            fileCount++;
                        } while (FindNextFileW(h, &fd));
                        FindClose(h);
                    }
                }
                entries.push_back({folderName, i, fileCount});
            }
        }
        // Sort by file count if selected
        if (s_histSortMode == 1) {
            std::sort(entries.begin(), entries.end(), [](const HistEntry& a, const HistEntry& b) {
                return a.fileCount > b.fileCount;
            });
        }

        ImGui::Separator();
        float historyHeight = ImGui::GetContentRegionAvail().y - 5;
        ImGui::BeginChild("VgmHistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

        if (entries.empty()) {
            ImGui::TextDisabled("No matching folders...");
        } else {
            for (const auto& e : entries) {
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

void RenderInline() {
    // Handle end-of-track FIRST, before capturing hasFile, to avoid
    // using stale state after UnloadFile() within the same frame.
    if (s_fileLoaded && (s_playState & VGM_PLAY) && (s_playState & 0x80)) {
        s_playState.fetch_and((UINT8)~0x80);
        if (s_autoPlayNext && s_isSequentialPlayback &&
            s_playlistIndex + 1 < (int)s_playlist.size()) {
            s_playlistIndex++;
            LoadFile(s_playlist[s_playlistIndex].c_str());
        } else if (s_autoPlayNext && !s_isSequentialPlayback && !s_playlist.empty()) {
            s_playlistIndex = rand() % (int)s_playlist.size();
            LoadFile(s_playlist[s_playlistIndex].c_str());
        } else {
            UnloadFile();
        }
    }

    bool hasFile = s_fileLoaded;
    bool isPlaying = hasFile && (s_playState == VGM_PLAY);
    bool isPaused  = hasFile && (s_playState == VGM_PAUSE);

    ImGui::SetNextItemAllowOverlap();
    bool playerOpen = ImGui::TreeNodeEx("VGM Player##vgmplayer",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (s_vgmInlinePlayerCollapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));

    // Semi-transparent progress bar as title bar background (collapsed)
    if (!playerOpen && hasFile) {
        double pos = s_player.GetCurTime(0);
        double dur = s_player.GetTotalTime(0);
        float progress = (dur > 0.0) ? (float)(pos / dur) : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

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

    // ===== Title bar mini controls (always visible) =====
    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("<<##miniprev")) {
        if (!s_playlist.empty() && s_playlistIndex > 0) {
            s_playlistIndex--;
            if (s_playlistIndex < (int)s_playlist.size())
                LoadFile(s_playlist[s_playlistIndex].c_str());
        }
    }
    ImGui::SameLine();
    if (isPlaying) {
        if (ImGui::SmallButton("||##minipause")) {
            s_playState = VGM_PAUSE;
            if (s_audDrv) AudioDrv_Pause(s_audDrv);
        }
    } else if (isPaused) {
        if (ImGui::SmallButton(">##minipause")) {
            s_playState = VGM_PLAY;
            if (s_audDrv) AudioDrv_Resume(s_audDrv);
        }
    } else if (hasFile) {
        if (ImGui::SmallButton(">##minipause")) {
            std::string reloadPath = s_loadedPath;
            UnloadFile();
            LoadFile(reloadPath.c_str());
        }
    } else {
        ImGui::SmallButton(">##minipause");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">>##mininext")) {
        if (!s_playlist.empty() && s_playlistIndex + 1 < (int)s_playlist.size()) {
            s_playlistIndex++;
            LoadFile(s_playlist[s_playlistIndex].c_str());
        }
    }
    ImGui::SameLine();

    // Scrolling filename on title bar (max 25 chars width)
    if (hasFile) {
        const char* fname = s_loadedPath.c_str();
        const char* slash = strrchr(fname, '\\');
        if (!slash) slash = strrchr(fname, '/');
        const char* displayName = slash ? slash + 1 : fname;

        ImU32 nameCol;
        if (isPlaying) nameCol = IM_COL32(100, 255, 100, 255);
        else if (isPaused) nameCol = IM_COL32(255, 255, 100, 255);
        else nameCol = IM_COL32(220, 220, 220, 255);

        float maxNameW = ImGui::CalcTextSize("ABCDEFGHIJKLMNOPQRSTUVWX"
                                              "ABCDEFGHIJKLMNOP").x;
        DrawScrollingText("##minifilename", displayName, nameCol, maxNameW);
    } else {
        ImGui::TextDisabled("(no file)");
    }

    // Time text on title bar right side (collapsed)
    if (!playerOpen && hasFile) {
        double pos = s_player.GetCurTime(0);
        double dur = s_player.GetTotalTime(0);
        int curSec = (int)pos; int curMin = curSec / 60; curSec %= 60;
        int totSec = (int)dur; int totMin = totSec / 60; totSec %= 60;
        char timeStr[64];
        sprintf(timeStr, "%02d:%02d / %02d:%02d", curMin, curSec, totMin, totSec);

        float timeW = ImGui::CalcTextSize(timeStr).x;
        float contentRight = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(contentRight - timeW);
        ImGui::Text("%s", timeStr);
    }

    if (playerOpen) {
        s_vgmInlinePlayerCollapsed = false;

        float buttonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
        if (ImGui::Button("Play", ImVec2(buttonWidth, 30))) {
            if (!hasFile) {
                // nothing to play
            } else if (isPaused) {
                s_playState = VGM_PLAY;
                if (s_audDrv) AudioDrv_Resume(s_audDrv);
            } else {
                std::string reloadPath = s_loadedPath;
                UnloadFile();
                LoadFile(reloadPath.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause", ImVec2(buttonWidth, 30))) {
            if (isPlaying) {
                s_playState = VGM_PAUSE;
                if (s_audDrv) AudioDrv_Pause(s_audDrv);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(buttonWidth, 30))) {
            UnloadFile();
        }

        float navButtonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
        if (ImGui::Button("<< Prev", ImVec2(navButtonWidth, 25))) {
            if (!s_playlist.empty() && s_playlistIndex > 0) {
                s_playlistIndex--;
                if (s_playlistIndex < (int)s_playlist.size())
                    LoadFile(s_playlist[s_playlistIndex].c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>", ImVec2(navButtonWidth, 25))) {
            if (!s_playlist.empty() && s_playlistIndex + 1 < (int)s_playlist.size()) {
                s_playlistIndex++;
                LoadFile(s_playlist[s_playlistIndex].c_str());
            }
        }

        ImGui::Checkbox("Auto-play", &s_autoPlayNext);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically play next track when current finishes");
        ImGui::SameLine();
        const char* modeText = s_isSequentialPlayback ? "Seq" : "Rnd";
        if (ImGui::Button(modeText, ImVec2(35, 0)))
            s_isSequentialPlayback = !s_isSequentialPlayback;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sequential / Random");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::InputInt("Loops##vgm", &s_loopCount)) {
            if (s_loopCount < 1) s_loopCount = 1;
            if (s_loopCount > 99) s_loopCount = 99;
            s_player.SetLoopCount((UINT32)s_loopCount);
            SavePlayerState();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderFloat("Vol##vgm", &s_masterVolume, 0.0f, 2.0f, "%.2f")) {
            PlayerA::Config cfg = s_player.GetConfiguration();
            cfg.masterVol = (UINT32)(s_masterVolume * 0x10000);
            s_player.SetConfiguration(cfg);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Master volume (0.0 - 2.0), scroll to adjust");
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                s_masterVolume += wheel * 0.05f;
                if (s_masterVolume < 0.0f) s_masterVolume = 0.0f;
                if (s_masterVolume > 2.0f) s_masterVolume = 2.0f;
                PlayerA::Config cfg = s_player.GetConfiguration();
                cfg.masterVol = (UINT32)(s_masterVolume * 0x10000);
                s_player.SetConfiguration(cfg);
            }
        }

        ImGui::Spacing();
        if (isPlaying) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Playing:");
        } else if (isPaused) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused:");
        } else {
            ImGui::Text("Ready:");
        }
        ImGui::SameLine();
        if (hasFile) {
            const char* fname = s_loadedPath.c_str();
            const char* slash = strrchr(fname, '\\');
            if (!slash) slash = strrchr(fname, '/');
            ImGui::Text("%s", slash ? slash + 1 : fname);
        } else {
            ImGui::TextDisabled("(no file loaded)");
        }

        // Progress bar with seek
        if (hasFile) {
            double pos = s_player.GetCurTime(PLAYTIME_LOOP_INCL);
            double dur = s_player.GetTotalTime(PLAYTIME_LOOP_INCL);
            float progress = (dur > 0.0) ? (float)(pos / dur) : 0.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            int curSec = (int)pos; int curMin = curSec / 60; curSec %= 60;
            int totSec = (int)dur; int totMin = totSec / 60; totSec %= 60;
            char curStr[32], totStr[32];
            sprintf(curStr, "%02d:%02d", curMin, curSec);
            sprintf(totStr, "%02d:%02d", totMin, totSec);
            ImGui::Text("%s / %s", curStr, totStr);

            // Slider for seeking — only seek on mouse release to avoid repeated seek stutter
            static float seek_progress = 0.0f;
            ImGui::SliderFloat("##Progress", &seek_progress, 0.0f, 1.0f, "");
            if (ImGui::IsItemActive()) {
                // Dragging: update display value but don't seek yet
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
                // Mouse released: perform the actual seek
                if (dur > 0.0f) {
                    UINT32 targetSample = (UINT32)(seek_progress * dur * (double)s_sampleRate);
                    s_player.Seek(PLAYPOS_SAMPLE, targetSample);
                }
            } else {
                // Idle: sync slider to current playback position
                seek_progress = progress;
            }
        } else {
            ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
        }

        ImGui::Spacing();
        ImGui::Separator();
    } else {
        s_vgmInlinePlayerCollapsed = true;
    }

    bool browserOpen = ImGui::TreeNodeEx("VGM File Browser##browser",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_DefaultOpen);
    // Filter on title bar
    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##fileBrowserFilter", "Filter...", s_fileBrowserFilter, sizeof(s_fileBrowserFilter));
    if (browserOpen) {
        RenderFileBrowser();
    }
}

bool GetScopeMode() {
    return s_showScope;
}

float GetScopeBackgroundHeight() {
    return s_scopeBackgroundHeight;
}

void SetScopeBackgroundHeight(float height) {
    if (height < 100.0f) height = 100.0f;
    if (height > 600.0f) height = 600.0f;
    s_scopeBackgroundHeight = height;
}

void RenderTab() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("libvgm")) { ImGui::End(); return; }

    ImGui::BeginChild("VGM_LeftPane", ImVec2(300, 0), true);
    RenderControls();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("VGM_RightPane", ImVec2(0, 0), false);

    static float s_levelMeterHeight = 200.0f;

    float pianoHeight      = 150;
    float statusAreaWidth  = 560;

    // Calculate top section height based on mode
    float topSectionHeight;
    if (GetScopeMode()) {
        topSectionHeight = pianoHeight + GetScopeBackgroundHeight() + 4;  // +4 for splitter
    } else {
        topSectionHeight = pianoHeight + s_levelMeterHeight;
    }

    ImGui::BeginGroup();
    ImGui::BeginChild("VGM_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoArea();
    ImGui::EndChild();

    // Display either level meter or scope area based on mode
    if (GetScopeMode()) {
        // Scope mode: show oscilloscope background area
        ImGui::BeginChild("VGM_ScopeBackground", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, GetScopeBackgroundHeight()), false);
        RenderScopeArea();
        ImGui::EndChild();

        // Resizable splitter below scope area - adjusts scope background height only
        ImGui::Button("##ScopeSplitter", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, 4));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float delta = ImGui::GetIO().MouseDelta.y;
            SetScopeBackgroundHeight(GetScopeBackgroundHeight() + delta);
            SavePlayerState();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    } else {
        // Level meter mode: show level meter area
        ImGui::BeginChild("VGM_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, s_levelMeterHeight), false);
        RenderLevelMeterArea();
        ImGui::EndChild();
    }

    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginChild("VGM_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderStatusArea();
    ImGui::EndChild();

    ImGui::BeginChild("VGM_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderFileBrowserPanel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("VGM_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();

    ImGui::EndChild(); // VGM_RightPane
    RenderChipAliasWindow();
    RenderScopeSettingsWindow();
    ImGui::End();
}

} // namespace VgmWindow

