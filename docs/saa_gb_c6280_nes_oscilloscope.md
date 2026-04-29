# SAA1099 / GB_DMG / C6280 / NES_APU 示波器适配记录

**日期**：2026-04-05
**版本**：v15
**作者**：Denjhang
**状态**：完成

## 背景

4 个芯片的 scope buffer 写入代码状态各不相同：
- SAA1099、GB_DMG、NES_APU：有旧版 MODIZER 代码，使用手动 `m_voice_ChipID[]` 搜索
- C6280：完全无 scope 实现，需从零添加

统一适配为 `scope_register_chip()` 注册表系统，并修复构建配置。

## 修改清单

### 1. saa1099_vb.c（6 通道）

**文件**：`libvgm-modizer/emu/cores/saa1099_vb.c`

- 修正 include 路径：`../../../../../src/ModizerVoicesData.h` → `"ModizerVoicesData.h"`
- 替换旧的 `m_voice_ChipID[]` 搜索为 `scope_register_chip("SAA1099", 0, 6, ...)`
- buffer 写入代码（6 通道，`>>0`）无需修改

### 2. gb.c（4 通道）

**文件**：`libvgm-modizer/emu/cores/gb.c`

- 修正 include 路径
- 替换旧的搜索为 `scope_register_chip("GB_DMG", 0, 4, ...)`
- buffer 写入代码（4 通道，`>>0`）无需修改

### 3. c6280_mame.c（6 通道）— 完整新实现

**文件**：`libvgm-modizer/emu/cores/c6280_mame.c`

- 添加 `#include "ModizerVoicesData.h"`
- 在 `c6280mame_update()` 开头添加 `scope_register_chip("C6280", 0, 6, ...)`
- 在 4 个音频分支（Noise、DDA、LFO Waveform、Normal Waveform）的 sample 循环内添加 scope buffer 写入
- 输出值 `vll * (data - 16)` 通过 `>>7` 压缩到 8bit buffer
- 静音/禁用通道也有指针更新，防止失步
- 添加 vgm_last_note/instr/vol 跟踪

### 4. nesintf.c + np_nes_apu.c（6 通道）

**文件**：`libvgm-modizer/emu/cores/nesintf.c`
**文件**：`libvgm-modizer/emu/cores/np_nes_apu.c`

两个文件都修正 include 路径并替换旧搜索为 `scope_register_chip("NES_APU", 0, 6, ...)`
- nesintf.c：负责 vgm_last_note 跟踪（Pulse/DMC/FDS）
- np_nes_apu.c：负责 scope buffer 写入（Pulse 通道，`>>7`）
- 两个文件注册相同的 "NES_APU" 名称，第二次注册会返回已有槽

### 5. CMakeLists.txt

添加 6 个源文件：
```cmake
${SCOPE_CORE_DIR}/saa1099_vb.c
${SCOPE_CORE_DIR}/gb.c
${SCOPE_CORE_DIR}/c6280_mame.c
${SCOPE_CORE_DIR}/nesintf.c
${SCOPE_CORE_DIR}/np_nes_apu.c
```

### 6. build_v15.sh

添加 ar d 移除预编译库冲突 .o：
```bash
ar d "$PREBUILT_LIB" saa1099_vb.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" gb.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" c6280_mame.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" nesintf.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" np_nes_apu.c.o 2>/dev/null || true
```

## 技术参数

| 参数 | SAA1099 | GB_DMG | C6280 | NES_APU |
|------|---------|--------|-------|---------|
| scope_name | "SAA1099" | "GB_DMG" | "C6280" | "NES_APU" |
| 通道数 | 6 | 4 | 6 | 6 |
| buffer 移位 | >>0 | >>0 | >>7 | >>7 |
| DC 偏移 | 无 | 无 | 无 | 无 |
| 输出格式 | INT16 立体声 | INT16 立体声 | vll*(data-16) ~14bit | INT16 非线性混合 |
| 仿真文件 | saa1099_vb.c | gb.c | c6280_mame.c | nesintf.c + np_nes_apu.c |
| DEVID | 0x23 | 0x13 | 0x1B | 0x14 |

## 前端（已存在，无需修改）

| 层级 | SAA1099 | GB_DMG | C6280 | NES_APU |
|------|---------|--------|-------|---------|
| GetModizerVoiceCount | 6 | 4 | 6 | 6 |
| scopeName | "SAA1099" | "GB_DMG" | "C6280" | "NES_APU" |
| BuildLevelMeters | s_shadowSAA1099 | s_shadowGB_DMG | s_shadowC6280 | s_shadowNES_APU |
| Shadow 寄存器 | [2][0x20] | [2][0x30] | [2][6][8] | [2][0x20] |
