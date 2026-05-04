// modizer_viz.cpp - Per-channel oscilloscope visualization
// Based on imgui-vgmplayer's modizer_viz implementation.
// Reads per-voice ring buffers written by modified libvgm chip cores.
//
// Cross-correlation algorithm modeled after original Modizer (RenderUtils.mm):
//   - Persistent per-channel offset (m_voice_ofs[]) survives across frames
//   - Search is centered on previous best offset, expanding left+right
//   - Ring buffer wraparound handled via & (BUFFER_LEN - 1)

#include "modizer_viz.h"
#include "libvgm-modizer/emu/cores/ModizerVoicesData.h"
#include "imgui/imgui.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// Global variable definitions for the modizer voice buffer system.
// These are declared extern in ModizerVoicesData.h and referenced by
// the modified chip emulator cores (fmopn.c, ym2151.c, etc.).

signed char *m_voice_buff[SOUND_MAXVOICES_BUFFER_FX] = {nullptr};
int m_voice_buff_adjustement = 0;
int m_voice_fadeout_factor = 0;
int64_t mdz_ratio_fp_cnt = 0, mdz_ratio_fp_inc = 0, mdz_ratio_fp_inv_inc = 0;
double mdz_pbratio = 0.0;
unsigned char m_voice_channel_mapping[256] = {0};
unsigned char m_channel_voice_mapping[256] = {0};
int m_genNumVoicesChannels = 0, m_genNumMidiVoicesChannels = 0;
unsigned int vgm_last_vol[SOUND_MAXVOICES_BUFFER_FX] = {0};
unsigned int vgm_last_note[SOUND_MAXVOICES_BUFFER_FX] = {0};
unsigned char vgm_last_instr[SOUND_MAXVOICES_BUFFER_FX] = {0};
int64_t m_voice_current_ptr[SOUND_MAXVOICES_BUFFER_FX] = {0};
int64_t m_voice_prev_current_ptr[SOUND_MAXVOICES_BUFFER_FX] = {0};
int m_voice_ChipID[SOUND_MAXVOICES_BUFFER_FX] = {0};
int m_voice_systemColor[SOUND_VOICES_MAX_ACTIVE_CHIPS] = {0};
int m_voice_voiceColor[SOUND_MAXVOICES_BUFFER_FX] = {0};
char vgmVRC7 = 0, vgm2610b = 0;
int HC_voicesMuteMask1 = 0, HC_voicesMuteMask2 = 0;
int64_t generic_mute_mask = 0;
signed char m_voice_current_system = 0, m_voice_current_systemSub = 0;
int m_voice_current_samplerate = 0;
int g_ssg_scope_shift = 6;  // SSG scope bit shift (0-10, default 6)
double m_voice_current_rateratio = 0.0;
char m_voice_current_systemPairedOfs = 0;
char m_voice_current_total = 0;
char m_voicesStatus[SOUND_MAXMOD_CHANNELS] = {0};
int m_voicesForceOfs = 0;

const int ModizerViz::BUFFER_LEN;

ModizerViz::ModizerViz() : m_voice_prev_buff(nullptr)
{
    memset(m_voice_ofs, 0, sizeof(m_voice_ofs));
}

ModizerViz::~ModizerViz()
{
    for (int i = 0; i < SOUND_MAXVOICES_BUFFER_FX; ++i)
    {
        if (m_voice_buff[i]) {
            delete[] m_voice_buff[i];
            m_voice_buff[i] = nullptr;
        }
        if (m_voice_prev_buff && m_voice_prev_buff[i]) {
            delete[] m_voice_prev_buff[i];
            m_voice_prev_buff[i] = nullptr;
        }
    }
    if (m_voice_prev_buff) {
        delete[] m_voice_prev_buff;
        m_voice_prev_buff = nullptr;
    }
}

void ModizerViz::Init()
{
    // Free existing buffers
    for (int i = 0; i < SOUND_MAXVOICES_BUFFER_FX; ++i)
    {
        if (m_voice_buff[i]) {
            delete[] m_voice_buff[i];
            m_voice_buff[i] = nullptr;
        }
        if (m_voice_prev_buff && m_voice_prev_buff[i]) {
            delete[] m_voice_prev_buff[i];
            m_voice_prev_buff[i] = nullptr;
        }
    }
    if (m_voice_prev_buff) {
        delete[] m_voice_prev_buff;
        m_voice_prev_buff = nullptr;
    }

    // Allocate per-voice ring buffers
    m_voice_prev_buff = new signed char*[SOUND_MAXVOICES_BUFFER_FX];
    for (int i = 0; i < SOUND_MAXVOICES_BUFFER_FX; ++i)
    {
        m_voice_buff[i] = new signed char[BUFFER_LEN];
        memset(m_voice_buff[i], 0, BUFFER_LEN);

        // Allocate prev buffer at max size (BUFFER_LEN) for dynamic samples
        m_voice_prev_buff[i] = new signed char[BUFFER_LEN];
        memset(m_voice_prev_buff[i], 0, BUFFER_LEN);

        m_voice_current_ptr[i] = 0;
    }

    // Reset persistent offsets
    memset(m_voice_ofs, 0, sizeof(m_voice_ofs));
    memset(m_voice_prev_write_ptr, 0, sizeof(m_voice_prev_write_ptr));
}

void ModizerViz::ResetOffsets()
{
    memset(m_voice_ofs, 0, sizeof(m_voice_ofs));
    memset(m_voice_prev_write_ptr, 0, sizeof(m_voice_prev_write_ptr));
}

// Helper: read one sample from ring buffer with wraparound
static inline signed char ring_read(const signed char* buf, int idx, int mask)
{
    return buf[idx & mask];
}

void ModizerViz::DrawChannel(int channel, ImDrawList* draw_list, float x, float y, float width, float height,
                              float amplitude_multiplier, unsigned int border_color, bool keyon, float level,
                              int samples, int offset,
                              int search_window, bool edge_align, bool fix_sample_read, int ac_mode,
                              bool legacy_mode)
{
    if (width < 1.0f) return;
    if (samples < 2) samples = 2;

    // search_window<0 clamp to 0
    if (search_window < 0) search_window = 0;

    // Clamp samples to buffer capacity (leave room for search)
    int max_samples = BUFFER_LEN - 64;  // conservative reserve
    if (max_samples < 2) max_samples = 2;
    if (samples > max_samples) samples = max_samples;

    const ImU32 col_bg   = IM_COL32(20, 20, 20, 255);

    // Dynamic border/zero color: blend channel color by level when keyon,
    // dimmed gray when idle — same approach as piano keyboard borders
    ImU32 col_border;
    ImU32 col_zero;
    const ImU32 col_line = IM_COL32(100, 255, 100, 255);  // waveform: always green
    if (keyon && border_color) {
        // Active: blend channel color by level [0.4..1.0]
        float dimF = 0.4f + std::min(1.0f, level) * 0.6f;
        int r = (int)(((border_color >> 0) & 0xFF) * dimF);
        int g = (int)(((border_color >> 8) & 0xFF) * dimF);
        int b = (int)(((border_color >> 16) & 0xFF) * dimF);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        col_border = IM_COL32(r, g, b, 255);
        col_zero   = IM_COL32(r, g, b, 180);  // zero line slightly transparent
    } else {
        // Idle: dimmed gray
        col_border = IM_COL32(80, 80, 100, 255);
        col_zero   = IM_COL32(60, 60, 70, 255);
    }

    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), col_bg);
    // Thick border (2px) — drawn as 4 lines since AddRect only supports 1px
    draw_list->AddRect(ImVec2(x, y), ImVec2(x + width, y + height), col_border, 0.0f, 0, 2.0f);

    // Zero line
    float zero_y = y + height / 2.0f;
    draw_list->AddLine(ImVec2(x, zero_y), ImVec2(x + width, zero_y), col_zero);

    // Return early if no valid channel (no waveform data)
    if (channel < 0 || channel >= SOUND_MAXVOICES_BUFFER_FX) return;
    if (!m_voice_buff[channel]) return;

    // Compute write position in ring buffer
    int64_t write_ptr = m_voice_current_ptr[channel];
    if (fix_sample_read) {
        write_ptr >>= 16;  // convert 16.16 fixed-point to integer
    }
    const int RING_MASK = BUFFER_LEN - 1;
    int ring_pos = (int)(write_ptr & RING_MASK);

    // --- Cross-correlation trigger ---
    int best_offset = 0;

    // ===== New mode: persistent offset with bidirectional search =====
    // Modeled after original Modizer: search centered on persistent offset,
    // expanding left and right simultaneously.
    best_offset = m_voice_ofs[channel];

    if (search_window > 0 && write_ptr > samples) {
        // Ensure persistent offset is in valid range
        // If offset is far from current write position, re-anchor it
        int write_back = (ring_pos - samples + BUFFER_LEN) & RING_MASK;
        // If best_offset hasn't been set or is stale, initialize to just before write position
        if (best_offset < 0 || best_offset >= BUFFER_LEN) {
            best_offset = write_back;
        }

        static const int CORR_STEP = 4;
        long max_correlation = -1;

        // Search range: [best_offset - search_window/2, best_offset + search_window/2]
        // Both directions (like original Modizer's ofs1/ofs2)
        int search_lo = best_offset - search_window / 2;
        int search_hi = best_offset + search_window / 2;

        // Bidirectional search from center outward
        int ofs_right = best_offset;
        int ofs_left = best_offset - CORR_STEP;
        bool right_done = false;
        bool left_done = false;

        for (;;) {
            // Right direction
            if (!right_done && ofs_right <= search_hi) {
                long corr = 0;
                for (int i = 0; i < samples; i += CORR_STEP) {
                    corr += (long)ring_read(m_voice_buff[channel], ofs_right + i, RING_MASK)
                          * m_voice_prev_buff[channel][i];
                }
                if (corr > max_correlation) {
                    max_correlation = corr;
                    best_offset = ofs_right;
                }
                ofs_right += CORR_STEP;
            } else {
                right_done = true;
            }

            // Left direction
            if (!left_done && ofs_left >= search_lo) {
                long corr = 0;
                for (int i = 0; i < samples; i += CORR_STEP) {
                    corr += (long)ring_read(m_voice_buff[channel], ofs_left + i, RING_MASK)
                          * m_voice_prev_buff[channel][i];
                }
                if (corr > max_correlation) {
                    max_correlation = corr;
                    best_offset = ofs_left;
                }
                ofs_left -= CORR_STEP;
            } else {
                left_done = true;
            }

            if (right_done && left_done) break;
        }

        // Edge-align refinement: snap to nearest rising edge near the correlation peak
        if (edge_align) {
            int refine_lo = best_offset - 32;
            int refine_hi = best_offset + 32;

            int best_edge = best_offset;
            int best_edge_dist = 999;
            for (int pos = refine_lo; pos <= refine_hi; pos++) {
                signed char cur  = ring_read(m_voice_buff[channel], pos, RING_MASK);
                signed char prev = ring_read(m_voice_buff[channel], pos - 1, RING_MASK);
                if (prev < 0 && cur >= 0) {
                    int dist = (pos >= best_offset) ? (pos - best_offset) : (best_offset - pos);
                    if (dist < best_edge_dist) {
                        best_edge_dist = dist;
                        best_edge = pos;
                    }
                }
            }
            best_offset = best_edge;
        }
    }

    // Persist offset for next frame
    m_voice_ofs[channel] = best_offset;

    // ===== Clear on Silence check (after trigger, before drawing) =====
    // Detect if no new data has been written since last frame.
    // If silent, return early to show empty scope box.
    if (legacy_mode) {
        if (write_ptr == m_voice_prev_write_ptr[channel]) {
            // No new data since last frame - silence detected
            m_voice_prev_write_ptr[channel] = write_ptr;
            return;  // Just show the empty box with zero line
        }
        m_voice_prev_write_ptr[channel] = write_ptr;
    }

    // Apply user offset
    if (offset < 0) offset = 0;
    int draw_start = (best_offset + offset) & RING_MASK;

    // --- Read waveform into linear buffer (ring-safe) ---
    signed char draw_buff[BUFFER_LEN];
    for (int i = 0; i < samples; i++) {
        draw_buff[i] = ring_read(m_voice_buff[channel], draw_start + i, RING_MASK);
    }

    // --- AC coupling for unipolar waveforms ---
    // ac_mode: 0=off, 1=center (remove DC), 2=bottom (map min..max to full range)
    signed char ac_buff[BUFFER_LEN];
    const signed char* draw_src = draw_buff;
    if (ac_mode >= 1) {
        bool unipolar = true;
        int min_val = 127, max_val = -128;
        for (int i = 0; i < samples; i++) {
            signed char v = draw_buff[i];
            if (v < min_val) min_val = v;
            if (v > max_val) max_val = v;
            if (v < -2) { unipolar = false; break; }
        }
        if (unipolar && (max_val - min_val) > 4) {
            if (ac_mode == 1) {
                // Center: subtract DC offset
                int dc = (min_val + max_val) / 2;
                for (int i = 0; i < samples; i++) {
                    int v = (int)draw_buff[i] - dc;
                    if (v > 127) v = 127;
                    if (v < -128) v = -128;
                    ac_buff[i] = (signed char)v;
                }
            } else {
                // Bottom: map min..max → -128..+127 (waveform starts from bottom)
                int range = max_val - min_val;
                if (range < 1) range = 1;
                for (int i = 0; i < samples; i++) {
                    int v = (int)(draw_buff[i] - min_val) * 255 / range - 128;
                    if (v > 127) v = 127;
                    if (v < -128) v = -128;
                    ac_buff[i] = (signed char)v;
                }
            }
            draw_src = ac_buff;
        }
    }

    // --- Draw waveform ---
    ImVec2 prev_p;
    for (int i = 0; i < samples; ++i)
    {
        float sample_val = ((float)draw_src[i] / 128.0f) * amplitude_multiplier;
        sample_val = std::min(1.0f, std::max(-1.0f, sample_val));

        ImVec2 p;
        p.x = x + (float)i / (samples - 1) * width;
        p.y = zero_y - sample_val * (height / 2.0f);

        if (i > 0)
        {
            draw_list->AddLine(prev_p, p, col_line, 1.0f);
        }
        prev_p = p;
    }

    // Store current waveform for next frame correlation
    memcpy(m_voice_prev_buff[channel], draw_src, samples);
}
