/**
 * SPFM Light interface for SN76489 hardware control.
 * Adapted from D:\working\vscode-projects\YM7129\sn76489_driver\spfm_lite.c
 */
#include "spfm.h"
#include "ftd2xx.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

static FT_HANDLE s_handle = NULL;
static uint8_t   s_buf[SPFM_LITE_BUF_SIZE];
static DWORD     s_buf_ptr = 0;

static void spfm_sleep_ms(unsigned int ms) { Sleep(ms); }

bool spfm_flush(void) {
    if (!s_handle || s_buf_ptr == 0) return true;
    DWORD total = 0;
    while (total < s_buf_ptr) {
        DWORD chunk = s_buf_ptr - total;
        if (chunk > 4096) chunk = 4096;
        DWORD written = 0;
        FT_STATUS st = FT_Write(s_handle, s_buf + total, chunk, &written);
        if (st != FT_OK || written != chunk) {
            s_buf_ptr = 0;
            return false;
        }
        total += written;
    }
    s_buf_ptr = 0;
    return true;
}

void spfm_write_reg(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data) {
    if (!s_handle) return;
    uint8_t cmd[4];
    cmd[0] = slot & 1;
    cmd[1] = (port & 7) << 1;
    cmd[2] = addr;
    cmd[3] = data;
    if (s_buf_ptr + 4 > SPFM_LITE_BUF_SIZE) spfm_flush();
    memcpy(s_buf + s_buf_ptr, cmd, 4);
    s_buf_ptr += 4;
}

void spfm_hw_wait(uint32_t samples) {
    for (uint32_t i = 0; i < samples; i++) {
        if (s_buf_ptr + 1 > SPFM_LITE_BUF_SIZE) spfm_flush();
        s_buf[s_buf_ptr++] = 0x80;
    }
}

void spfm_wait_and_write_reg(uint32_t wait_samples, uint8_t slot, uint8_t port, uint8_t addr, uint8_t data) {
    const uint32_t HW_WAIT_THRESHOLD = 10;
    if (wait_samples > 0) {
        if (wait_samples < HW_WAIT_THRESHOLD) {
            spfm_hw_wait(wait_samples);
        } else {
            spfm_flush();
            unsigned int us = (unsigned int)((uint64_t)wait_samples * 1000000 / 44100);
            if (us >= 1000)
                Sleep(us / 1000);
            else {
                LARGE_INTEGER freq, start, now;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&start);
                double ticks = (double)us * freq.QuadPart / 1e6;
                do { QueryPerformanceCounter(&now); }
                while ((double)(now.QuadPart - start.QuadPart) < ticks);
            }
        }
    }
    spfm_write_reg(slot, port, addr, data);
}

void spfm_write_regs(uint8_t slot, const spfm_reg_t* regs, uint32_t count, uint32_t write_wait) {
    for (uint32_t i = 0; i < count; i++) {
        spfm_write_reg(slot, regs[i].port, regs[i].addr, regs[i].data);
        for (uint32_t j = 0; j < write_wait; j++) {
            if (s_buf_ptr + 1 > SPFM_LITE_BUF_SIZE) spfm_flush();
            s_buf[s_buf_ptr++] = 0x80;
        }
    }
}

void spfm_write_data(uint8_t slot, uint8_t data) {
    if (!s_handle) return;
    if (s_buf_ptr + 3 > SPFM_LITE_BUF_SIZE) spfm_flush();
    s_buf[s_buf_ptr++] = slot & 1;
    s_buf[s_buf_ptr++] = 0x20;
    s_buf[s_buf_ptr++] = data;
}

int spfm_init(int dev_idx) {
    FT_STATUS st;
    s_buf_ptr = 0;

    st = FT_Open(dev_idx, &s_handle);
    if (st != FT_OK) return -1;

    FT_SetBaudRate(s_handle, 1500000);
    FT_SetDataCharacteristics(s_handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(s_handle, FT_FLOW_NONE, 0, 0);
    FT_SetTimeouts(s_handle, 100, 100);
    FT_SetLatencyTimer(s_handle, 2);
    FT_SetUSBParameters(s_handle, 65536, 65536);
    FT_Purge(s_handle, FT_PURGE_RX | FT_PURGE_TX);

    FT_SetDtr(s_handle); FT_SetRts(s_handle);
    spfm_sleep_ms(1);
    FT_ClrDtr(s_handle); FT_ClrRts(s_handle);
    spfm_sleep_ms(1);
    FT_SetRts(s_handle);
    spfm_sleep_ms(1);
    FT_ClrDtr(s_handle); FT_ClrRts(s_handle);
    spfm_sleep_ms(10);

    uint8_t reset_cmd = 0xFE;
    DWORD written = 0;
    FT_Write(s_handle, &reset_cmd, 1, &written);
    spfm_sleep_ms(100);
    FT_Purge(s_handle, FT_PURGE_RX | FT_PURGE_TX);

    return 0;
}

void spfm_cleanup(void) {
    if (!s_handle) return;
    spfm_flush();
    uint8_t reset_cmd = 0xFE;
    DWORD written = 0;
    FT_Write(s_handle, &reset_cmd, 1, &written);
    spfm_sleep_ms(50);
    FT_Close(s_handle);
    s_handle = NULL;
}

void spfm_reset(void) {
    if (!s_handle) return;
    spfm_flush();
    uint8_t reset_cmd = 0xFE;
    DWORD written = 0;
    FT_Write(s_handle, &reset_cmd, 1, &written);
    spfm_sleep_ms(50);
}

void spfm_chip_reset(void) { spfm_reset(); }

SPFM_HANDLE spfm_get_handle(void) { return s_handle; }

void spfm_set_handle(SPFM_HANDLE h) {
    s_handle = h;
    s_buf_ptr = 0;
}
