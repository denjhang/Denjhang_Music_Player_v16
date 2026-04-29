# OPL 系列示波器支持 + 寄存器显示优化

**日期**: 2026-04-04
**版本**: v15

## 概述

1. 为整个 OPL 系列芯片添加示波器支持，包括 YM3526 (OPL)、YM3812 (OPL2)、Y8950 (OPL2+ADPCM)、YMF262 (OPL3)。
2. 实现示波器自动换行功能：当总宽度超过 990 像素时自动换行显示。
3. **OPL 系列通道标题改为表格显示**：标题只显示 "CH1"、"CH2" 等，Note/FB/CN/Pan 等信息通过表格显示。

## 问题诊断

### 初始问题
OPL 系列芯片示波器窗口不显示任何波形。

### 根本原因
通过 VGM Debug Log 输出确认：
```
[SCOPE_DBG] YM3812 core=AdLibEmu (FCC=0x41444C45)
```

OPL 芯片默认使用 **AdLibEmu** 核心，而不是 MAME 核心。原因是 `libvgm-modizer/emu/cores/oplintf.c` 中 `devDefList_YM3812[]` 数组的定义：

```c
const DEV_DEF* devDefList_YM3812[] = {
#ifdef EC_YM3812_ADLIBEMU
    &devDef3812_AdLibEmu,  // default, because it's better than MAME
#endif
    ...
};
```

当 `emuCore == 0`（默认值）时，系统选择数组第一个可用核心，即 AdLibEmu。因此 fmopl.c（MAME 核心）中的 `scope_register_chip` 调用永远不会执行。

## 解决方案

### 1. AdLibEmu 核心添加 scope 支持

修改 `libvgm-modizer/emu/cores/adlibemu_opl_inc.c`：

- 将旧的 `m_voice_ChipID` 线性搜索替换为 `scope_register_chip` 调用
- 根据 `OPLTYPE_IS_OPL3` 区分 OPL2 和 OPL3

```c
#if defined(OPLTYPE_IS_OPL3)
    ScopeChipSlot *scope_slot = scope_register_chip("YMF262", 0, 18, m_voice_current_samplerate);
    ...
#else
    ScopeChipSlot *scope_slot = scope_register_chip("OPL2", 0, 9, m_voice_current_samplerate);
    ...
#endif
```

### 2. 构建系统修改

**CMakeLists.txt**: 添加 adlibemu_opl2.c 和 adlibemu_opl3.c 到 scope_core_lib

```cmake
add_library(scope_core_lib STATIC
    ...
    ${SCOPE_CORE_DIR}/adlibemu_opl2.c
    ${SCOPE_CORE_DIR}/adlibemu_opl3.c
)
```

**从预编译库移除对应 .o**:
```bash
ar d libvgm-modizer/build/bin/libvgm-emu.a adlibemu_opl2.c.o adlibemu_opl3.c.o
```

### 3. 前端映射

**vgm_window.cpp** 添加 OPL 系列芯片的支持：

```cpp
// GetModizerVoiceCount
case DEVID_YM3812:  return 9;   // 9 FM (OPL2)
case DEVID_YM3526:  return 9;   // 9 FM (OPL)
case DEVID_Y8950:   return 10;  // 9 FM + 1 ADPCM
case DEVID_YMF262:  return 18;  // 18 FM (OPL3)

// RenderScopeArea scopeName 映射
case DEVID_YM3812:  scopeName = "OPL2";     break;
case DEVID_YM3526:  scopeName = "OPL2";     break;
case DEVID_Y8950:   scopeName = "Y8950";    break;
case DEVID_YMF262:  scopeName = "YMF262";   break;
```

## 示波器自动换行功能

### 需求
当示波器总宽度超过 990 像素时自动换行显示。

### 实现逻辑

**核心原则**：
1. 不同芯片的通道保持在一起（不会将同一芯片拆分）
2. 不同芯片可以放在同一行（如果宽度允许）
3. 如果单个芯片宽度超过 990px，则将其拆分到多行

**实现位置**: `vgm_window.cpp` 的 `RenderScopeArea()`

```cpp
float maxWidth = 990.0f;

// 按 chip group 分配到行
for (int g = 0; g < groups.size(); g++) {
    const ChipGroup& grp = groups[g];

    // 单个芯片宽度超过 maxWidth 时拆分
    if (grp.width > maxWidth) {
        int maxChPerRow = (int)((maxWidth + kBarGap) / (s_scopeWidth + kBarGap));
        // 拆分到多行
    } else {
        // 正常情况：检查是否需要新行
        if (rows.back().width + grp.width + kGroupGap > maxWidth) {
            rows.push_back({});
        }
    }
}
```

## 文件修改列表

| 文件 | 修改内容 |
|------|----------|
| `libvgm-modizer/emu/cores/adlibemu_opl_inc.c` | 替换 `m_voice_ChipID` 搜索为 `scope_register_chip`，修复 include 路径 |
| `CMakeLists.txt` | 添加 adlibemu_opl2.c、adlibemu_opl3.c 到 scope_core_lib |
| `vgm_window.cpp` | 添加 OPL 芯片映射、示波器自动换行逻辑、调试日志 |
| `libvgm-modizer/build/bin/libvgm-emu.a` | 移除 adlibemu_opl2.c.o、adlibemu_opl3.c.o |

## 测试验证

### VGM Debug Log 输出示例
```
[SCOPE_DBG] YM3812 core=AdLibEmu (FCC=0x41444C45)
[SCOPE_LAYOUT] availW=1036 s_scopeWidth=150 maxWidth=990 maxChPerRow=6
[SCOPE_LAYOUT] totalChannels=12 groups=1 numRows=2
[SCOPE_LAYOUT] row0: 1 groups, width=910 px
[SCOPE_LAYOUT] row1: 1 groups, width=910 px
```

### 验证点
- [x] YM3526 (OPL) 示波器可见
- [x] YM3812 (OPL2) 示波器可见
- [x] Y8950 (OPL2+ADPCM) 示波器可见，显示 10 通道
- [x] YMF262 (OPL3) 示波器可见，显示 18 通道
- [x] 单个芯片宽度超过 990px 时自动换行
- [x] 多个不同芯片可以放在同一行（如果宽度允许）
- [x] OPL 系列通道标题使用表格显示（Note/FB/CN/Pan）

## OPL 系列通道标题表格化

### 修改前
TreeNode 标题包含所有信息：
```cpp
snprintf(chLabel, "CH%d %s FB%d CN%d %s##opl3ch%d",
    chIdx+1, notePadOpl3, fb, cnt, pan_str4[pan], chIdx, c);
// 结果: "CH1 B6  FB0 CN0 LR"
```

### 修改后
TreeNode 标题只显示通道号，内部表格显示详细信息：
```cpp
snprintf(chLabel, "CH%d##opl3ch%d", chIdx+1, chIdx);
ImGui::BeginTable("##opl3info", 4, ...)
// Note | FB | CN | Pan
// B6     0    0   LR
```

### 参考实现
- YM2203 (OPN): TreeNode "FM1"，表格 Note/AL/FB
- YMF262 (OPL3): TreeNode "CH1"，表格 Note/FB/CN/Pan
- Y8950: TreeNode "CH1"，表格 Note/FB/CN
- YM3812/YM3526: TreeNode "CH1"，表格 Note/FB/CN

## 技术要点

### libvgm 多核心架构
libvgm 支持多种仿真核心，通过 `devDefList_XXX[]` 数组定义优先级：
- 数组第一个元素是默认核心（emuCore=0 时）
- AdLibEmu 对 OPL 芯片通常排第一位，因为被认为比 MAME 更准确

### scope_register_chip 系统
v15 项目使用新的 `scope_register_chip()` 系统替代旧的 `m_voice_ChipID` 线性搜索：
- 优点：O(1) 查找，支持动态设备管理
- 缺点：需要修改每个芯片核心的代码

### 增量编译覆盖
通过 CMake `--whole-archive` + `ar d` 从预编译库移除对应 .o，实现增量编译覆盖。

## 相关文档

- [v15 示波器添加指南](v15_oscilloscope_guide.md)
- [反馈与协作偏好](feedback.md)
- [示波器开发关键点](scope_development_key_points.md)
