# GB_DMG / NES_APU 示波器适配记录

**日期**：2026-04-05
**版本**：v15
**状态**：完成

## 背景

GB_DMG 和 NES_APU 都已有旧版 MODIZER 示波器代码（`m_voice_ChipID[]` 手动搜索方式），但因各种原因无法正常工作：
- GB_DMG：有声但无波形
- NES_APU：无声无波形

需要诊断并修复。

## GB_DMG 修复

### 根本原因

gb.c 使用旧 `m_voice_ChipID[]` 搜索方式定位 scope buffer 偏移，但没有调用 `scope_register_chip()` 注册芯片。新注册系统通过 `scope_register_chip` 填充 `m_voice_ChipID[]`，不调用就永远不会匹配 → `m_voice_ofs` 始终为 -1 → 无波形。

### 修改（1 处）

**文件**：`libvgm-modizer/emu/cores/gb.c`（行 1117-1125）

```c
// 修改前
int m_voice_ofs=-1;
int m_total_channels=4;
for (int ii=0;ii<=SOUND_MAXVOICES_BUFFER_FX-m_total_channels;ii++) {
    if (m_voice_ChipID[ii]==m_voice_current_system) {
        m_voice_ofs=ii+(m_voice_current_systemSub?m_voice_current_systemPairedOfs:0);
        m_voice_current_total=m_total_channels;
        break;
    }
}

// 修改后
int m_voice_ofs=-1;
int m_total_channels=4;
ScopeChipSlot *scope_slot = scope_register_chip("GB_DMG", 0, 4, m_voice_current_samplerate);
m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
```

buffer 写入代码（4 通道，`>>0`，`LIMIT8(val>>0)`）保持不变，已经正确。

### 前端（已存在）

| 层级 | 值 |
|------|-----|
| GetModizerVoiceCount | return 4 |
| scopeName | "GB_DMG" |
| BuildLevelMeters | s_shadowGB_DMG[2][0x30] |
| Shadow 寄存器 | NR10-NR52，freq/vol/duty 从 shadow 数组读取 |
| 芯片标签 | "GB DMG" |
| DEVID | 0x13 |

## NES_APU 修复

### 根本原因

CMakeLists.txt 定义了 `EC_NES_APU_MAME` 宏，但 nesintf.h 只识别 `EC_NES_MAME` 和 `EC_NES_NSFPLAY`。同时 `SNDDEV_SELECT` 被定义后阻断了 nesintf.h 中的默认宏定义（`#ifndef SNDDEV_SELECT` 块），导致 `devDefList_NES_APU` 编译为空数组 `[NULL]`，设备初始化直接失败。

### 修改（3 处）

**1. CMakeLists.txt（行 129）— 编译定义**

```cmake
# 修改前
SNDDEV_C6280 SNDDEV_NES_APU EC_NES_APU_MAME

# 修改后
SNDDEV_C6280 SNDDEV_NES_APU EC_NES_MAME EC_NES_NSFPLAY EC_NES_NSFP_FDS
```

这样 nesintf.c 同时编译 MAME 和 NSFPlay 路径。NSFPlay 路径（`nes_stream_update_nsfplay`）包含 MODIZER 示波器代码。

**2. CMakeLists.txt — scope_core_lib 添加 np_nes_dmc.c**

```cmake
${SCOPE_CORE_DIR}/nesintf.c
${SCOPE_CORE_DIR}/np_nes_apu.c
${SCOPE_CORE_DIR}/np_nes_dmc.c   # 新增
${SCOPE_CORE_DIR}/gb.c
```

np_nes_dmc.c 包含 Triangle/Noise/DMC 通道的 scope buffer 写入代码，是 NSFPlay 路径必需的。

**3. np_nes_dmc.c（行 19）— include 路径**

```c
// 修改前
#include "../../../../../src/ModizerVoicesData.h"
// 修改后
#include "ModizerVoicesData.h"
```

### build_v15.sh — 新增 ar d

```bash
ar d "$PREBUILT_LIB" nesintf.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" np_nes_apu.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" np_nes_dmc.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" gb.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" c6280_mame.c.o 2>/dev/null || true
```

### NES_APU 架构说明

nesintf.c 有两个编译路径：
- `#ifdef EC_NES_MAME`：`nes_stream_update_mame()`，调用 `nes_apu_update()`，**无** MODIZER 代码
- `#ifdef EC_NES_NSFPLAY`：`nes_stream_update_nsfplay()`，调用 `NES_APU_np_Render()` + `NES_DMC_np_Render()`，**有** MODIZER 代码

两个路径在 `devDefList_NES_APU[]` 中按 NSFPlay 优先、MAME 其次排列。NES 使用 NSFPlay 核心时示波器才能工作。

NES_APU 的 scope buffer 写入分布在三个源文件中：
- np_nes_apu.c：Pulse 1/2（通道 0-1），在 `NES_APU_np_Render()` 内写入
- np_nes_dmc.c：Triangle/Noise/DMC（通道 2-4），在 `NES_DMC_np_Render()` 内写入，偏移为 `m_voice_ofs+jj+2`
- nesintf.c：`nes_stream_update_nsfplay()` 内负责 vgm_last_note/instr/vol 跟踪（通道 0-5）

### 前端（已存在）

| 层级 | 值 |
|------|-----|
| GetModizerVoiceCount | return 6 |
| scopeName | "NES_APU" |
| BuildLevelMeters | s_shadowNES_APU[2][0x20] |
| Shadow 寄存器 | 2A03 寄存器 $4000-$4017 |
| 芯片标签 | "NES APU" |
| DEVID | 0x14 |

## 技术参数

| 参数 | GB_DMG | NES_APU |
|------|--------|---------|
| scope_name | "GB_DMG" | "NES_APU" |
| 通道数 | 4 | 6 |
| 通道分组 | SQ1 SQ2 Wav Nse | P1 P2 Tri Nse DMC (FDS) |
| buffer 移位 | >>0 | >>7 |
| DC 偏移 | 无 | 无 |
| 仿真文件 | gb.c | nesintf.c + np_nes_apu.c + np_nes_dmc.c |

## 教训总结

1. **旧搜索方式必须替换为 scope_register_chip**：不调用注册函数，`m_voice_ChipID[]` 里就没有对应条目，搜索永远返回 -1
2. **编译宏名必须与头文件一致**：`EC_NES_APU_MAME` 是无效宏名，nesintf.h 只认 `EC_NES_MAME`/`EC_NES_NSFPLAY`
3. **NES 需要三个源文件**：nesintf.c（接口）、np_nes_apu.c（Pulse）、np_nes_dmc.c（Triangle/Noise/DMC），缺一不可
4. **SNDDEV_SELECT 会阻断默认宏**：nesintf.h 在 `#ifndef SNDDEV_SELECT` 下定义默认宏，CMakeLists 定义了 SNDDEV_SELECT 就必须手动添加正确的宏
