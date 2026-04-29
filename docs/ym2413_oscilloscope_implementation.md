# YM2413 示波器完整实现指南

**日期**：2026-04-04  
**版本**：v15  
**作者**：Denjhang  
**状态**：完成

## 目录

1. [概述](#概述)
2. [调试和核心识别](#调试和核心识别)
3. [增量编译策略](#增量编译策略)
4. [架构设计](#架构设计)
5. [仿真核心层修改](#仿真核心层修改)
6. [前端 BuildLevelMeters 层修改](#前端-buildlevelmeters-层修改)
7. [前端 RenderScopeArea 层修改](#前端-renderscopearea-层修改)
8. [完整修改清单](#完整修改清单)
9. [测试验证](#测试验证)

---

## 调试和核心识别

### 问题
libvgm-modizer 可能包含多个 YM2413 核心实现（如 emu2413.c、fmopl.c 等），需要确定实际使用的是哪一个。

### 调试步骤

#### 步骤 1：添加初始化调试语句

在 `emu2413.c` 的初始化函数中添加调试输出：

**文件**：`libvgm-modizer/emu/cores/emu2413.c`

```c
// 在 Device_Init_OPLL 或类似初始化函数中添加
VgmLog("[YM2413] Initializing OPLL emulator core\n");
VgmLog("[YM2413] Using emu2413.c implementation\n");
VgmLog("[YM2413] Registering scope slot with %d channels\n", num_channels);
```

#### 步骤 2：添加示波器注册调试语句

在 `scope_register_chip()` 调用处添加调试：

**文件**：`libvgm-modizer/emu/cores/emu2413.c`

```c
ScopeChipSlot *scope_slot = scope_register_chip("YM2413", 0, num_channels, m_voice_current_samplerate);
if (scope_slot) {
    VgmLog("[YM2413 Scope] Registered successfully\n");
    VgmLog("[YM2413 Scope] slot_base=%d, num_channels=%d\n", scope_slot->slot_base, num_channels);
    VgmLog("[YM2413 Scope] num_groups=%d\n", scope_slot->num_groups);
} else {
    VgmLog("[YM2413 Scope] Registration FAILED\n");
}
```

#### 步骤 3：添加前端查找调试语句

在 `RenderScopeArea()` 中添加调试：

**文件**：`vgm_window.cpp`

```cpp
if (chipType == DEVID_YM2413) {
    VgmLog("[Scope] Looking for YM2413 slot (chipInst=%d)\n", chipInst);
    ScopeChipSlot *slot = scope_find_slot(scopeName, chipInst);
    if (slot) {
        VgmLog("[Scope] Found YM2413 slot: base=%d, channels=%d\n", 
               slot->slot_base, slot->num_channels);
        voice_ofs = slot->slot_base;
    } else {
        VgmLog("[Scope] YM2413 slot NOT found!\n");
        voice_ofs = -1;
    }
}
```

#### 步骤 4：检查编译输出

运行构建并查看日志：

```bash
cd YM2163_Piano_v15_Release
bash build_v15.sh 2>&1 | grep -i "ym2413\|scope"
```

#### 步骤 5：运行程序并检查调试输出

加载 YM2413 VGM 文件，查看控制台输出：

```
[YM2413] Initializing OPLL emulator core
[YM2413] Using emu2413.c implementation
[YM2413] Registering scope slot with 14 channels
[YM2413 Scope] Registered successfully
[YM2413 Scope] slot_base=0, num_channels=14
[YM2413 Scope] num_groups=2
[Scope] Looking for YM2413 slot (chipInst=0)
[Scope] Found YM2413 slot: base=0, channels=14
```

### 常见问题排查

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| `[YM2413 Scope] Registration FAILED` | 超过最大槽数 | 检查 SCOPE_MAX_CHIPS 定义 |
| `[Scope] YM2413 slot NOT found!` | chip_inst 不匹配 | 确保前端查找时使用 chipInst=0 |
| 没有 YM2413 初始化日志 | 使用了其他核心 | 检查 CMakeLists.txt 中的编译配置 |

---

## 增量编译策略

### 问题
修改仿真核心后，需要确保增量编译正确更新所有依赖。

### 编译流程

#### 1. 清理策略

**完全重建**（推荐首次修改）：
```bash
cd YM2163_Piano_v15_Release
rm -rf build
bash build_v15.sh
```

**增量编译**（仅修改 emu2413.c）：
```bash
cd YM2163_Piano_v15_Release
bash build_v15.sh
```

#### 2. 验证编译

检查是否重新编译了相关文件：

```bash
# 查看编译日志中是否包含 emu2413.c
bash build_v15.sh 2>&1 | grep "emu2413.c"

# 预期输出：
# Building C object CMakeFiles/scope_core_lib.dir/libvgm-modizer/emu/cores/emu2413.c.o
```

#### 3. 链接验证

确保静态库被重新链接：

```bash
# 查看是否重新链接了 scope_core_lib
bash build_v15.sh 2>&1 | grep "scope_core_lib"

# 预期输出：
# Linking C static library libscope_core_lib.a
```

#### 4. 最终可执行文件

确保最终可执行文件被重新链接：

```bash
# 查看是否重新链接了主程序
bash build_v15.sh 2>&1 | grep "ym2163_piano_gui_v15.exe"

# 预期输出：
# Linking CXX executable /d/working/vscode-projects/YM2163-Midi/YM2163_Piano_v15_Release/ym2163_piano_gui_v15.exe
```

### CMakeLists.txt 配置

确保 CMakeLists.txt 中正确配置了 emu2413.c：

**文件**：`YM2163_Piano_v15_Release/CMakeLists.txt`

```cmake
# scope_core_lib 应该包含 emu2413.c
add_library(scope_core_lib STATIC
    libvgm-modizer/emu/cores/emu2413.c
    libvgm-modizer/emu/cores/scope_chip_reg.c
    # ... 其他核心文件
)
```

### 强制重编译

如果增量编译没有更新，使用强制重编译：

```bash
# 方法 1：删除特定的 .o 文件
cd YM2163_Piano_v15_Release/build
rm -f CMakeFiles/scope_core_lib.dir/libvgm-modizer/emu/cores/emu2413.c.o
cd ..
bash build_v15.sh

# 方法 2：使用 cmake 清理
cd YM2163_Piano_v15_Release
cmake --build build --target clean
bash build_v15.sh

# 方法 3：完全重建（最保险）
cd YM2163_Piano_v15_Release
rm -rf build
bash build_v15.sh
```

### 编译时间优化

对于大型项目，完全重建可能很慢。使用增量编译的最佳实践：

1. **修改仿真核心**（emu2413.c）
   - 增量编译通常足够
   - 如果有链接错误，执行完全重建

2. **修改前端代码**（vgm_window.cpp）
   - 增量编译足够
   - 不需要重新编译仿真核心

3. **修改头文件**（SoundDevs.h）
   - 可能需要完全重建
   - 因为多个文件可能包含这个头文件

---

## 架构设计

YM2413（OPLL）是一个 FM 合成芯片，包含：
- **9 个 FM 通道**（melody channels）
- **5 个节奏通道**（rhythm channels）：BD（低音鼓）、HH（踩镲）、SD（小鼓）、TOM（汤姆鼓）、CYM（铙钹）

示波器实现需要在三个层次上进行修改，确保：
1. 仿真核心总是注册最大通道数（14）
2. 前端总是显示所有通道
3. 通道映射与仿真核心的缓冲区布局一致

---

## 架构设计

### 三层架构

```
┌─────────────────────────────────────────────────────────┐
│ 前端渲染层 (vgm_window.cpp)                              │
│ - RenderScopeArea(): 渲染示波器框                        │
│ - BuildLevelMeters(): 构建电平条数据                     │
│ - 通道映射逻辑                                           │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│ 示波器注册层 (scope_chip_reg.c)                          │
│ - scope_register_chip(): 注册芯片示波器槽                │
│ - scope_find_slot(): 查找已注册的槽                      │
│ - 全局槽表管理                                           │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│ 仿真核心层 (emu2413.c)                                   │
│ - 初始化时注册示波器槽                                   │
│ - 每个采样周期写入音频数据到缓冲区                       │
│ - 维护通道组信息（FM、Rhythm）                          │
└─────────────────────────────────────────────────────────┘
```

### 通道布局

YM2413 在示波器缓冲区中的布局：

```
voice_ofs + 0  → FM Channel 1
voice_ofs + 1  → FM Channel 2
voice_ofs + 2  → FM Channel 3
voice_ofs + 3  → FM Channel 4
voice_ofs + 4  → FM Channel 5
voice_ofs + 5  → FM Channel 6
voice_ofs + 6  → FM Channel 7
voice_ofs + 7  → FM Channel 8
voice_ofs + 8  → FM Channel 9
voice_ofs + 9  → Rhythm BD (Bass Drum)
voice_ofs + 10 → Rhythm HH (Hi-Hat)
voice_ofs + 11 → Rhythm SD (Snare Drum)
voice_ofs + 12 → Rhythm TOM (Tom-tom)
voice_ofs + 13 → Rhythm CYM (Cymbal)
```

---

## 仿真核心层修改

### 文件：`libvgm-modizer/emu/cores/emu2413.c`

#### 问题
原始代码根据 `rhythm_mode` 动态计算通道数：

```c
// 原始代码（错误）
int num_channels = (vgmVRC7 ? 6 : 9) + (opll->rhythm_mode ? 5 : 0);
```

这导致：
- 当 rhythm_mode 改变时，注册的通道数改变
- 示波器窗口数量在播放中反复变化

#### 解决方案
总是注册最大通道数（14），即使 rhythm_mode 为 false：

```c
// 修复后的代码
int fm_count = (vgmVRC7 ? 6 : 9);
int num_channels = fm_count + 5;  // Always include 5 rhythm channels
```

#### 修改位置
**文件**：`libvgm-modizer/emu/cores/emu2413.c`  
**行号**：1642-1658

#### 完整修改代码

```c
//TODO:  MODIZER changes start / YOYOFR
// Use new scope_register_chip system
// Always register maximum channels (9 FM + 5 Rhythm) to keep oscilloscope window count stable
// even when rhythm_mode changes during playback
int fm_count = (vgmVRC7 ? 6 : 9);
int num_channels = fm_count + 5;  // Always include 5 rhythm channels
ScopeChipSlot *scope_slot = scope_register_chip("YM2413", 0, num_channels, m_voice_current_samplerate);

if (scope_slot && scope_slot->num_groups == 0) {
    // Create channel groups: FM for melody channels, Rhythm for rhythm channels
    scope_slot->groups[0] = (ScopeChGroup){SCOPE_CH_FM, fm_count, 0, "FM"};
    scope_slot->groups[1] = (ScopeChGroup){SCOPE_CH_RHYTHM, 5, fm_count, "Rhythm"};
    scope_slot->num_groups = 2;
}
// Assign to global static variable for use in mix_output_stereo
m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
m_total_channels = num_channels;
```

#### 关键改动说明

1. **固定通道数**：`num_channels = fm_count + 5` 而不是条件计算
2. **总是创建两个通道组**：FM 和 Rhythm，不再根据 rhythm_mode 条件创建
3. **通道组偏移**：Rhythm 组的起始位置是 `fm_count`（即 9）

### 节奏通道缓冲区映射修复

#### 问题
原始代码中节奏通道的缓冲区映射错误，导致节奏通道数据写入到错误的缓冲区位置（7-8-9 而不是 9-13）：

```c
// 原始代码（错误）
int jj=i;
if (vgmVRC7) {
    if (i>=6) jj=-1;
} else if (i==9) jj=6;   // BD 映射到 jj=6（错误！）
else if (i==10) jj=7;    // HH 映射到 jj=7（错误！）
else if (i==11) jj=7;    // SD 映射到 jj=7（错误！）
else if (i==12) jj=8;    // TOM 映射到 jj=8（错误！）
else if (i==13) jj=8;    // CYM 映射到 jj=8（错误！）
```

这导致：
- 节奏通道的音频数据写入到错误的缓冲区位置
- 示波器显示节奏通道在位置 7-8-9 而不是 9-13
- 节奏通道的示波器波形显示在错误的位置

#### 解决方案
直接使用 `i` 作为 `jj`，使节奏通道映射到正确的缓冲区位置：

```c
// 修复后的代码
int jj=i;
if (vgmVRC7) {
    if (i>=6) jj=-1;
}
// For YM2413: rhythm channels (i=9-13) map directly to jj=9-13
```

#### 修改位置
**文件**：`libvgm-modizer/emu/cores/emu2413.c`  
**行号**：1100-1117

#### 完整修改代码

```c
for (i = 0; i < 14; i++) {

    //YOYOFR
    /* 0..8:tone 9:bd 10:hh 11:sd 12:tom 13:cym */
    int jj=i;
    if (vgmVRC7) {
        if (i>=6) jj=-1;
    }
    // For YM2413: rhythm channels (i=9-13) map directly to jj=9-13
    //YOYOFR

    if (opll->pan[i] & 2)
      out[0] += APPLY_PANNING_S(opll->ch_out[i], opll->pan_fine[i][0]);
    if (opll->pan[i] & 1)
      out[1] += APPLY_PANNING_S(opll->ch_out[i], opll->pan_fine[i][1]);
```

#### 关键改动说明

1. **移除条件映射**：删除了 `else if (i==9) jj=6;` 等错误的映射
2. **直接映射**：节奏通道 i=9-13 现在直接映射到 jj=9-13
3. **缓冲区位置正确**：节奏通道数据现在写入到正确的缓冲区位置

### 缓冲区指针更新修复

#### 问题
原始代码只更新前 9 个通道的缓冲区指针，导致节奏通道（9-13）的指针不更新：

```c
// 原始代码（错误）
for (int jj = 0; jj < 9; jj++) {
    int64_t ofs_end=(m_voice_current_ptr[m_voice_ofs+jj]+smplIncr);
    while ((ofs_end>>MODIZER_OSCILLO_OFFSET_FIXEDPOINT)>=SOUND_BUFFER_SIZE_SAMPLE*4*2) 
        ofs_end-=(SOUND_BUFFER_SIZE_SAMPLE*4*2<<MODIZER_OSCILLO_OFFSET_FIXEDPOINT);
    m_voice_current_ptr[m_voice_ofs+jj]=ofs_end;
}
```

#### 解决方案
更新所有 14 个通道的缓冲区指针：

```c
// 修复后的代码
for (int jj = 0; jj < m_total_channels; jj++) {
    int64_t ofs_end=(m_voice_current_ptr[m_voice_ofs+jj]+smplIncr);
    while ((ofs_end>>MODIZER_OSCILLO_OFFSET_FIXEDPOINT)>=SOUND_BUFFER_SIZE_SAMPLE*4*2) 
        ofs_end-=(SOUND_BUFFER_SIZE_SAMPLE*4*2<<MODIZER_OSCILLO_OFFSET_FIXEDPOINT);
    m_voice_current_ptr[m_voice_ofs+jj]=ofs_end;
}
```

#### 修改位置
**文件**：`libvgm-modizer/emu/cores/emu2413.c`  
**行号**：1154-1159

---

## 前端 BuildLevelMeters 层修改

### 文件：`vgm_window.cpp`

#### 问题
原始代码只在 `rhMode == true` 时添加节奏通道：

```c
// 原始代码（错误）
if (rhMode) {
    for (int i = 0; i < 5; i++) {
        // 添加节奏通道
    }
}
```

这导致：
- 当 rhMode 为 false 时，5 个节奏通道不显示
- 示波器窗口数量变化

#### 解决方案
总是添加 5 个节奏通道，即使 rhMode 为 false：

```c
// 修复后的代码
for (int i = 0; i < 5; i++) {
    ChVizState& rv = s_ym2413Rhy_viz[c][i];
    if (rhMode && rv.key_on_event) {
        rv.decay = 1.0f;
        rv.key_on_event = false;
    } else {
        rv.decay *= 0.80f;
        if (rv.decay < 0.01f) rv.decay = 0.0f;
    }
    float lv = (1.0f - rhVols[i] / 15.0f) * rv.decay;
    add(kRh2413[i], 0, lv, IM_COL32(200,150,80,255), rv.decay > 0.0f, -1);
}
```

#### 修改位置
**文件**：`vgm_window.cpp`  
**行号**：2939-2964（YM2413 case 块中）

#### 完整修改代码

```cpp
case DEVID_YM2413: {
    int c = dev.instance & 1;
    UINT32 clk = (dev.devCfg && dev.devCfg->clock) ? dev.devCfg->clock : 3579545;
    UINT8 rh2413 = s_shadowYM2413[c][0x0E];
    bool  rhMode = (rh2413 >> 5) & 1;
    
    // FM channels (0-8)
    for (int ch = 0; ch < 9; ch++) {
        UINT8 lo   = s_shadowYM2413[c][0x10 + ch];
        UINT8 hi   = s_shadowYM2413[c][0x20 + ch];
        UINT8 vol  = s_shadowYM2413[c][0x30 + ch] & 0x0F;
        int fnum2413 = ((hi & 0x01) << 8) | lo;
        int blk2413  = (hi >> 1) & 0x07;
        int nt = (fnum2413 > 0) ? FrequencyToMIDINote(
            (float)fnum2413 * (float)clk / (float)(72 * (1 << (19 - blk2413)))) : -1;
        ChVizState& vs = s_ym2413_viz[c][ch];
        if (vs.key_on_event) {
            vs.decay = 1.0f;
            vs.key_on_event = false;
        } else {
            vs.decay *= 0.80f;
            if (vs.decay < 0.01f) vs.decay = 0.0f;
        }
        float lv = (1.0f - vol / 15.0f) * vs.decay;
        bool kon = vs.key_on;
        add("F", ch+1, lv, IM_COL32(80,200,80,255), kon, nt);
    }
    
    // Rhythm channels (always show all 5, even if rhMode is false)
    static const char* kRh2413[5] = {"BD","HH","SD","TM","CY"};
    UINT8 bdVol  = (s_shadowYM2413[c][0x36] >> 4) & 0x0F;
    UINT8 hhVol  = (s_shadowYM2413[c][0x37] >> 4) & 0x0F;
    UINT8 sdVol  =  s_shadowYM2413[c][0x37] & 0x0F;
    UINT8 tomVol = (s_shadowYM2413[c][0x38] >> 4) & 0x0F;
    UINT8 cymVol =  s_shadowYM2413[c][0x38] & 0x0F;
    UINT8 rhVols[5] = {bdVol, hhVol, sdVol, tomVol, cymVol};
    
    for (int i = 0; i < 5; i++) {
        ChVizState& rv = s_ym2413Rhy_viz[c][i];
        if (rhMode && rv.key_on_event) {
            rv.decay = 1.0f;
            rv.key_on_event = false;
        } else {
            rv.decay *= 0.80f;
            if (rv.decay < 0.01f) rv.decay = 0.0f;
        }
        float lv = (1.0f - rhVols[i] / 15.0f) * rv.decay;
        add(kRh2413[i], 0, lv, IM_COL32(200,150,80,255), rv.decay > 0.0f, -1);
    }
    break;
}
```

#### 关键改动说明

1. **移除条件判断**：不再检查 `if (rhMode)`
2. **总是循环 5 次**：添加所有 5 个节奏通道
3. **条件衰减**：只在 `rhMode && key_on_event` 时重置衰减，否则继续衰减

---

## 前端 RenderScopeArea 层修改

### 文件：`vgm_window.cpp`

#### 问题 1：缺少芯片映射

原始代码的 switch 语句缺少 YM2413 和其他芯片的映射：

```c
// 原始代码（不完整）
switch (chipType) {
    case DEVID_YM2151:  scopeName = "YM2151";  break;
    case DEVID_YM2203:  scopeName = "YM2203";  break;
    // ... 缺少 YM2413 等
    default: break;
}
```

#### 问题 2：节奏通道映射错误

原始代码使用通用的通道映射逻辑：

```c
// 原始代码（错误）
int vc = voice_ofs + localCh;
```

对于 YM2413，这会导致节奏通道（localCh 9-13）映射到错误的位置。

#### 解决方案

**修改 1**：添加完整的芯片映射

**文件**：`vgm_window.cpp`  
**位置 1**：第 4019-4065 行（`RenderLevelMeterArea()` 中）  
**位置 2**：第 4287-4369 行（`RenderScopeArea()` 中）

```cpp
// Map dev type to scope registration name
switch (chipType) {
    case DEVID_SN76496:  scopeName = "SN76489";  break;
    case DEVID_YM2413:   scopeName = "YM2413";   break;
    case DEVID_YM2612:   scopeName = "YM2612";   break;
    case DEVID_YM2151:   scopeName = "YM2151";   break;
    case DEVID_SEGAPCM:  scopeName = "SEGAPCM";  break;
    case DEVID_RF5C68:   scopeName = "RF5C68";   break;
    case DEVID_YM2203:   scopeName = "YM2203";   break;
    case DEVID_YM2608:   scopeName = "YM2608";   break;
    case DEVID_YM2610:   scopeName = "YM2610";   break;
    case DEVID_YM3812:   scopeName = "OPL2";     break;
    case DEVID_YM3526:   scopeName = "OPL2";     break;
    case DEVID_Y8950:    scopeName = "Y8950";    break;
    case DEVID_YMF262:   scopeName = "YMF262";   break;
    case DEVID_YMF278B:  scopeName = "YMF278B";  break;
    case DEVID_YMF271:   scopeName = "YMF271";   break;
    case DEVID_YMZ280B:  scopeName = "YMZ280B";  break;
    case DEVID_32X_PWM:  scopeName = "32X_PWM";  break;
    case DEVID_AY8910:   scopeName = "SSG";      break;
    case DEVID_GB_DMG:   scopeName = "GB_DMG";   break;
    case DEVID_NES_APU:  scopeName = "NES_APU";  break;
    case DEVID_YMW258:   scopeName = "MultiPCM"; break;
    case DEVID_uPD7759:  scopeName = "uPD7759";  break;
    case DEVID_OKIM6258: scopeName = "OKIM6258"; break;
    case DEVID_OKIM6295: scopeName = "OKIM6295"; break;
    case DEVID_K051649:  scopeName = "SCC";      break;
    case DEVID_K054539:  scopeName = "K054539";  break;
    case DEVID_C6280:    scopeName = "C6280";    break;
    case DEVID_C140:     scopeName = "C140";     break;
    case DEVID_C219:     scopeName = "C219";     break;
    case DEVID_K053260:  scopeName = "K053260";  break;
    case DEVID_POKEY:    scopeName = "POKEY";    break;
    case DEVID_QSOUND:   scopeName = "QSOUND";   break;
    case DEVID_SCSP:     scopeName = "SCSP";     break;
    case DEVID_WSWAN:    scopeName = "WSWAN";    break;
    case DEVID_VBOY_VSU: scopeName = "VBOY_VSU"; break;
    case DEVID_SAA1099:  scopeName = "SAA1099";  break;
    case DEVID_ES5503:   scopeName = "ES5503";   break;
    case DEVID_ES5506:   scopeName = "ES5506";   break;
    case DEVID_X1_010:   scopeName = "X1_010";   break;
    case DEVID_C352:     scopeName = "C352";     break;
    case DEVID_GA20:     scopeName = "GA20";     break;
    case DEVID_MIKEY:    scopeName = "MIKEY";    break;
    case DEVID_K007232:  scopeName = "K007232";  break;
    case DEVID_K005289:  scopeName = "K005289";  break;
    case DEVID_MSM5205:  scopeName = "MSM5205";  break;
    default: break;
}
```

**修改 2**：添加 YM2413 特殊通道映射

**文件**：`vgm_window.cpp`  
**行号**：4380-4410（`RenderScopeArea()` 中的通道映射循环）

```cpp
for (int k = i; k < j; k++) {
    int localCh = k - i;
    if (voice_ofs >= 0) {
        int totalVoices = GetModizerVoiceCount(
            devIdx < (int)devs.size() ? devs[devIdx].type : 0);
        int vc = voice_ofs + localCh;
        UINT8 chipType = devIdx < (int)devs.size() ? devs[devIdx].type : 0;
        bool isSSGCh = false;
        int ssgLocalCh = 0;
        
        // Handle SSG channels for OPN family
        if (chipType == DEVID_YM2203 && localCh >= 3) {
            isSSGCh = true; ssgLocalCh = localCh - 3;
        } else if (chipType == DEVID_YM2608 && localCh >= 6 && localCh < 9) {
            isSSGCh = true; ssgLocalCh = localCh - 6;
        } else if (chipType == DEVID_YM2610 && localCh >= 4 && localCh < 7) {
            isSSGCh = true; ssgLocalCh = localCh - 4;
        }
        
        if (isSSGCh) {
            ScopeChipSlot *ssgSlot = scope_find_slot("SSG", chipInst);
            if (ssgSlot) vc = ssgSlot->slot_base + ssgLocalCh;
            else vc = -1;
        } else if (chipType == DEVID_YM2608 && localCh >= 9) {
            // YM2608: FM(0-5) SSG(6-8 in frontend) ADPCM-A(9-14) ADPCM-B(15)
            // Core slot: FM(0-5) ADPCM-A(6-11) ADPCM-B(12) — no SSG gap
            vc = voice_ofs + localCh - 3;
        } else if (chipType == DEVID_YM2610 && localCh >= 7) {
            // YM2610: FM(0-3) SSG(4-6 in frontend) ADPCM-A(7-12) ADPCM-B(13)
            // Core slot: FM(0-3) ADPCM-A(4-9) ADPCM-B(10) — no SSG gap
            vc = voice_ofs + localCh - 3;
        } else if (chipType == DEVID_YM2413 && localCh >= 9) {
            // YM2413: FM channels 0-8, Rhythm channels 9-13
            // Rhythm channels map to voice buffer indices 9-13
            vc = voice_ofs + localCh;
        }
        
        meters[k].voice_ch = (localCh < totalVoices && vc >= 0) ? vc : -1;
    } else {
        meters[k].voice_ch = -1;
    }
}
```

#### 关键改动说明

1. **完整的芯片映射**：覆盖所有 44 个 VGM 支持的芯片
2. **YM2413 特殊处理**：节奏通道（localCh >= 9）直接映射到 `voice_ofs + localCh`
3. **通道验证**：确保 `localCh < totalVoices` 且 `vc >= 0`

---

## 完整修改清单

### 1. 仿真核心层

| 文件 | 行号 | 修改内容 |
|------|------|---------|
| `libvgm-modizer/emu/cores/emu2413.c` | 1642-1658 | 固定通道数为最大值（14），总是创建两个通道组 |
| `libvgm-modizer/emu/cores/emu2413.c` | 1100-1117 | 修复节奏通道缓冲区映射，直接映射 i=9-13 到 jj=9-13 |
| `libvgm-modizer/emu/cores/emu2413.c` | 1154-1159 | 更新所有 14 个通道的缓冲区指针（不仅仅是前 9 个） |

### 2. 前端 BuildLevelMeters 层

| 文件 | 行号 | 修改内容 |
|------|------|---------|
| `vgm_window.cpp` | 2939-2964 | 总是添加 5 个节奏通道，即使 rhMode 为 false |

### 3. 前端 RenderScopeArea 层

| 文件 | 行号 | 修改内容 |
|------|------|---------|
| `vgm_window.cpp` | 4019-4065 | 添加完整的 44 个芯片的 scopeName 映射（RenderLevelMeterArea） |
| `vgm_window.cpp` | 4287-4369 | 添加完整的 44 个芯片的 scopeName 映射（RenderScopeArea） |
| `vgm_window.cpp` | 4380-4410 | 添加 YM2413 特殊通道映射逻辑 |

### 4. 头文件更新

| 文件 | 行号 | 修改内容 |
|------|------|---------|
| `libvgm-modizer/emu/SoundDevs.h` | 45-48 | 添加缺失的芯片定义（K007232、K005289、MSM5205） |

---

## 测试验证

### 测试场景 1：YM2413 示波器显示

**步骤**：
1. 加载包含 YM2413 的 VGM 文件
2. 启用示波器显示
3. 验证示波器框显示 14 个通道（9 FM + 5 Rhythm）

**预期结果**：
- 示波器框正常显示
- 9 个 FM 通道显示为 "F1" 到 "F9"
- 5 个节奏通道显示为 "BD"、"HH"、"SD"、"TM"、"CY"

### 测试场景 2：窗口数量稳定性

**步骤**：
1. 加载 YM2413 VGM 文件
2. 播放音乐
3. 观察示波器窗口数量是否变化

**预期结果**：
- 示波器窗口数量始终为 14
- 即使节奏通道开启/关闭，窗口数量不变

### 测试场景 3：通道映射正确性

**步骤**：
1. 加载 YM2413 VGM 文件
2. 播放音乐
3. 观察节奏通道的示波器位置

**预期结果**：
- 节奏通道显示在正确的位置（9-13）
- 不显示在错误的位置（7-8-9）

### 测试场景 4：其他芯片兼容性

**步骤**：
1. 加载包含其他芯片的 VGM 文件（YM2612、YM2203 等）
2. 验证示波器正常显示

**预期结果**：
- 所有支持的芯片示波器正常显示
- 没有新的 Bug 引入

---

## 构建和部署

### 构建命令

```bash
cd YM2163_Piano_v15_Release
bash build_v15.sh
```

### 验证构建

```bash
# 检查是否有编译错误
bash build_v15.sh 2>&1 | grep -i "error"

# 检查是否构建成功
bash build_v15.sh 2>&1 | grep "Build successful"
```

---

## 总结

YM2413 示波器的完整实现涉及仿真核心层的三个关键修改：

1. **固定通道注册**（行 1642-1658）：总是注册 14 个通道（9 FM + 5 Rhythm），确保窗口数量不变
2. **正确的缓冲区映射**（行 1100-1117）：节奏通道 i=9-13 直接映射到缓冲区位置 jj=9-13
3. **完整的指针更新**（行 1154-1159）：更新所有 14 个通道的缓冲区指针

以及前端层的两个修改：

4. **完整的芯片映射**（vgm_window.cpp 行 4019-4065 和 4287-4369）：覆盖所有 44 个 VGM 支持的芯片
5. **正确的通道映射**（vgm_window.cpp 行 4380-4410）：YM2413 节奏通道映射到正确的缓冲区位置

这个设计确保了：
- ✅ 示波器框正常显示
- ✅ 窗口数量保持稳定
- ✅ 节奏通道数据写入到正确的缓冲区位置
- ✅ 节奏通道显示在正确的示波器位置（9-13）
- ✅ 其他芯片兼容性不受影响

---

## Bugfix：chip_inst 不匹配导致有窗口无波形

**日期**：2026-04-05

### 问题

YM2413 示波器显示窗口框但无波形数据。

### 根本原因

`scope_register_chip()` 的 `chip_inst` 参数使用了指针地址：

```c
int chip_inst = (int)((uintptr_t)opll & 0xFF);
```

而前端 `RenderScopeArea()` 使用 VGM 设备的 instance 值（通常为 0）：

```cpp
chipInst = (int)devs[devIdx].instance;
```

两者不一致，`scope_find_chip("YM2413", chipInst)` 无法匹配到核心注册的 slot → 找不到缓冲区偏移 → 无波形。

### 修复

**文件**：`libvgm-modizer/emu/cores/emu2413.c`（行 1646）

```c
// 修改前
int chip_inst = (int)((uintptr_t)opll & 0xFF);
ScopeChipSlot *scope_slot = scope_register_chip("YM2413", chip_inst, num_channels, m_voice_current_samplerate);

// 修改后
ScopeChipSlot *scope_slot = scope_register_chip("YM2413", 0, num_channels, m_voice_current_samplerate);
```

所有已适配的芯片（SAA1099、GB_DMG、C6280、NES_APU、SEGAPCM、OKIM6258、OKIM6295）都使用 `chip_inst = 0`，与前端 `dev.instance` 一致。

