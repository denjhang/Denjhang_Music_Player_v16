#ifndef GIGATRON_EMU_H
#define GIGATRON_EMU_H

#include <stdint.h>
#include <stdbool.h> // For bool type

#ifdef __cplusplus
extern "C" {
#endif

// Define INLINE for compatibility
#ifndef INLINE
#define INLINE static inline
#endif

// Define UINT64 for compatibility if not already defined
#ifndef UINT64
typedef uint64_t UINT64;
#endif
#ifndef INT64
typedef int64_t INT64;
#endif
#ifndef UINT32
typedef uint32_t UINT32;
#endif
#ifndef INT32
typedef int32_t INT32;
#endif

// Gigatron 寄存器地址定义
#define adrWavA 250 // 0xFA: 波形幅度 (Waveform Amplitude)
#define adrWavX 251 // 0xFB: 波形选择 (Waveform Select)
#define adrFnumL 252 // 0xFC: 频率低位 (Frequency Low)
#define adrFnumH 253 // 0xFD: 频率高位 (Frequency High)

// Ratio Counter (from libvgm-master/emu/RatioCntr.h)
#define RC_SHIFT	32
typedef UINT64		RC_TYPE;
typedef INT64		RC_STYPE;

typedef struct
{
	RC_TYPE inc;	// counter increment
	RC_TYPE val;	// current value
} RATIO_CNTR;

INLINE void RC_SET_RATIO(RATIO_CNTR* rc, UINT32 mul, UINT32 div)
{
	rc->inc = (RC_TYPE)((((UINT64)mul << RC_SHIFT) + div / 2) / div);
}

INLINE void RC_SET_INC(RATIO_CNTR* rc, double val)
{
	rc->inc = (RC_TYPE)(((RC_TYPE)1 << RC_SHIFT) * val + 0.5);
}

INLINE void RC_STEP(RATIO_CNTR* rc)
{
	rc->val += rc->inc;
}

INLINE void RC_STEPS(RATIO_CNTR* rc, UINT32 step)
{
	rc->val += rc->inc * step;
}

INLINE UINT32 RC_GET_VAL(const RATIO_CNTR* rc)
{
	return (UINT32)(rc->val >> RC_SHIFT);
}

INLINE void RC_SET_VAL(RATIO_CNTR* rc, UINT32 val)
{
	rc->val = (RC_TYPE)val << RC_SHIFT;
}

INLINE void RC_RESET(RATIO_CNTR* rc)
{
	rc->val = 0;
}

INLINE void RC_RESET_PRESTEP(RATIO_CNTR* rc)
{
	rc->val = ((RC_TYPE)1 << RC_SHIFT) - rc->inc;
}

INLINE void RC_MASK(RATIO_CNTR* rc)
{
	rc->val &= (((RC_TYPE)1 << RC_SHIFT) - 1);
}

INLINE void RC_VAL_INC(RATIO_CNTR* rc)
{
	rc->val += (RC_TYPE)1 << RC_SHIFT;
}

INLINE void RC_VAL_DEC(RATIO_CNTR* rc)
{
	rc->val -= (RC_TYPE)1 << RC_SHIFT;
}

INLINE void RC_VAL_ADD(RATIO_CNTR* rc, INT32 val)
{
	rc->val += (RC_STYPE)val << RC_SHIFT;
}

INLINE void RC_VAL_SUB(RATIO_CNTR* rc, INT32 val)
{
	rc->val -= (RC_STYPE)val << RC_SHIFT;
}

// Gigatron 声音通道状态
typedef struct {
    uint16_t osc; // 振荡器相位
    uint16_t key; // 频率步进值
    uint8_t wavX; // 波形选择
    int8_t wavA; // 波形幅度 (直流偏移量)
} GigatronChannel;

// Gigatron 仿真器状态
#define GT_SCOPE_BUF_SIZE 4096

typedef struct {
    GigatronChannel ch[4]; // 4个通道
    uint8_t soundTable[256]; // 原始 6-bit 波形数据表
    uint8_t customWaveTable[256]; // 自定义波形表 (6-bit, 0-63)
    bool useCustomWaveTable; // 使用自定义波形表开关
    int32_t samp; // 当前累积的样本 (使用 int32_t 防止溢出)
    RATIO_CNTR scanline_rc; // 用于跟踪扫描线进度的比率计数器
    uint32_t audioSampleRate; // 音频输出采样率
    uint8_t channelMask; // 通道掩码，用于控制哪些通道参与声音生成

    // 新增的音频处理选项
    uint8_t audio_bit_depth; // 4, 6, 8, 12, 16
    bool dc_offset_removal_enabled;
    double dc_bias;
    double dc_alpha; // 用于 DC 移除滤波器
    float volume_scale; // 音量缩放 (0.0 - 2.0, default 0.5)

    // 示波器波形缓冲（4 通道环形缓冲）
    int16_t scope_buf[4][GT_SCOPE_BUF_SIZE];
    volatile int scope_pos; // 写入位置（供外部读取）
    int scope_skip_ctr;     // 降采样计数器
} GigatronState;

// 初始化 Gigatron 仿真器
void gigatron_emu_init(GigatronState *state);

// 重置自定义波形表（从原始 soundTable 复制默认数据）
void gigatron_emu_reset_custom_wave_table(GigatronState *state);

// 写入 Gigatron 寄存器
// adr: 寄存器地址 (包含通道信息)
// data: 写入的数据
void gigatron_emu_write_register(GigatronState *state, uint16_t adr, uint8_t data);

// 更新 Gigatron 仿真器并生成音频样本
// state: Gigatron 仿真器状态
// output_buffer: 存储生成的 16 位立体声样本的缓冲区
// num_samples: 要生成的样本数量 (每个样本包含左右声道)
void gigatron_emu_update(GigatronState *state, int16_t *output_buffer, int num_samples);

#ifdef __cplusplus
}
#endif

#endif // GIGATRON_EMU_H