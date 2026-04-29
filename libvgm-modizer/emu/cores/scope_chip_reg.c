// scope_chip_reg.c - Scope chip registration table implementation
// Used by chip emulator cores (fmopn.c, ym2151.c) to register their
// oscilloscope buffer slots by chip name and instance.

#include "ModizerVoicesData.h"
#include <string.h>

ScopeChipSlot g_scope_slots[SCOPE_MAX_CHIPS];
int g_scope_chip_count = 0;

ScopeChipSlot* scope_find_slot(const char *chip_name, int chip_inst)
{
    for (int i = 0; i < g_scope_chip_count; i++) {
        if (g_scope_slots[i].active &&
            g_scope_slots[i].chip_inst == chip_inst &&
            strcmp(g_scope_slots[i].chip_name, chip_name) == 0) {
            return &g_scope_slots[i];
        }
    }
    return NULL;
}

ScopeChipSlot* scope_register_chip(const char *chip_name, int chip_inst,
                                    int num_channels, int samplerate)
{
    /* Try to find existing slot first */
    ScopeChipSlot *slot = scope_find_slot(chip_name, chip_inst);
    if (slot) {
        slot->num_channels = num_channels;
        if (samplerate > 0) m_voice_current_samplerate = samplerate;
        return slot;
    }

    /* Allocate new slot */
    if (g_scope_chip_count >= SCOPE_MAX_CHIPS) return NULL;

    /* Calculate slot_base: sum of all existing channels */
    int base = 0;
    for (int i = 0; i < g_scope_chip_count; i++) {
        if (g_scope_slots[i].active)
            base += g_scope_slots[i].num_channels;
    }
    /* Ensure we don't exceed the global buffer size */
    if (base + num_channels > SOUND_MAXVOICES_BUFFER_FX) return NULL;

    ScopeChipSlot *s = &g_scope_slots[g_scope_chip_count];
    s->chip_name = chip_name;
    s->chip_inst = chip_inst;
    s->num_channels = num_channels;
    s->slot_base = base;
    s->active = 1;
    s->num_groups = 0;
    memset(s->groups, 0, sizeof(s->groups));
    g_scope_chip_count++;

    if (samplerate > 0) m_voice_current_samplerate = samplerate;
    return s;
}

void scope_reset_all(void)
{
    g_scope_chip_count = 0;
    for (int i = 0; i < SCOPE_MAX_CHIPS; i++) {
        g_scope_slots[i].active = 0;
        g_scope_slots[i].chip_name = NULL;
        g_scope_slots[i].slot_base = 0;
    }
}

ScopeChipSlot* scope_find_slot_by_index(int index)
{
    if (index < 0 || index >= g_scope_chip_count) return NULL;
    if (!g_scope_slots[index].active) return NULL;
    return &g_scope_slots[index];
}
