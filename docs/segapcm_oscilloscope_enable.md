# SPCM (SegaPCM) 示波器启用记录

**日期**：2026-04-05
**版本**：v15
**作者**：Denjhang
**状态**：完成

## 背景

SegaPCM（SPCM）是 Sega 16 通道 8bit PCM 芯片，用于 Sega System 1/2 等街机。
示波器代码实现已存在于 `segapcm.c`，但构建配置遗漏导致修改版从未被编译链接。

## 问题

预编译库 `libvgm-emu.a` 中的旧 `segapcm.c.o`（无 scope 支持）被链接器使用，
而本地修改版（含 `scope_register_chip` 调用）因以下两个遗漏从未生效：

1. `segapcm.c` 未加入 `scope_core_lib`（CMakeLists.txt）
2. 预编译库中旧 `segapcm.c.o` 未被 `ar d` 移除
3. `#include` 路径错误：`../../../../../src/ModizerVoicesData.h`（找不到头文件）

## 修改清单

### 1. segapcm.c — 修正头文件 include 路径

**文件**：`libvgm-modizer/emu/cores/segapcm.c`
**行号**：19

```c
// 修改前（错误路径，编译时找不到头文件）
#include "../../../../../src/ModizerVoicesData.h"

// 修改后（与其他核心文件一致）
#include "ModizerVoicesData.h"
```

### 2. CMakeLists.txt — 添加 segapcm.c 到 scope_core_lib

**文件**：`CMakeLists.txt`
**行号**：104（`emu2413.c` 之后）

```cmake
add_library(scope_core_lib STATIC
    ${SCOPE_CORE_DIR}/fmopn.c
    ${SCOPE_CORE_DIR}/ym2151.c
    ${SCOPE_CORE_DIR}/scope_chip_reg.c
    ${SCOPE_CORE_DIR}/emu2149.c
    ${SCOPE_CORE_DIR}/ay8910.c
    ${SCOPE_CORE_DIR}/sn76496.c
    ${SCOPE_CORE_DIR}/k051649.c
    ${SCOPE_CORE_DIR}/fmopl.c
    ${SCOPE_CORE_DIR}/ymf262.c
    ${SCOPE_CORE_DIR}/adlibemu_opl2.c
    ${SCOPE_CORE_DIR}/adlibemu_opl3.c
    ${SCOPE_CORE_DIR}/emu2413.c
    ${SCOPE_CORE_DIR}/segapcm.c        # ← 新增
)
```

### 3. build_v15.sh — 添加 ar d 移除预编译库冲突 .o

**文件**：`build_v15.sh`
**位置**：cmake 配置之前

```bash
# Remove .o files from prebuilt lib that we override with scope_core_lib
PREBUILT_LIB="libvgm-modizer/build/bin/libvgm-emu.a"
if [ -f "$PREBUILT_LIB" ]; then
    ar d "$PREBUILT_LIB" segapcm.c.o 2>/dev/null || true
fi
```

## 核心实现细节（已存在，无需修改）

### scope 注册

`segapcm.c` 初始化时注册 16 通道：

```c
ScopeChipSlot *scope_slot = scope_register_chip("SEGAPCM", 0, 16, m_voice_current_samplerate);
int m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
int m_total_channels = 16;
```

### buffer 写入（segapcm.c:200-212）

每个采样周期将 PCM 数据写入环形缓冲区：

```c
if (m_voice_ofs>=0) {
    int64_t ofs_start = m_voice_current_ptr[m_voice_ofs+ch];
    int64_t ofs_end = (m_voice_current_ptr[m_voice_ofs+ch] + smplIncr);

    if (ofs_end > ofs_start)
        for (;;) {
            m_voice_buff[m_voice_ofs+ch][...] = LIMIT8(((v * ((regs[2]&0x7F)+(regs[3]&0x7F)))>>7));
            ofs_start += 1 << MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
            if (ofs_start >= ofs_end) break;
        }
    m_voice_current_ptr[m_voice_ofs+ch] = ofs_end;
}
```

- 每通道独立写入（非 shared-ofs 模式），因为 SegaPCM 各通道采样率可不同
- 移位 `>>7`：将 14bit 左右声道混合值压缩到 8bit buffer
- 静音通道也有指针更新（segapcm.c:222-231），避免失步

### 前端映射（已存在）

| 层级 | 位置 | 内容 |
|------|------|------|
| 语音计数 | `vgm_window.cpp:2535` | `case DEVID_SEGAPCM: return 16;` |
| 芯片名称 | `vgm_window.cpp:4067,4376` | `case DEVID_SEGAPCM: scopeName = "SEGAPCM";` |
| 电平条 | `vgm_window.cpp:3998-4011` | 16 通道完整实现 |
| 芯片标签 | `vgm_window.cpp:2643` | `chipLabel = "SegaPCM"; chipAbbrev = "SPCM";` |
| Shadow 寄存器 | `vgm_window.cpp:173` | `s_shadowSEGAPCM[2][0x100]` |

## 技术参数

| 参数 | 值 |
|------|-----|
| scope_name | "SEGAPCM" |
| 通道数 | 16 |
| buffer 移位 | >>7 |
| DC 偏移 | 无（已由 `v = spcm->rom[...] - 0x80` 在采样时处理） |
| 写入模式 | 每通道独立指针（非 shared-ofs） |
| 设备 ID | DEVID_SEGAPCM (0x04) |
| VGM 命令 | 0xC0 |

## 测试验证

1. `rm -rf build && bash build_v15.sh`
2. 加载含 SegaPCM 的 VGM 文件（如 Sega System 1/2 游戏）
3. 确认示波器显示 16 个通道（P1-P9, 10-16）
4. 确认波形有 PCM 采样特征（非 FM 连续波形）
