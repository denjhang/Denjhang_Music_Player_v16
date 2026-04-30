/**
 * SN76489 (DCSG) Hardware Driver for SPFM Interface
 * Adapted from D:\working\vscode-projects\YM7129\sn76489_driver
 */
#ifndef SN76489_H
#define SN76489_H

#include <stdint.h>
#include <math.h>

#define SN76489_CLOCK_NTSC  3579545.0
#define SN76489_CLOCK_PAL   4000000.0

static inline uint8_t sn76489_tone_latch(uint8_t ch, uint8_t low4) {
    return 0x80 | ((ch & 3) << 5) | (low4 & 0x0F);
}

static inline uint8_t sn76489_tone_data(uint8_t high6) {
    return (high6 & 0x3F);
}

static inline uint8_t sn76489_vol_latch(uint8_t ch, uint8_t vol) {
    return 0x80 | ((ch & 3) << 5) | 0x10 | (vol & 0x0F);
}

static inline uint8_t sn76489_noise_vol_latch(uint8_t vol) {
    return 0xF0 | (vol & 0x0F);
}

static inline uint8_t sn76489_noise_latch(uint8_t ntype, uint8_t shift_freq) {
    return 0xE0 | ((ntype & 1) << 2) | (shift_freq & 0x03);
}

static inline uint16_t sn76489_note_to_period(double clock, double note) {
    double freq = 440.0 * pow(2.0, (note - 69.0) / 12.0);
    double period = clock / (32.0 * freq);
    if (period > 1023.0) period = 1023.0;
    if (period < 0.0) period = 0.0;
    return (uint16_t)(period + 0.5);
}

#endif /* SN76489_H */
