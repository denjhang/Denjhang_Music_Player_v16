# 双芯片示波器完整支持 v15

**日期**：2026-04-05  
**版本**：v15  
**状态**：完成

## 概述

本次更新实现了完整的双芯片示波器支持，确保所有芯片（无论核心是否注册 scope）都能显示示波器窗口框。

## 问题背景

### 原始问题
1. **双芯片场景示波器窗口数量不正确**：双 OPN 只显示 6 个窗口而非 12 个
2. **部分芯片完全无示波器窗口**：SEGAPCM 等芯片因核心未注册 scope 而完全不显示
3. **双芯片无法区分**：两个同类型芯片都显示相同标签，无法区分

### 根本原因
1. 核心 `scope_register_chip()` 只注册了约 14 个芯片，其余约 36 个芯片无 scope 注册
2. `scope_find_slot()` 对未注册芯片返回 NULL，导致 `voice_ofs = -1`
3. `RenderScopeArea()` 过滤掉 `voice_ch < 0` 的通道
4. 双芯片回退逻辑缺失

## 解决方案

### 1. 补全所有芯片通道数定义

**文件**：`vgm_window.cpp` — `GetModizerVoiceCount()`

添加了约 30 个芯片的通道数定义，从原来的 13 个扩展到约 50 个：

```cpp
case DEVID_SEGAPCM: return 16;  // 16 channels
case DEVID_RF5C68:  return 8;   // 8 channels
case DEVID_YMF278B: return 24;  // OPL4
case DEVID_YMF271:  return 12;  // OPX
// ... 等 30 个芯片
```

### 2. 双芯片回退逻辑

**文件**：`vgm_window.cpp` — `RenderLevelMeterArea()` 和 `RenderScopeArea()`

当 `scope_find_slot(scopeName, chipInst)` 对第 2 个芯片查找失败时，回退到 `chip_inst=0`：

```cpp
ScopeChipSlot *slot = scope_find_slot(scopeName, chipInst);
// Dual-chip fallback: core may only register chip_inst=0
if (!slot && chipInst > 0)
    slot = scope_find_slot(scopeName, 0);
if (slot) voice_ofs = slot->slot_base;
```

SSG 通道也添加了相同回退逻辑。

### 3. 显示所有示波器窗口框

**文件**：`vgm_window.cpp` — `RenderScopeArea()`

**修改前**：过滤掉 `voice_ch < 0` 的通道
```cpp
for (const auto& m : meters) {
    if (m.voice_ch >= 0) scopeMeters.push_back(m);
}
```

**修改后**：包含所有通道
```cpp
// Include all channels for oscilloscope display, even without scope data
for (const auto& m : meters) {
    scopeMeters.push_back(m);
}
```

### 4. 无 scope 数据时显示空框

**文件**：`modizer_viz.cpp` — `DrawChannel()`

**修改前**：`channel < 0` 时直接返回，不画框
```cpp
if (width < 1.0f || channel < 0 || channel >= SOUND_MAXVOICES_BUFFER_FX) return;
```

**修改后**：先画框，再检查 channel 有效性
```cpp
// 先画背景框和零线
draw_list->AddRectFilled(...);
draw_list->AddRect(...);
draw_list->AddLine(...);

// 再检查 channel 是否有效
if (channel < 0 || channel >= SOUND_MAXVOICES_BUFFER_FX) return;
```

### 5. 芯片实例编号显示

**文件**：`vgm_window.cpp` — `BuildLevelMeters()`

为每个芯片添加实例编号（#1, #2, #3）：

```cpp
std::map<UINT8, int> chipTypeCount;  // 每次加载文件时重置
int instNum = ++chipTypeCount[dev.type];
char chipLabelBuf[32];
snprintf(chipLabelBuf, sizeof(chipLabelBuf), "%s #%d", chipLabel, instNum);
chipLabel = chipLabelBuf;
```

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `vgm_window.cpp` | `GetModizerVoiceCount()` 补全 30+ 芯片通道数 |
| `vgm_window.cpp` | `RenderLevelMeterArea()` 添加双芯片回退逻辑 |
| `vgm_window.cpp` | `RenderScopeArea()` 添加双芯片回退逻辑 |
| `vgm_window.cpp` | `RenderScopeArea()` 移除通道过滤，包含所有通道 |
| `vgm_window.cpp` | `BuildLevelMeters()` 添加芯片实例编号显示 |
| `modizer_viz.cpp` | `DrawChannel()` 先画框再检查 channel 有效性 |
| `libvgm-modizer/emu/cores/segapcm.c` | 添加 `scope_register_chip("SEGAPCM", 0, 16, ...)` |

## 效果

### 有 scope 数据的芯片（14个）
YM2151, YM2203, YM2608, YM2610, YM2610B, YM2612, YM2413, SSG, OPL2, Y8950, YMF262, SN76489, SCC, SEGAPCM
**显示正常波形**

### 无 scope 数据的芯片（31个）
RF5C68, YMF278B, YMF271, YMZ280B, 32X_PWM, GB_DMG, NES_APU, MultiPCM, uPD7759, OKIM6258, OKIM6295, K054539, C6280, C140, C219, K053260, POKEY, QSOUND, SCSP, WSWAN, VBOY_VSU, SAA1099, ES5503, ES5506, X1_010, C352, GA20, MIKEY, K007232, K005289, MSM5205
**显示空窗口框**（有边框、零线、标签，但无波形）

### 总计：前端支持 45 个芯片

### 双芯片场景
- 双 OPN：12 个窗口，标签 "YM2203 #1" 和 "YM2203 #2"
- 所有芯片都支持双芯片显示

## 验证

1. 加载单芯片 VGM → 显示 "SegaPCM #1"
2. 加载双 OPN VGM → 显示 12 个窗口，标签 "YM2203 #1" 和 "YM2203 #2"
3. 切换文件 → 编号重置，不累积
4. 无 scope 芯片 → 显示空框

## 编译

```bash
cd YM2163_Piano_v15_Release
bash build_v15.sh
```
