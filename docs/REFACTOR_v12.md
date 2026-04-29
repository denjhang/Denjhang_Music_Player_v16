# YM2163 Piano GUI v12 - 模块化重构文档

## 概述

本次重构将 v11 的单体文件 `ym2163_piano_gui_v11.cpp`（5107行）拆分为多个模块，
提升代码可维护性和可读性。构建系统从 shell 脚本迁移至 CMake。

**重构日期**: 2026年3月28日
**基础版本**: v11（5107行单体文件）
**目标版本**: v12（模块化，CMake 构建）

---

## 模块划分

| 文件 | 命名空间 | 职责 | 大致行数 |
|---|---|---|---|
| `ym2163_control.h/.cpp` | `YM2163::` | FTDI 通信、芯片初始化、音符/鼓声控制、通道管理、速度映射 | ~660行 |
| `config_manager.h/.cpp` | `Config::` | INI 文件读写（频率表、乐器配置、MIDI 配置、Slot 配置） | ~300行 |
| `midi_player.h/.cpp` | `MidiPlayer::` | MIDI 文件解析与播放、文件浏览器、媒体键、时序引擎 | ~700行 |
| `gui_renderer.h/.cpp` | global | DX11 初始化、WndProc、键盘处理、辅助渲染函数 | ~300行 |
| `gui_renderer_impl.cpp` | global | 7个 ImGui 渲染函数实现（`#include` 进 gui_renderer.cpp） | ~1040行 |
| `main.cpp` | global | WinMain 入口、DPI 设置、窗口创建、ImGui 初始化、主循环 | ~235行 |
| `CMakeLists.txt` | — | CMake 构建配置（替代 build_v12.sh） | ~60行 |

---

## 构建方式

```bash
mkdir build && cd build
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

输出文件：`build/ym2163_piano_gui_v12.exe`

编译器：g++ 15.2.0（MSYS2 MinGW64）
CMake：4.1.2
标准：C++11

---

## 主要技术决策

### 1. `gui_renderer_impl.cpp` 用 `#include` 方式引入

7个渲染函数体量约1040行，直接放入 `gui_renderer.cpp` 会超过单次编辑上限。
采用在 `gui_renderer.cpp` 末尾 `#include "gui_renderer_impl.cpp"` 的方式，
使渲染函数共享 `gui_renderer.cpp` 的作用域，同时保持文件独立可读。

### 2. C++ 命名空间封装

| v11（全局） | v12 |
|---|---|
| `g_ftHandle` | `YM2163::g_ftHandle` |
| `g_midiPlayer` | `MidiPlayer::g_midiPlayer` |
| `LoadFrequenciesFromINI()` | `Config::LoadFrequenciesFromINI()` |

### 3. `static` 变量提升为 `extern`

原 v11 中部分变量为文件级 `static`，模块化后需跨文件访问，
统一移除 `static`，在对应头文件中添加 `extern` 声明：

- `YM2163::g_drumActive[MAX_CHIPS][5]`
- `YM2163::g_manualDisconnect`
- `YM2163::g_velocityAnalysis`
- `YM2163::g_noteNames[12]`、`g_isBlackNote[12]`、`g_drumNames[5]`

---

## 编译过程中修复的问题

### 问题1：`expf` 未声明

**文件**：`ym2163_control.cpp`
**原因**：缺少 `<cmath>` 头文件
**修复**：添加 `#include <cmath>`

### 问题2：`MidiPlayer::TIMER_MIDI_UPDATE` 无效

**文件**：`gui_renderer.cpp`
**原因**：`TIMER_MIDI_UPDATE` 是 `#define` 宏，不能加命名空间前缀
**修复**：将 `MidiPlayer::TIMER_MIDI_UPDATE` 改为直接使用 `TIMER_MIDI_UPDATE`

### 问题3：`uint8_t` 未声明

**文件**：`config_manager.h`
**原因**：缺少 `<cstdint>` 头文件
**修复**：添加 `#include <cstdint>`

### 问题4：`ResetYM2163Chip` / `ResetAllYM2163Chips` 未实现

**文件**：`ym2163_control.cpp`
**原因**：这两个函数在 `ym2163_control.h` 中声明，但实现未从 v11 移植
**修复**：从 `ym2163_piano_gui_v11.cpp`（第1372-1432行）移植实现到 `ym2163_control.cpp` 末尾

### 问题5：`config_manager.h` 缺少 `goto` 跳过变量声明（C++ 非法）

**文件**：`gui_renderer_impl.cpp`（`RenderMIDIPlayer` 函数）
**原因**：v11 中使用 `goto skip_progress` 跳过了局部变量声明，C++ 不允许
**修复**：改为嵌套 `if` 块结构

---

## v12 相对 v11 的新增内容

| 新增项 | 位置 | 说明 |
|---|---|---|
| `ChannelState::hasBeenUsed` | `ym2163_control.h` | 用于电平表显示优化 |
| `ChannelState::currentLevel` | `ym2163_control.h` | 实时电平值 |
| `VelocityAnalysis::reset()` | `ym2163_control.h` | 速度分析数据重置方法 |
| `FormatTime(double)` | `midi_player.h/.cpp` | 进度条时间格式化辅助函数 |
| Numpad 1-5 鼓点触发 | `gui_renderer.cpp` | `HandleKeyPress` 中新增小键盘触发鼓声 |

---

## 功能完整性验证

经逐项对比，v12 包含 v11 的全部功能：

- FTDI 硬件通信与自动重连
- 4芯片（Slot0-3）、16通道旋律音符
- Slot3 2MHz 模式高八度扩展
- 5路打击乐（BD/HC/SDN/HHO/HHD）
- MIDI 文件播放（高精度时序，QueryPerformanceCounter）
- 进度条可点击跳转
- 文件浏览器（Unicode、面包屑导航、历史记录）
- 全局媒体键（播放/暂停/上一首/下一首）
- Sustain 踏板支持
- 动态速度映射（4级：0dB/-6dB/-12dB/静音）
- INI 配置文件（频率表、乐器、MIDI 映射、Slot 配置）
- ImGui + DirectX11 GUI（中日韩字体支持）
- 钢琴键盘可视化（73键）
- 通道电平表
- 频率调音窗口
