# Scope Chip Registration System

## 概述

示波器芯片注册表（Scope Chip Registration）用于管理多个音频芯片的示波器 buffer 映射。
每个芯片通过 `(chip_name, chip_inst)` 二元组注册自己的示波器数据槽位，
获得全局 `m_voice_buff[]` 中的独立 buffer 区间。

## 设计背景

之前芯片核心（fmopn.c, ym2151.c）通过全局变量 `m_voice_current_system`
和线性搜索 `m_voice_ChipID[]` 数组来查找自己的示波器 buffer 偏移。
这种机制存在以下问题：

- 无法区分芯片类型（只靠设备索引号）
- 多个同类型芯片时会冲突
- 通道数量硬编码在各 update 函数中
- `m_voice_current_system` 是全局状态，在多芯片场景下互相覆盖

## 核心数据结构

### 通道类型标识

```c
#define SCOPE_CH_FM      0   // FM 合成通道
#define SCOPE_CH_SSG     1   // SSG/PSG 方波通道
#define SCOPE_CH_ADPCM_A 2   // ADPCM-A 节奏/采样通道
#define SCOPE_CH_ADPCM_B 3   // ADPCM-B Delta-T 通道
#define SCOPE_CH_DAC     4   // 直通 DAC 通道
#define SCOPE_CH_NOISE   5   // 噪声通道
```

### 通道分组描述

每个芯片可以包含多种类型的通道（如 OPNA 有 FM + ADPCM-A + ADPCM-B）。
`ScopeChGroup` 描述一个通道分组：

```c
#define SCOPE_MAX_GROUPS 8

typedef struct {
    int type;           // SCOPE_CH_FM, SCOPE_CH_SSG, etc.
    int count;          // 该组的通道数量
    int start;          // 在芯片内的通道偏移（0-based）
    const char *label;  // 短标签前缀，如 "FM", "SSG", "ADPCM-A", "ADPCM-B"
} ScopeChGroup;
```

### 芯片槽位

```c
typedef struct {
    const char *chip_name;    // 芯片名称，如 "YM2151", "YM2608"
    int         chip_inst;    // 同类型芯片的实例编号 (0, 1, ...)
    int         num_channels; // 该芯片的总示波器通道数
    int         slot_base;    // 在 m_voice_buff[] 中的基地址
    int         active;       // 1 = 槽位正在使用
    int         num_groups;   // 通道分组数量
    ScopeChGroup groups[SCOPE_MAX_GROUPS]; // 通道分组描述
} ScopeChipSlot;
```

前端根据 `groups` 信息生成通道标签，如 "FM 1", "FM 2", ..., "ADPCM-A 1", "ADPCM-B"。

## API

```c
// 注册或查找芯片槽位。如果已存在则返回已有槽位，否则分配新槽位。
// 返回指向 ScopeChipSlot 的指针，或 NULL（表满/buffer 不够）。
ScopeChipSlot* scope_register_chip(const char *chip_name, int chip_inst,
                                    int num_channels, int samplerate);

// 按 (chip_name, chip_inst) 查找已注册的槽位。未找到返回 NULL。
ScopeChipSlot* scope_find_slot(const char *chip_name, int chip_inst);

// 重置所有槽位（文件卸载时调用）。
void scope_reset_all(void);
```

## 文件结构

| 文件 | 说明 |
|------|------|
| `emu/cores/ModizerConstants.h` | 定义 `SCOPE_MAX_CHIPS 16` |
| `emu/cores/ModizerVoicesData.h` | `ScopeChipSlot` 结构体和 API 声明 |
| `emu/cores/scope_chip_reg.c` | 注册表实现（属于 scope_core_lib） |
| `emu/cores/fmopn.c` | OPN 系列芯片调用 `scope_register_chip()` |
| `emu/cores/ym2151.c` | YM2151 调用 `scope_register_chip()` |
| `vgm_window.cpp` | 前端调用 `scope_find_slot()` 获取 buffer 映射 |
| `modizer_viz.cpp` | 定义全局 buffer 数组和 legacy 变量 |

## 芯片注册表

| 芯片 | chip_name | 通道数 | 通道分组 |
|------|-----------|--------|---------|
| YM2151 | `"YM2151"` | 8 | FM 1-8 |
| YM2203 | `"YM2203"` | 3 | FM 1-3 |
| YM2608 (OPNA) | `"YM2608"` | 13 | FM 1-6, ADPCM-A 1-6, ADPCM-B |
| YM2610 (OPNB) | `"YM2610"` | 11 | FM 1-4, ADPCM-A 1-6, ADPCM-B |
| YM2610B (OPNB+) | `"YM2610B"` | 13 | FM 1-6, ADPCM-A 1-6, ADPCM-B |
| YM2612 (OPN2) | `"YM2612"` | 6 | FM 1-6 |

## 工作流程

### 芯片核心侧（写入数据）

在每个芯片的 `update_one()` 函数开头：

```c
ScopeChipSlot *slot = scope_register_chip("YM2151", 0, 8, m_voice_current_samplerate);
// 设置通道分组（只需在首次注册时设置一次）
if (slot && slot->num_groups == 0) {
    slot->groups[0] = (ScopeChGroup){SCOPE_CH_FM, 8, 0, "FM"};
    slot->num_groups = 1;
}
int m_voice_ofs = slot ? slot->slot_base : -1;
```

对于包含多种通道类型的芯片（如 YM2608 OPNA）：

```c
ScopeChipSlot *slot = scope_register_chip("YM2608", 0, 13, m_voice_current_samplerate);
if (slot && slot->num_groups == 0) {
    slot->groups[0] = (ScopeChGroup){SCOPE_CH_FM, 6, 0, "FM"};
    slot->groups[1] = (ScopeChGroup){SCOPE_CH_ADPCM_A, 6, 6, "ADPCM-A"};
    slot->groups[2] = (ScopeChGroup){SCOPE_CH_ADPCM_B, 1, 12, "ADPCM-B"};
    slot->num_groups = 3;
}
int m_voice_ofs = slot ? slot->slot_base : -1;
```

后续的 buffer 写入代码不变，仍使用 `m_voice_buff[m_voice_ofs + ch]`。

### 前端侧（读取数据）

`vgm_window.cpp` 中，通过芯片类型映射到 scope 名称，再查找槽位：

```c
ScopeChipSlot *slot = scope_find_slot("YM2151", chipInst);
if (slot) voice_ofs = slot->slot_base;
```

### 文件卸载时

```c
scope_reset_all();  // 清空注册表
```

## 扩展新芯片：完整指南

以添加一个新芯片（例如 YM3812/OPL2）的示波器支持为例，分两种情况：

### 情况 A：芯片源码在预编译库中（不可修改）

如果芯片核心在 `libvgm-emu.a` 中且不想重新编译，则需要新建一个 override 源文件
并加入 `scope_core_lib` 的 `--whole-archive` 覆盖链接。

**步骤 1 — 创建 override 源文件**

在 `libvgm-modizer/emu/cores/` 下创建新文件（如 `ym3812_scope.c`），
只包含需要 override 的 `update_one` 函数和 scope 注册代码：

```c
// ym3812_scope.c — override ym3812_update_one from libvgm-emu.a
#include "ModizerVoicesData.h"
// ... 其他必要的头文件，复制原文件中的 include

// 将原 update 函数体复制过来，在开头替换 offset 查找逻辑：
static void ym3812_update_one(void *chip, UINT32 length, DEV_SMPL **buffers)
{
    // 替换旧的 m_voice_ChipID 搜索为：
    ScopeChipSlot *slot = scope_register_chip("YM3812", 0, 9, m_voice_current_samplerate);
    int m_voice_ofs = slot ? slot->slot_base : -1;

    // ... 原有音频生成代码不变 ...

    // 在采样写入循环中，使用 m_voice_buff[m_voice_ofs + ch] 写入数据
}
```

**步骤 2 — 将新文件加入 CMakeLists.txt 的 scope_core_lib**

```cmake
# CMakeLists.txt
add_library(scope_core_lib STATIC
    ${SCOPE_CORE_DIR}/fmopn.c
    ${SCOPE_CORE_DIR}/ym2151.c
    ${SCOPE_CORE_DIR}/scope_chip_reg.c
    ${SCOPE_CORE_DIR}/ym3812_scope.c          # <-- 新增
)
```

由于 `scope_core_lib` 在链接时使用 `--whole-archive`，新文件中的符号会
自动覆盖预编译 `libvgm-emu.a` 中的同名符号。

**步骤 3 — 在 vgm_window.cpp 添加前端映射**

在 `RenderLevelMeterArea()` 的 switch 中添加：

```cpp
case DEVID_YM3812:  scopeName = "YM3812";  break;
```

并在 `GetModizerVoiceCount()` 中添加：

```cpp
case DEVID_YM3812:  return 9;
```

**步骤 4 — 增量编译**

```bash
bash build_v14.sh
```

只有 `ym3812_scope.c`、`vgm_window.cpp` 和链接步骤会重新执行。
CMake 会自动检测 CMakeLists.txt 变更并重新配置。

---

### 情况 B：芯片源码已在 scope_core_lib 中（如 fmopn.c 内的芯片）

如果芯片的 update 函数已经在 `fmopn.c` 或 `ym2151.c` 等已 override 的文件中，
只需在该文件中添加 MODIZER 代码段即可，无需新建文件。

**步骤 1 — 在对应 update 函数中添加 scope 注册**

在 fmopn.c 中找到目标芯片的 `update_one` 函数，在原有音频代码之前插入：

```c
//TODO:  MODIZER changes start / YOYOFR
ScopeChipSlot *scope_slot_xxx = scope_register_chip("芯片名", 0, 通道数, m_voice_current_samplerate);
// 设置通道分组（首次注册时）
if (scope_slot_xxx && scope_slot_xxx->num_groups == 0) {
    scope_slot_xxx->groups[0] = (ScopeChGroup){SCOPE_CH_FM, 通道数, 0, "FM"};
    // 如有多种类型，继续添加：
    // scope_slot_xxx->groups[1] = (ScopeChGroup){SCOPE_CH_ADPCM_A, n, offset, "ADPCM-A"};
    scope_slot_xxx->num_groups = 1; // 或实际分组数
}
int m_voice_ofs = scope_slot_xxx ? scope_slot_xxx->slot_base : -1;
if (!m_voice_current_samplerate) m_voice_current_samplerate = 44100;
int64_t smplIncr = (int64_t)44100*(1<<MODIZER_OSCILLO_OFFSET_FIXEDPOINT)/m_voice_current_samplerate;
//TODO:  MODIZER changes end / YOYOFR
```

在采样循环中插入 buffer 写入代码（参考同文件中 YM2151 或 YM2612 的写法）：

```c
//TODO:  MODIZER changes start / YOYOFR
if (m_voice_ofs >= 0) {
    int64_t ofs_start = m_voice_current_ptr[m_voice_ofs + 0];
    int64_t ofs_end = ofs_start + smplIncr;
    if (ofs_end > ofs_start)
    for (;;) {
        for (int jj = 0; jj < 通道数; jj++) {
            INT32 val = /* 获取该通道的输出值 */;
            m_voice_buff[m_voice_ofs + jj][(ofs_start >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT)
                & (SOUND_BUFFER_SIZE_SAMPLE * 4 * 2 - 1)] = LIMIT8((val >> 移位位数));
        }
        ofs_start += 1 << MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
        if (ofs_start >= ofs_end) break;
    }
    while ((ofs_end >> MODIZER_OSCILLO_OFFSET_FIXEDPOINT) >= SOUND_BUFFER_SIZE_SAMPLE * 4 * 2)
        ofs_end -= (SOUND_BUFFER_SIZE_SAMPLE * 4 * 2 << MODIZER_OSCILLO_OFFSET_FIXEDPOINT);
    for (int jj = 0; jj < 通道数; jj++)
        m_voice_current_ptr[m_voice_ofs + jj] = ofs_end;
}
//TODO:  MODIZER changes end / YOYOFR
```

**步骤 2 — 前端映射**（同情况 A 的步骤 3）

**步骤 3 — 增量编译**

```bash
bash build_v14.sh
```

由于 `fmopn.c` 已在 `scope_core_lib` 中，只有它和链接步骤需要重新编译。

---

### 确定通道数和移位位数的方法

| 参数 | 如何确定 |
|------|---------|
| 通道数 | 芯片的 FM 通道 + SSG/ADPCM 通道总数。参考芯片数据手册或源码中的通道输出数组大小。 |
| 移位位数 | 将芯片内部 INT32 输出值右移到 [-128, 127] 范围所需的位数。YM2151 用 `>>7`，YM2612 用 `>>6`。可通过打印输出值范围来确定。 |
| chip_name 字符串 | 必须与 `vgm_window.cpp` 中 `RenderLevelMeterArea()` 的 switch 分支一致。建议直接使用芯片型号名称。 |

### 多实例芯片（双芯片）

如果 VGM 文件使用两个同类型芯片（如双 YM2151），
`chip_inst` 参数区分实例：

```c
// 芯片核心侧 — 需要知道当前实例编号
// 如果无法从芯片结构体获取，可暂时都用 0
ScopeChipSlot *slot = scope_register_chip("YM2151", chip_inst, 8, rate);
```

```cpp
// 前端侧 — dev.instance 来自 PLR_DEV_INFO
ScopeChipSlot *slot = scope_find_slot("YM2151", devs[devIdx].instance);
```

### 增量编译机制说明

项目的 CMake 构建系统支持增量编译：
- 只修改了 `.c/.cpp` 源文件 → 只重新编译该文件，重新链接
- 修改了 `CMakeLists.txt` → CMake 重新配置，然后增量编译
- 添加了新 `.c` 文件到 `scope_core_lib` → 该文件编译为 .o，更新 `libscope_core_lib.a`，重新链接
- 由于 `scope_core_lib` 使用 `--whole-archive`，新符号会自动覆盖预编译库中的同名符号

强制完全重建：

```bash
rm -rf build && bash build_v14.sh
```

## 向后兼容

旧的预编译库（libvgm-player.a 等）仍会写入 `m_voice_current_system` 等全局变量。
这些变量的定义保留在 `modizer_viz.cpp` 中以确保链接兼容，但不再被 scope 系统使用。
