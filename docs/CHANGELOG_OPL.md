# OPL 系列示波器支持 + 寄存器显示优化

**日期**: 2026-04-04
**版本**: v15

## 概述

1. 为整个 OPL 系列芯片添加示波器支持，包括 YM3526 (OPL)、YM3812 (OPL2)、Y8950 (OPL2+ADPCM)、YMF262 (OPL3)。
2. 实现示波器自动换行功能：当总宽度超过 990 像素时自动换行显示。
3. 移除所有芯片的单行文本显示方式，统一使用表格。
4. **OPL 系列通道标题改为表格显示**：标题只显示 "CH1"、"CH2" 等，Note/FB/CN/Pan 等信息通过表格显示。

## 问题诊断

### 初始问题
OPL 系列芯片示波器窗口不显示任何波形。

### 根本原因
通过 VGM Debug Log 输出确认：
```
[SCOPE_DBG] YM3812 core=AdLibEmu (FCC=0x41444C45)
```

OPL 芯片默认使用 **AdLibEmu** 核心，而不是 MAME 核心。原因是 `oplintf.c` 中 `devDefList_YM3812[]` 数组将 AdLibEmu 排在第一位作为默认核心。因此 fmopl.c（MAME 核心）中的 `scope_register_chip` 调用永远不会执行。

## 解决方案

### 1. AdLibEmu 核心添加 scope 支持
修改 `adlibemu_opl_inc.c`，将旧的 `m_voice_ChipID` 线性搜索替换为 `scope_register_chip` 调用。

### 2. 构建系统修改
- CMakeLists.txt 添加 adlibemu_opl2.c 和 adlibemu_opl3.c 到 scope_core_lib
- 从预编译库移除对应 .o 文件

### 3. 前端映射
vgm_window.cpp 添加 OPL 系列芯片的 GetModizerVoiceCount 和 scopeName 映射。

## 示波器自动换行功能

**规则**：
- 不同芯片的通道保持在一起
- 不同芯片可以放在同一行（如果宽度 ≤ 990px）
- 如果单个芯片宽度超过 990px，则将其拆分到多行

**实现**：vgm_window.cpp 的 RenderScopeArea() 中按 chip group 分配到行。

## 文件修改列表

| 文件 | 修改内容 |
|------|----------|
| `libvgm-modizer/emu/cores/adlibemu_opl_inc.c` | 替换 m_voice_ChipID 搜索为 scope_register_chip |
| `CMakeLists.txt` | 添加 adlibemu_opl2.c、adlibemu_opl3.c |
| `vgm_window.cpp` | OPL 芯片映射、自动换行逻辑、通道标题表格化 |
| `libvgm-emu.a` | 移除 adlibemu_opl2.c.o、adlibemu_opl3.c.o |

## OPL 系列通道标题表格化

### 修改前
```
CH1 B6  FB0 CN0 LR
├── OP 参数表
```

### 修改后
```
CH1
├── Note | FB | CN | Pan  (通道信息表)
│   B6     0    0   LR
├── OP 参数表
```

### 参考实现
- YM2203 (OPN): TreeNode 标题 "FM1"，内部表格 Note/AL/FB
- YMF262 (OPL3): TreeNode 标题 "CH1"，内部表格 Note/FB/CN/Pan
- Y8950: TreeNode 标题 "CH1"，内部表格 Note/FB/CN
- YM3812/YM3526: TreeNode 标题 "CH1"，内部表格 Note/FB/CN

## 验证点
- [x] YM3526/YM3812/Y8950/YMF262 示波器可见
- [x] 单个芯片宽度超过 990px 时自动换行
- [x] 多个不同芯片可以放在同一行（如果宽度允许）
- [x] OPL 系列通道标题使用表格显示（Note/FB/CN/Pan）
