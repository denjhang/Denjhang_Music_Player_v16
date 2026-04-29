// ay8910_scope.c - Override ay8910_update_one from libvgm-emu.a
// Adds per-channel scope buffer writes for SSG (AY8910/YM2149) channels.
// Linked via --whole-archive to override the prebuilt symbol.

#include "ModizerVoicesData.h"
#include <string.h>
#include <stddef.h>

// ---- Type definitions matching ay8910.c ----
typedef signed int  INT32;
typedef unsigned int UINT32;
typedef unsigned char UINT8;
typedef signed short DEV_SMPL;
typedef signed char INT8;

#define NUM_CHANNELS 3

// Avoid redefinition - LIMIT8 is already in ModizerConstants.h
#undef LIMIT8
#define LIMIT8(a) ((a)>127?127:((a)<-128?-128:(a)))

// Minimal ay8910 context layout - matches the real struct in ay8910.c.
// We only access the fields used in update_one for scope writes.
typedef struct {
    char _pad0[8];          // _devData (void*)
    char _pad1[8];          // logger
    int   type;
    UINT8 streams;
    UINT8 ioports;
    UINT8 active;
    UINT8 register_latch;
    UINT8 regs[16];
    UINT8 last_enable;
    INT32 count[NUM_CHANNELS];
    UINT8 output[NUM_CHANNELS];
    UINT8 prescale_noise;
    INT32 count_noise;
    INT32 count_env;
    INT8  env_step;
    UINT32 env_volume;
    UINT8 hold, alternate, attack, holding;
    INT32 rng;
    UINT8 env_step_mask;
    int   step;
    UINT8 zero_is_off;
    UINT8 vol_enabled[NUM_CHANNELS];
    const void* par;
    const void* par_env;
    INT32 vol_table[NUM_CHANNELS][16];
    INT32 env_table[NUM_CHANNELS][32];
    int   flags;
    int   res_load[3];
    UINT8 StereoMask[NUM_CHANNELS];
    UINT32 MuteMsk[NUM_CHANNELS];
    UINT32 clock;
    UINT8 chip_type;
    UINT8 chip_flags;
    void* SmpRateFunc;
    void* SmpRateData;
} ay8910_ctx;

#define TONE_VOLUME(p, ch)  ((p)->regs[0x08 + (ch)] & 0x0f)
#define TONE_ENABLEQ(p, ch) (!(((p)->regs[0x07] >> (ch)) & 0x01))
#define NOISE_ENABLEQ(p, ch) (!(((p)->regs[0x07] >> (3 + (ch))) & 0x01))
#define TONE_ENVELOPE(p, ch) ((p)->regs[0x0d] & (1 << (ch)))
#define NOISE_OUTPUT(p) (((p)->rng & 1) | ((p)->prescale_noise << 1))
#define TONE_PERIOD(p, ch)  ((p)->regs[((ch) << 1)] | ((INT32)((p)->regs[((ch) << 1) + 1]) << 8))
#define NOISE_PERIOD(p)    ((p)->regs[0x06] | ((INT32)((p)->regs[0x07]) << 8))
#define ENVELOPE_PERIOD(p) ((p)->regs[0x0b] | ((INT32)((p)->regs[0x0c]) << 8))

#define LIMIT8(a) ((a)>127?127:((a)<-128?-128:(a)))

void ay8910_update_one(void *param, UINT32 samples, DEV_SMPL **outputs)
{
    ay8910_ctx *psg = (ay8910_ctx *)param;
    int chan;
    UINT32 cur_smpl;
    DEV_SMPL *bufL = outputs[0];
    DEV_SMPL *bufR = outputs[1];
    DEV_SMPL chnout;

    memset(outputs[0], 0x00, samples * sizeof(DEV_SMPL));
    memset(outputs[1], 0x00, samples * sizeof(DEV_SMPL));

    //TODO:  MODIZER changes start / YOYOFR
    ScopeChipSlot *scope_slot = scope_register_chip("SSG", 0, 3, m_voice_current_samplerate);
    if (scope_slot && scope_slot->num_groups == 0) {
        scope_slot->groups[0] = (ScopeChGroup){SCOPE_CH_SSG, 3, 0, "S"};
        scope_slot->num_groups = 1;
    }
    int m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
    if (!m_voice_current_samplerate) m_voice_current_samplerate = 44100;
    int64_t smplIncr = (int64_t)44100 * (1 << MODIZER_OSCILLO_OFFSET_FIXEDPOINT) / m_voice_current_samplerate;
    //TODO:  MODIZER changes end / YOYOFR

    for (cur_smpl = 0; cur_smpl < samples; cur_smpl++)
    {
        for (chan = 0; chan < NUM_CHANNELS; chan++)
        {
            psg->count[chan]++;
            if (psg->count[chan] >= TONE_PERIOD(psg, chan))
            {
                psg->output[chan] ^= 1;
                psg->count[chan] = 0;
            }
        }

        psg->count_noise++;
        if (psg->count_noise >= NOISE_PERIOD(psg))
        {
            psg->count_noise = 0;
            psg->prescale_noise ^= 1;
            if (psg->prescale_noise)
            {
                psg->rng ^= (((psg->rng & 1) ^ ((psg->rng >> 3) & 1)) << 17);
                psg->rng >>= 1;
            }
        }

        for (chan = 0; chan < NUM_CHANNELS; chan++)
        {
            psg->vol_enabled[chan] = (psg->output[chan] | TONE_ENABLEQ(psg, chan)) & (NOISE_OUTPUT(psg) | NOISE_ENABLEQ(psg, chan));
        }

        if (psg->holding == 0)
        {
            psg->count_env++;
            if (psg->count_env >= ENVELOPE_PERIOD(psg) * psg->step)
            {
                psg->count_env = 0;
                psg->env_step--;
                if (psg->env_step < 0)
                {
                    if (psg->hold)
                    {
                        if (psg->alternate)
                            psg->attack ^= psg->env_step_mask;
                        psg->holding = 1;
                        psg->env_step = 0;
                    }
                    else
                    {
                        if (psg->alternate && (psg->env_step & (psg->env_step_mask + 1)))
                            psg->attack ^= psg->env_step_mask;
                        psg->env_step &= psg->env_step_mask;
                    }
                }
            }
        }
        psg->env_volume = (psg->env_step ^ psg->attack);

        for (chan = 0; chan < NUM_CHANNELS; chan++)
        {
            if (!psg->MuteMsk[chan])
                continue;
            if (TONE_ENVELOPE(psg, chan) != 0)
            {
                chnout = psg->env_table[chan][psg->vol_enabled[chan] ? psg->env_volume : 0];
            }
            else
            {
                chnout = psg->vol_table[chan][psg->vol_enabled[chan] ? TONE_VOLUME(psg, chan) : 0];
            }
            if (psg->StereoMask[chan] & 0x01)
                bufL[cur_smpl] += chnout;
            if (psg->StereoMask[chan] & 0x02)
                bufR[cur_smpl] += chnout;

            //TODO:  MODIZER changes start / YOYOFR
            if (m_voice_ofs >= 0)
            {
                int64_t ofs = m_voice_current_ptr[m_voice_ofs + chan];
                int idx = (ofs >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT) & (SOUND_BUFFER_SIZE_SAMPLE * 4 * 2 - 1);
                m_voice_buff[m_voice_ofs + chan][idx] = LIMIT8(chnout >> 8);
            }
            //TODO:  MODIZER changes end / YOYOFR
        }
    }

    //TODO:  MODIZER changes start / YOYOFR
    if (m_voice_ofs >= 0)
    {
        for (int jj = 0; jj < 3; jj++)
            m_voice_current_ptr[m_voice_ofs + jj] += smplIncr;
    }
    //TODO:  MODIZER changes end / YOYOFR
}
