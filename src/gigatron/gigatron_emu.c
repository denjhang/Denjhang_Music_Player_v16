#include "gigatron_emu.h"
#include <string.h> // For memset
#include <stdlib.h> // For rand, srand
#include <stdint.h> // For uint8_t, uint16_t, uint32_t, int16_t
#include <time.h>   // For time

// 内部函数：初始化 soundTable
static void reset_sound_table(GigatronState *state) {
    // 使用当前时间作为随机数种子，确保每次运行的噪声不同
    srand((unsigned int)time(NULL));
    uint32_t r = (uint32_t)rand(); // 用于噪声生成，使用随机初始值
    for (int i = 0; i < 64; i++) {
        // Noise
        r += r * 56465321 + 456156321;
        state->soundTable[i * 4 + 0] = (uint8_t)(r & 63);
        // Triangle
        state->soundTable[i * 4 + 1] = (uint8_t)(i < 32 ? 2 * i : (127 - 2 * i));
        // Pulse
        state->soundTable[i * 4 + 2] = (uint8_t)(i < 32 ? 0 : 63);
        // Sawtooth
        state->soundTable[i * 4 + 3] = (uint8_t)i;
    }
}

// 重置自定义波形表（生成指定位深的默认波形）
void gigatron_emu_reset_custom_wave_table(GigatronState *state, uint8_t bits) {
    srand((unsigned int)time(NULL));
    uint32_t r = (uint32_t)rand();
    uint16_t maxVal = (bits == 6) ? 63 : (bits == 8) ? 255 : (bits == 12) ? 4095 : 65535;
    for (int i = 0; i < 64; i++) {
        // Noise
        r += r * 56465321 + 456156321;
        state->customWaveTable[i * 4 + 0] = (uint16_t)(r % (maxVal + 1));
        // Triangle
        if (i < 32)
            state->customWaveTable[i * 4 + 1] = (uint16_t)(2 * i * maxVal / 63);
        else
            state->customWaveTable[i * 4 + 1] = (uint16_t)((63 - 2 * (i - 32)) * maxVal / 63);
        // Pulse
        state->customWaveTable[i * 4 + 2] = (uint16_t)(i < 32 ? 0 : maxVal);
        // Sawtooth
        state->customWaveTable[i * 4 + 3] = (uint16_t)(i * maxVal / 63);
    }
    state->waveTableBits = bits;
}

// 初始化 Gigatron 仿真器
void gigatron_emu_init(GigatronState *state) {
    memset(state, 0, sizeof(GigatronState));
    reset_sound_table(state);
    // 初始化通道
    for (int i = 0; i < 4; i++) {
        state->ch[i].osc = 0;
        state->ch[i].key = 0;
        state->ch[i].wavX = 0;
        state->ch[i].wavA = 0;
    }
    state->samp = 0;
    state->scope_pos = 0;
    state->scope_skip_ctr = 0;
    // Initialize Ratio Counter for scanline updates
    // Gigatron frame rate is 59.98 Hz, and there are 521 scanlines per frame.
    // So, scanline frequency = 521 * 59.98 Hz.
    // We want to update the emulator 4 times per scanline (as per original code's `while (state->scanlineCounter >= 4.0)`).
    // So, the target update rate for the emulator's internal state is 521 * 59.98 * 4 Hz.
    // The audio sample rate is state->audioSampleRate.
    // We need to calculate how many internal emulator updates (4 per scanline) occur per audio sample.
    // Ratio = (521 * 59.98 * 4) / state->audioSampleRate
    // For RC_SET_RATIO(rc, mul, div), we need mul / div = (521 * 59.98 * 4) / state->audioSampleRate
    // Let's simplify: Gigatron's internal clock runs at 521 * 59.98 * 4 "ticks" per second.
    // We want to advance the emulator by 1 "tick" for every (audioSampleRate / (521 * 59.98 * 4)) audio samples.
    // Or, more simply, how many audio samples correspond to 4 scanlines?
    // The original code increments scanlineCounter by (double)state->audioSampleRate / state->bClock
    // where bClock = 521.0 * 59.98 * 2.
    // This means scanlineCounter is effectively tracking audio samples per 2 scanlines.
    // The loop condition `while (state->scanlineCounter >= 4.0)` means it processes 4 scanlines worth of audio samples.
    // So, 4 scanlines = 2 * bClock / audioSampleRate audio samples.
    // Let's use a simpler approach:
    // We want to advance the Gigatron emulator's internal state (which updates 4 times per scanline)
    // at a rate proportional to the audio sample rate.
    // Gigatron's internal "tick" rate (4 updates per scanline) = 521 * 59.98 * 4.
    // We need to map this to the audio sample rate.
    // RC_SET_RATIO(state->scanline_rc, Gigatron_Internal_Ticks_Per_Second, Audio_Sample_Rate)
    // Gigatron_Internal_Ticks_Per_Second = 521 * 59.98 * 4 = 124998.08
    // This is still floating point. Let's use the original bClock value.
    // The original code effectively advances scanlineCounter by `audioSampleRate / bClock` for each audio sample.
    // And it processes when `scanlineCounter >= 4.0`.
    // This means `4.0 * bClock / audioSampleRate` audio samples per Gigatron update.
    // Let's define a fixed ratio for the Gigatron's internal clock to the audio sample rate.
    // The Gigatron's internal clock runs at 521 * 59.98 * 4 "units" per second.
    // The audio system runs at `audioSampleRate` "units" per second.
    // So, for each audio sample, the Gigatron's internal clock should advance by `(521 * 59.98 * 4) / audioSampleRate`.
    // Let's use a fixed value for the Gigatron's internal clock frequency.
    // The original bClock is 521.0 * 59.98 * 2.
    // The original code effectively means 2 audio samples per bClock unit.
    // And it processes when scanlineCounter reaches 4.0.
    // This means 4.0 / (audioSampleRate / bClock) = 4.0 * bClock / audioSampleRate audio samples per update.
    // Let's define the Gigatron's internal "tick" rate as `GIGATRON_INTERNAL_TICK_RATE = 521 * 59.98 * 4`.
    // For each audio sample, we want to advance the Gigatron's internal tick counter by `GIGATRON_INTERNAL_TICK_RATE / audioSampleRate`.
    // So, `mul = GIGATRON_INTERNAL_TICK_RATE` and `div = audioSampleRate`.
    // GIGATRON_INTERNAL_TICK_RATE = 521 * 5998 * 4 / 100 = 12499808 / 100 = 124998.08
    // To avoid floating point, let's use a large integer for the Gigatron's internal clock.
    // Let's say Gigatron's internal clock runs at `GIGATRON_SCANLINE_FREQ * 4` "ticks" per second.
    // GIGATRON_SCANLINE_FREQ = 521 * 59.98 = 31250.08
    // So, `GIGATRON_INTERNAL_TICK_RATE = 31250.08 * 4 = 125000` (approx)
    // Let's use the exact values:
    // Gigatron's internal "tick" frequency = 521 * 5998 * 4 (scaled by 100)
    // Audio sample rate = state->audioSampleRate * 100
    // RC_SET_RATIO(&state->scanline_rc, 521 * 5998 * 4, state->audioSampleRate * 100);
    // This is equivalent to RC_SET_RATIO(&state->scanline_rc, 12499808, state->audioSampleRate * 100);
    // Or, more simply, the original code processes 4 scanlines at a time.
    // The number of audio samples per 4 scanlines is `4.0 * bClock / audioSampleRate`.
    // Let's define the Gigatron's internal "scanline ticks" as 4 per scanline.
    // So, `GIGATRON_SCANLINE_TICKS_PER_SECOND = 521 * 59.98 * 4`.
    // We want to advance `scanline_rc` by `GIGATRON_SCANLINE_TICKS_PER_SECOND / audioSampleRate` for each audio sample.
    // Let's use the original `bClock` value to derive the ratio.
    // `state->scanlineCounter` increments by `state->audioSampleRate / state->bClock` for each audio sample.
    // And it processes when `state->scanlineCounter >= 4.0`.
    // This means `4.0` units of `scanlineCounter` correspond to one Gigatron update cycle.
    // So, `mul = 4.0 * state->bClock` and `div = state->audioSampleRate`.
    // To avoid floating point, let's use integers.
    // `bClock` is `521 * 59.98 * 2`. Let's use `521 * 5998 * 2` and scale `audioSampleRate` by 100.
    // So, `mul = 4 * 521 * 5998 * 2` and `div = state->audioSampleRate * 100`.
    // `mul = 24999616`
    // `div = state->audioSampleRate * 100`
    // RC_SET_RATIO(&state->scanline_rc, 24999616, state->audioSampleRate * 100);
    // This seems too complex. Let's re-evaluate the original logic.
    // `state->scanlineCounter` represents "audio samples per bClock unit".
    // When `state->scanlineCounter` reaches `4.0`, it means `4.0 * bClock / audioSampleRate` audio samples have passed.
    // This is the number of audio samples that correspond to 4 "bClock units".
    // Let's define the Gigatron's internal "tick" as 1/4 of a scanline.
    // So, 4 ticks = 1 scanline.
    // Gigatron frame rate = 59.98 Hz. Scanlines per frame = 521.
    // Ticks per second = 59.98 * 521 * 4.
    // We want to advance the `scanline_rc` by `(59.98 * 521 * 4) / audioSampleRate` for each audio sample.
    // Let's use `mul = 5998 * 521 * 4` and `div = audioSampleRate * 100`.
    // `mul = 12499616`
    // `div = audioSampleRate * 100`
    state->audioSampleRate = 44100; // Default audio sample rate
    state->useCustomWaveTable = false;
    state->waveTableBits = 6;
    // MDSound bClock = 521.0 * 59.98 * 2 = 62499.08
    // To avoid floating point, scale by 100: 521 * 5998 * 2 = 6249908
    // RC_SET_RATIO(rc, mul, div) sets rc->inc = (mul << RC_SHIFT) / div
    // We want RC_GET_VAL to increment by (audioSampleRate / bClock)
    // So, mul = state->audioSampleRate * 100, div = 6249908
    // Gigatron's internal "tick" frequency = 521 * 60 * 4 = 125040 Hz
    // To avoid floating point, scale by 100: 521 * 6000 * 4 = 12504000
    // We want RC_GET_VAL to increment by (Gigatron_Internal_Ticks_Per_Second / Audio_Sample_Rate)
    // So, mul = state->audioSampleRate * 100, div = 6249908
    RC_SET_RATIO(&state->scanline_rc, state->audioSampleRate * 100, 6249908);
    RC_RESET(&state->scanline_rc); // Initialize the counter
    state->channelMask = 0xF; // Default to all channels active (0, 1, 2, 3)

    // 初始化新增的音频处理选项
    state->audio_bit_depth = 4; // 默认为 4 位音频
    state->dc_offset_removal_enabled = false;
    state->dc_bias = 0.0;
    state->dc_alpha = 0.99; // 根据 Gigatron_Audio_Emulation.md
    state->volume_scale = 0.5f; // 默认音量缩放，防止破音
}

// 写入 Gigatron 寄存器
void gigatron_emu_write_register(GigatronState *state, uint16_t adr, uint8_t data) {
    // 0x21: channelMask
    if (adr == 0x21) {
        state->channelMask = (uint8_t)(data & 0xF); // Allow control over all 4 channels
        return;
    }

    // 0x0700 - 0x07FF: soundTable 写入
    if ((adr & 0xFF00) == 0x0700) {
        state->soundTable[adr & 0xFF] = data;
        return;
    }

    // 通道寄存器写入
    uint8_t lo = adr & 0xFF;
    uint8_t hi = (adr >> 8) & 0xFF;

    // 过滤无效的高字节地址
    if (hi < 0x01 || hi > 0x04) return;
    
    uint8_t channel = hi - 1; // 1-4 映射到 0-3

    switch (lo) { // 使用 adr 的低字节作为寄存器地址
        case adrWavA: // 0xFA
            state->ch[channel].wavA = data;
            break;
        case adrWavX: // 0xFB
            state->ch[channel].wavX = data;
            break;
        case adrFnumL: // 0xFC
            state->ch[channel].key = (state->ch[channel].key & 0xFF80) | (data & 0x7F); // 7位
            break;
        case adrFnumH: // 0xFD
            state->ch[channel].key = (state->ch[channel].key & 0x007F) | ((uint16_t)(data & 0x7F) << 7); // 7位
            state->ch[channel].key *= 4; // 将频率步进值加倍，提高一个八度
            break;
        case 0xFE: // osc low (MDSound uses 0xFE for osc low)
            state->ch[channel].osc = (state->ch[channel].osc & 0xFF80) | (data & 0x7F); // 7位
            break;
        case 0xFF: // osc high (MDSound uses 0xFF for osc high)
            state->ch[channel].osc = (state->ch[channel].osc & 0x007F) | ((uint16_t)(data & 0x7F) << 7); // 7位
            break;
        default:
            // 未知寄存器，可以忽略或记录错误
            break;
    }
}

// 更新 Gigatron 仿真器并生成音频样本
void gigatron_emu_update(GigatronState *state, int16_t *output_buffer, int num_samples) {
    uint16_t maxVal = (state->waveTableBits == 6) ? 63 :
                      (state->waveTableBits == 8) ? 255 :
                      (state->waveTableBits == 12) ? 4095 : 65535;

    for (int p = 0; p < num_samples; p++) {
        RC_STEP(&state->scanline_rc);

        while (RC_GET_VAL(&state->scanline_rc) >= 4) { // Process 4 internal ticks (equivalent to 1 scanline)
            state->samp = 3; // 初始样本值，MDSound 中是 3

            for (int n = 0; n < 4; n++) { // 遍历所有通道
                // 只有当通道被 channelMask 启用时才更新
                if ((state->channelMask >> n) & 1) {
                    state->ch[n].osc += state->ch[n].key; // 振荡器相位累加频率步进值

                    uint8_t i_idx = (uint8_t)((state->ch[n].osc >> 7) & 0xfc); // 获取 soundTable 索引
                    i_idx ^= state->ch[n].wavX; // 与 wavX 异或

                    if (state->useCustomWaveTable) {
                        // 自定义高精度波表路径
                        // 模拟原始 6-bit 行为: (soundTable[i] + wavA) & 0x3F
                        // 即 (wave_val + wavA) mod (maxVal+1)，取低 N 位
                        uint16_t wave_val = state->customWaveTable[i_idx];
                        int32_t total = (int32_t)wave_val + (int32_t)state->ch[n].wavA;
                        int32_t range = (int32_t)maxVal + 1;
                        int32_t sample = total % range;
                        if (sample < 0) sample += range;
                        state->samp += sample;
                    } else {
                        // 原始 6-bit 波表路径
                        int32_t temp_val = state->soundTable[i_idx] + state->ch[n].wavA;
                        uint8_t sample_val;
                        if (temp_val & 0x80) {
                            sample_val = 63;
                        } else {
                            sample_val = temp_val & 0x3F;
                        }
                        state->samp += sample_val;
                    }
                }
            }

            double processed_sample;

            if (state->useCustomWaveTable) {
                // 自定义表：累加值范围更大，归一化到 0.0-1.0
                int32_t accMax = (int32_t)maxVal * 4 + 3; // 4通道 + 初始值3
                double normalized = (double)state->samp / (double)accMax;
                if (normalized < 0.0) normalized = 0.0;
                if (normalized > 1.0) normalized = 1.0;

                // 自定义表统一走归一化 → int16 路径
                double pcm = (normalized - 0.5) * 2.0 * 32767.0 * state->volume_scale;
                if (state->dc_offset_removal_enabled) {
                    state->dc_bias = (state->dc_alpha * state->dc_bias) + ((1.0 - state->dc_alpha) * pcm);
                    pcm = pcm - state->dc_bias;
                }
                if (pcm > 32767.0) pcm = 32767.0;
                if (pcm < -32768.0) pcm = -32768.0;
                state->samp = (int32_t)pcm;
                RC_VAL_SUB(&state->scanline_rc, 4);
                continue; // 跳过原始路径的 PCM 转换
            } else {
                // 原始路径
                switch (state->audio_bit_depth) {
                    case 4:
                        processed_sample = (double)(state->samp & 0xF0);
                        break;
                    case 6:
                        processed_sample = (double)(state->samp & 0xFC);
                        break;
                    case 8:
                        {
                            int32_t v = state->samp;
                            if (v > 255) v = 255;
                            if (v < 0) v = 0;
                            processed_sample = (double)v;
                        }
                        break;
                    case 12:
                        {
                            int32_t v = state->samp;
                            if (v > 255) v = 255;
                            if (v < 0) v = 0;
                            processed_sample = (double)(v * 4095 / 255);
                        }
                        break;
                    case 16:
                    default:
                        processed_sample = (double)state->samp + 3.0;
                        break;
                }
            }

            if (state->dc_offset_removal_enabled) {
                state->dc_bias = (state->dc_alpha * state->dc_bias) + ((1.0 - state->dc_alpha) * processed_sample);
                processed_sample = processed_sample - state->dc_bias;
            }

            // 转换为 16 位有符号 PCM，应用音量缩放和防破音
            double pcm = (processed_sample - 128.0) * 256.0 * state->volume_scale;
            if (pcm > 32767.0) pcm = 32767.0;
            if (pcm < -32768.0) pcm = -32768.0;
            state->samp = (int32_t)pcm;

            RC_VAL_SUB(&state->scanline_rc, 4);
        }

        // 将样本写入立体声缓冲区
        int16_t out = (int16_t)state->samp;
        output_buffer[p * 2] = out;     // 左声道
        output_buffer[p * 2 + 1] = out; // 右声道

        // 示波器波形捕获：每 4 个样本写入一次（降采样到 ~11025Hz）
        state->scope_skip_ctr++;
        if (state->scope_skip_ctr >= 4) {
            state->scope_skip_ctr = 0;
            int pos = state->scope_pos;
            for (int n = 0; n < 4; n++) {
                uint8_t i_idx = (uint8_t)((state->ch[n].osc >> 7) & 0xfc);
                i_idx ^= state->ch[n].wavX;
                if (state->useCustomWaveTable) {
                    uint16_t wave_val = state->customWaveTable[i_idx];
                    int32_t total = (int32_t)wave_val + (int32_t)state->ch[n].wavA;
                    int32_t range = (int32_t)maxVal + 1;
                    int32_t sv = total % range;
                    if (sv < 0) sv += range;
                    state->scope_buf[n][pos] = (int16_t)(((int32_t)sv - (int32_t)(maxVal / 2)) * (32767 / (maxVal > 0 ? maxVal : 1)));
                } else {
                    int32_t tv = state->soundTable[i_idx] + state->ch[n].wavA;
                    uint8_t sv;
                    if (tv & 0x80) sv = 63;
                    else sv = tv & 0x3F;
                    state->scope_buf[n][pos] = (int16_t)((sv - 32) * 1024);
                }
            }
            state->scope_pos = (pos + 1) % GT_SCOPE_BUF_SIZE;
        }
    }
}