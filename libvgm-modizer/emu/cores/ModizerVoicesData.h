#ifndef MODIZER_VOICES_DATA_H
#define MODIZER_VOICES_DATA_H

#include "ModizerConstants.h"
#include <stdint.h>

/* ---- Legacy global variables (kept for prebuilt .a compatibility) ---- */
extern signed char *m_voice_buff[SOUND_MAXVOICES_BUFFER_FX];
extern int64_t m_voice_current_ptr[SOUND_MAXVOICES_BUFFER_FX];
extern int m_voice_ChipID[SOUND_MAXVOICES_BUFFER_FX];
extern signed char m_voice_current_system;
extern signed char m_voice_current_systemSub;
extern char m_voice_current_systemPairedOfs;
extern char m_voice_current_total;
extern int m_voice_current_samplerate;
extern int g_ssg_scope_shift;  /* SSG scope bit shift amount (0-10, default 6) */

extern unsigned int vgm_last_vol[SOUND_MAXVOICES_BUFFER_FX];
extern unsigned int vgm_last_note[SOUND_MAXVOICES_BUFFER_FX];
extern unsigned char vgm_last_instr[SOUND_MAXVOICES_BUFFER_FX];

extern int m_genNumVoicesChannels;

/* ---- Scope chip registration table ---- */
/* Each chip instance registers itself with a unique (chip_name, instance) pair.
 * The registration assigns a slot_base in the global m_voice_buff[] array.
 * Chip update functions call scope_register_chip() to find their offset. */

/* Channel type identifiers for per-channel classification */
#define SCOPE_CH_FM      0   /* FM synthesis channel */
#define SCOPE_CH_SSG     1   /* SSG/PSG square wave channel */
#define SCOPE_CH_ADPCM_A 2   /* ADPCM-A rhythm/sample channel */
#define SCOPE_CH_ADPCM_B 3   /* ADPCM-B delta-T channel */
#define SCOPE_CH_DAC     4   /* Direct DAC channel */
#define SCOPE_CH_NOISE   5   /* Noise channel */
#define SCOPE_CH_RHYTHM  6   /* Rhythm/percussion channel */

/* Maximum number of channel groups per chip */
#define SCOPE_MAX_GROUPS 8

/* Describes one group of channels (e.g. "FM 1-6", "ADPCM-A 1-6") */
typedef struct {
    int type;           /* SCOPE_CH_FM, SCOPE_CH_SSG, etc. */
    int count;          /* number of channels in this group */
    int start;          /* channel index offset within the chip (0-based) */
    const char *label;  /* short label prefix, e.g. "FM", "SSG", "ADPCM-A", "ADPCM-B" */
} ScopeChGroup;

typedef struct {
    const char *chip_name;    /* e.g. "YM2151", "YM2608" */
    int         chip_inst;    /* instance index for same-type chips (0, 1, ...) */
    int         num_channels; /* total voice channels for this chip */
    int         slot_base;    /* base index in m_voice_buff[], assigned at registration */
    int         active;       /* 1 = slot is in use */
    int         num_groups;   /* number of channel groups */
    ScopeChGroup groups[SCOPE_MAX_GROUPS]; /* channel group descriptions */
} ScopeChipSlot;

#ifdef __cplusplus
extern "C" {
#endif

extern ScopeChipSlot g_scope_slots[SCOPE_MAX_CHIPS];
extern int g_scope_chip_count;

/* Register or find a scope slot for a chip instance.
 * If a slot with matching (chip_name, chip_inst) exists, returns it.
 * Otherwise allocates a new slot.
 * Returns pointer to the slot, or NULL if table is full. */
ScopeChipSlot* scope_register_chip(const char *chip_name, int chip_inst,
                                    int num_channels, int samplerate);

/* Find a scope slot by chip_name and instance. Returns NULL if not found. */
ScopeChipSlot* scope_find_slot(const char *chip_name, int chip_inst);

/* Reset all scope slots (called when unloading a file). */
void scope_reset_all(void);

/* Get a scope slot by index (for debugging). Returns NULL if index is out of range. */
ScopeChipSlot* scope_find_slot_by_index(int index);

#ifdef __cplusplus
}
#endif

#endif
