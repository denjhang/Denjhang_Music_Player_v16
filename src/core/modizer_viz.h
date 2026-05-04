#ifndef MODIZER_VIZ_H
#define MODIZER_VIZ_H

#include "libvgm-modizer/emu/cores/ModizerConstants.h"
#include <cstdint>

struct ImDrawList;

class ModizerViz
{
public:
    ModizerViz();
    ~ModizerViz();

    void Init();
    // border_color: channel base color (e.g. IM_COL32(80,200,80,255))
    // keyon/level:  when keyon=true, border and zero line are tinted by level [0,1]
    //               when keyon=false, uses dimmed gray (like piano keyoff)
    // samples:      number of samples to display (time domain)
    // offset:       sample offset into buffer (shifts waveform left/right)
    void DrawChannel(int channel, ImDrawList* draw_list, float x, float y, float width, float height,
                     float amplitude_multiplier = 3.0f, unsigned int border_color = 0,
                     bool keyon = false, float level = 0.0f,
                     int samples = 441, int offset = 0,
                     int search_window = 0,    // 0 = skip correlation (raw buffer)
                     bool edge_align = true,     // true = snap to rising edge
                     bool fix_sample_read = true, // true = right-shift fixed-point
                     int ac_mode = 1,            // 0=off, 1=center, 2=bottom
                     bool legacy_mode = false);   // true = clear waveform on silence (for PCM)

    void ResetOffsets();  // call on file load/unload

private:
    signed char** m_voice_prev_buff;
    int m_voice_ofs[SOUND_MAXVOICES_BUFFER_FX];  // persistent offset per channel (cross-frame)
    int64_t m_voice_prev_write_ptr[SOUND_MAXVOICES_BUFFER_FX];  // previous frame's write ptr (for legacy mode silence detection)
    static const int BUFFER_LEN = 4096;
};

#endif // MODIZER_VIZ_H
