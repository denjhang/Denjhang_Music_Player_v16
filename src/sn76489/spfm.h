/**
 * SPFM Light Interface for SN76489 hardware control
 * Adapted from D:\working\vscode-projects\YM7129\sn76489_driver
 */
#ifndef SPFM_H
#define SPFM_H

#include <stdint.h>
#include <stdbool.h>

typedef void* SPFM_HANDLE;

typedef struct {
    uint8_t port;
    uint8_t addr;
    uint8_t data;
} spfm_reg_t;

#define SPFM_LITE_BUF_SIZE (1024 * 64)

#ifdef __cplusplus
extern "C" {
#endif

int spfm_init(int dev_idx);
void spfm_cleanup(void);
SPFM_HANDLE spfm_get_handle(void);
void spfm_reset(void);
void spfm_chip_reset(void);
void spfm_write_reg(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data);
void spfm_write_regs(uint8_t slot, const spfm_reg_t* regs, uint32_t count, uint32_t write_wait);
void spfm_write_data(uint8_t slot, uint8_t data);
void spfm_hw_wait(uint32_t samples);
void spfm_wait_and_write_reg(uint32_t wait_samples, uint8_t slot, uint8_t port, uint8_t addr, uint8_t data);
bool spfm_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* SPFM_H */
