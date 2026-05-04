# Denjhang's Music Player v16 - 功能全览与源码定位

> 最后更新：2026-05-04

---

## 目录

1. [项目概览](#1-项目概览)
2. [目录结构](#2-目录结构)
3. [DockSpace 多窗口架构](#3-dockspace-多窗口架构)
4. [YM2163 硬件控制模块](#4-ym2163-硬件控制模块)
5. [MIDI 播放器模块](#5-midi-播放器模块)
6. [YM2163 窗口渲染模块](#6-ym2163-窗口渲染模块)
7. [OPL3 窗口模块](#7-opl3-窗口模块)
8. [VGM/libvgm 播放器模块](#8-vgmlibvgm-播放器模块)
9. [Gigatron Tracker 播放器模块](#9-gigatron-tracker-播放器模块)
10. [YM2413 (OPLL) VGM 播放器模块](#10-ym2413-opll-vgm-播放器模块)
11. [配置管理模块](#11-配置管理模块)
12. [VGM 文件解析模块](#12-vgm-文件解析模块)
13. [可视化模块（Modizer）](#13-可视化模块modizer)
14. [第三方库](#14-第三方库)
15. [构建系统](#15-构建系统)
16. [版本演进](#16-版本演进)

---

## 1. 项目概览

Denjhang's Music Player 是一款 Windows 平台的芯片音乐工作站，核心功能是通过 FTDI USB 接口控制 YM2163 FM 合成芯片硬件。项目同时集成了 MIDI 播放器和 VGM/S98/GYM/DRO 等芯片音乐格式播放器，配备实时示波器和电平表可视化。

### 代码量统计

| 类别 | 行数 | 文件数 |
|------|------|--------|
| 项目自有代码（不含第三方） | ~20,000 | 27 |
| libvgm-modizer（芯片仿真库） | ~169,000 | 275 |
| ImGui（GUI 框架） | ~25,000 | 13 |
| midifile（MIDI 解析库） | ~5,000 | 10 |
| **合计** | **~219,000** | **325+** |

### 核心源文件清单

| 文件 | 行数 | 用途 |
|------|------|------|
| `src/vgm_window.cpp` | 8,340 | VGM 播放器窗口 |
| `src/gigatron_window.cpp` | ~2,700 | Gigatron Tracker 窗口 |
| `src/ym2413_window.cpp` | ~2,800 | YM2413 OPLL VGM 播放器窗口 |
| `src/gui_renderer_impl.cpp` | 1,275 | YM2163 渲染函数实现 |
| `src/midi_player.cpp` | 845 | MIDI 解析与播放引擎 |
| `src/chip_control.cpp` | 705 | YM2163 硬件控制核心 |
| `src/opl3_renderer.cpp` | 600 | OPL3 渲染函数 |
| `src/vgm_parser.cpp` | 466 | VGM/S98 文件解析 |
| `src/main.cpp` | 425 | 程序入口、DockSpace 宿主 |
| `src/modizer_viz.cpp` | 358 | 示波器波形可视化 |
| `src/gui_renderer.cpp` | 292 | DX11 设备、键盘输入、WndProc |
| `src/config_manager.cpp` | 185 | INI 配置读写 |

---

## 2. 目录结构

```
Denjhang_Music_Player_v16/
├── CMakeLists.txt                # CMake 构建配置
├── build_v16.sh                  # v16 构建脚本
├── ftd2xx64.dll                  # FTDI USB 驱动 DLL
│
├── src/                          # 所有项目源码
│   ├── main.cpp                  # 程序入口（WinMain、DockSpace、多 Viewport 渲染）
│   ├── chip_control.cpp/h        # YM2163 硬件控制（FTDI、音符、鼓）
│   ├── chip_window_ym2163.cpp/h  # YM2163 窗口模块（DockSpace 模式）
│   ├── midi_player.cpp/h         # MIDI 播放引擎（解析、定时、播放列表）
│   ├── gui_renderer.cpp/h        # DX11 设备管理、键盘输入、WndProc
│   ├── gui_renderer_impl.cpp     # YM2163 渲染（钢琴、音量条、控制面板、文件浏览器）
│   ├── config_manager.cpp/h      # INI 配置管理（频率、乐器、MIDI映射）
│   ├── opl3_renderer.cpp/h       # OPL3 (YMF262) UI 渲染函数
│   ├── opl3_window.cpp/h         # OPL3 窗口模块（DockSpace 模式）
│   ├── vgm_window.cpp/h          # VGM/libvgm 播放器窗口（8340行）
│   ├── vgm_parser.cpp/h          # VGM/S98 文件解析器
│   ├── modizer_viz.cpp/h         # 示波器波形渲染（Modizer 风格）
│   ├── gigatron_window.cpp/h     # Gigatron Tracker 窗口模块
│   ├── ym2413_window.cpp/h       # YM2413 (OPLL) VGM 播放器窗口
│   ├── gigatron/gigatron_emu.c/h # Gigatron 仿真核心（4通道 TTL 音频）
│   ├── gigatron/winmm.c/h        # WinMM 音频输出
│   ├── gigatron/audio_output.h   # 音频输出接口
│   ├── gigatron/fnum_table.h     # 频率表（96 entries, 8 octaves×12）
│   └── window_interface.h        # 窗口接口（Render() 无参数签名）
│
├── bin/                          # 编译输出 + 运行时配置
│   ├── denjhang_music_player.exe # 可执行文件
│   ├── ym2163_config.ini         # YM2163 窗口配置（频率、Slot、128乐器、MIDI历史）
│   ├── opl3_config.ini           # OPL3 窗口配置（文件夹历史）
│   ├── vgm_config.ini            # VGM 窗口配置（播放器状态、UI、示波器、历史）
│   ├── gigatron_config.ini       # Gigatron 窗口配置（音频、示波器、波形编辑器）
│   ├── ym2413_config.ini         # YM2413 窗口配置（路径、设置、颜色）
│   ├── gigatron_custom_wave.wtab # Gigatron 自定义波形表（二进制，256×uint16）
│   └── imgui.ini                 # DockSpace 布局持久化
│
├── docs/                         # 所有文档
│   ├── FEATURES.md               # 本文件
│   ├── RELEASE_NOTES.md          # 发布说明
│   ├── VERSION_HISTORY.md        # 版本历史
│   └── ... (20+ 文档)
│
├── legacy/                       # v11-v15 旧版文件
│   ├── ym2163_piano_gui_v11.cpp  # v11 历史遗留
│   ├── build_v11.sh ~ build_v15.sh
│   └── *.exe / *.o               # 旧版编译产物
│
├── imgui/                        # ImGui GUI 框架（docking 分支 v1.92.8 WIP）
│   ├── imgui.h/cpp               # 核心库
│   ├── imgui_internal.h          # 内部 API（DockBuilder）
│   ├── imgui_impl_dx11.h/cpp     # DirectX 11 后端
│   ├── imgui_impl_win32.h/cpp    # Win32 后端
│   └── imstb_*.h                 # STB 子库
│
├── midifile/                     # MIDI 文件解析库
│   ├── include/                  # MidiFile.h, MidiEvent.h 等
│   └── src/                      # 对应实现
│
├── ftdi_driver/                  # FTDI USB 驱动
│   ├── amd64/                    # 64位驱动（ftd2xx64.dll, libftd2xx.a）
│   ├── i386/                     # 32位驱动
│   ├── Static/                   # 静态链接库
│   ├── ftd2xx.h                  # API 头文件
│   └── ftdibus.inf               # 驱动安装信息
│
├── libvgm-modizer/               # VGM 播放引擎（Modizer 定制版）
│   ├── emu/                      # 芯片仿真核心
│   │   ├── SoundEmu.c/h          # 声音设备管理
│   │   ├── cores/                # 各芯片仿真实现
│   │   │   ├── 2151intf.c        # YM2151/OPM
│   │   │   ├── 2612intf.c        # YM2612/OPN2
│   │   │   ├── 2203intf.c        # YM2203/OPN
│   │   │   ├── 2608intf.c        # YM2608/OPNA
│   │   │   ├── 2610intf.c        # YM2610
│   │   │   ├── adlibemu_opl2.c   # OPL2 仿真
│   │   │   ├── adlibemu_opl3.c   # OPL3 仿真
│   │   │   ├── ay8910.c          # AY8910/SSG
│   │   │   ├── sn76496.c         # SN76489
│   │   │   ├── k051649.c         # SCC (K051649)
│   │   │   └── ...               # 40+ 芯片
│   │   ├── Resampler.c           # 音频重采样
│   │   └── panning.c             # 声像控制
│   ├── player/                   # 播放器实现
│   │   ├── playera.cpp/hpp       # 主播放器（PlayerA）
│   │   ├── vgmplayer.cpp/hpp     # VGM 格式播放
│   │   ├── s98player.cpp/hpp     # S98 格式播放
│   │   ├── gymplayer.cpp/hpp     # GYM 格式播放
│   │   └── droplayer.cpp/hpp     # DRO 格式播放
│   └── audio/                    # 音频输出驱动
│       ├── AudDrv_WASAPI.cpp     # Windows WASAPI
│       ├── AudDrv_DSound.cpp     # DirectSound
│       ├── AudDrv_XAudio2.cpp    # XAudio2
│       └── AudDrv_WaveWriter.c   # WAV 文件输出
│
├── tools/                        # 调试工具
│   ├── sn76489_dump.cpp/exe      # SN76489 寄存器转储
│   └── saa1099_dump.cpp/exe      # SAA1099 寄存器转储
│
├── vgms/                         # 测试用 VGM 文件
│   ├── saa1099.vgm
│   ├── sn76489-*.vgm
│   ├── Alien3-SMS/
│   ├── Shining_Crystal_(MSX2+)/
│   └── kssorg/
│
└── build/                        # CMake 构建缓存
```

---

## 3. DockSpace 多窗口架构

### 入口：`src/main.cpp`

程序采用 ImGui Docking 分支（v1.92.8 WIP）+ DirectX 11，三个窗口作为 DockSpace 标签页运行。支持拖拽拆分为独立 Windows 系统窗口。

#### ImGui 配置 — `main.cpp:82-98`

```cpp
io.ConfigFlags |= ImGuiConfigFlags_DockingEnable    // 启用 DockSpace
                | ImGuiConfigFlags_ViewportsEnable;  // 启用多 Viewport
io.ConfigViewportsNoDecoration = false;              // 拆分窗口显示 OS 标题栏
```

#### 主循环结构 — `main.cpp`

```
主循环:
  1. PeekMessage 消息泵
  2. SwapChain 遮挡检查 + ResizeBuffers
  3. 所有窗口模块 Update()（硬件轮询、MIDI/VGM 更新）
  4. ImGui NewFrame
  5. DockSpace 渲染 + 所有窗口模块 Render()
  6. ImGui Render + DX11 Present
  7. ImGui::UpdatePlatformWindows() + RenderPlatformWindowsDefault()
  8. Sleep(1) ≈ 高帧率
```

#### DockSpace 初始布局 — `main.cpp`（DockBuilder API）

首次启动时用 DockBuilder 设置默认三栏布局：
- 左侧：`YM2163(DSG)`
- 右侧上：`YMF262(OPL3)`
- 右侧下：`libvgm`

布局由 `imgui.ini`（ImGui 内建功能）自动保存和恢复。

#### 可见性自动暂停/恢复

基于 `ImGui::Begin()` 返回值检测窗口可见性：

| 场景 | 行为 |
|------|------|
| 标签不活动或窗口隐藏 | 自动暂停 MIDI/VGM 播放 |
| 标签重新激活或窗口显示 | 自动恢复播放（除非用户手动暂停） |

关键变量：
- `g_midiUserPaused` (`main.cpp:26`) — 用户是否手动暂停了 MIDI
- `s_midiAutoPaused` (`chip_window_ym2163.cpp`) — MIDI 自动暂停标志

#### 多 Viewport 渲染

```
ImGui::UpdatePlatformWindows();
ImGui::RenderPlatformWindowsDefault();
```

拆分的窗口使用 `WS_OVERLAPPEDWINDOW` 样式，支持 Windows 系统贴靠/Dock 功能。

#### DPI 感知 — `main.cpp:32-50`

优先使用 `SetProcessDpiAwarenessContext(-4)` (Per Monitor Aware V2)，回退到 `SetProcessDpiAwareness(2)`。

#### CJK 字体加载 — `main.cpp`

主字体：微软雅黑 → 宋体 → ImGui 默认。合并字体：Malgun Gothic（韩语）+ MS Gothic（日语）。

#### 窗口模块接口 — `window_interface.h`

```cpp
// 所有窗口模块实现此接口
class WindowModule {
    void Init();
    void Update();
    void Render();  // 无参数签名，自管理 ImGui::Begin/End
};
```

---

## 4. YM2163 硬件控制模块

### 文件：`src/chip_control.h` (157行) / `src/chip_control.cpp` (705行)

### 4.1 芯片与通道常量 — `chip_control.h:19-22`

```cpp
constexpr int MAX_CHANNELS = 16;           // 4 芯片 × 4 通道
constexpr int MAX_CHIPS = 4;               // 最多 4 块 YM2163
constexpr int CHANNELS_PER_CHIP = 4;       // 每块 4 通道
```

### 4.2 通道状态结构 — `chip_control.h:25-44`

```cpp
struct ChannelState {
    int note, octave, fnum;                // 音符、八度、F-Number
    bool active;                           // 是否发声中
    int midiChannel;                       // MIDI 通道映射（-1=未用）
    int timbre, envelope, volume;          // 音色、包络、音量
    int chipIndex;                         // 所属芯片（0-3）
    time_point startTime, releaseTime;     // 起音/释放时间
    bool hasBeenUsed;                      // 是否曾经使用过
    float currentLevel;                    // 当前包络级别（0.0-1.0）
    float previousLevel;                   // 上帧级别（平滑过渡用）
};
```

### 4.3 FTDI USB 通信

#### 连接管理

| 函数 | 行号 | 功能 |
|------|------|------|
| `ftdi_init(dev_idx)` | cpp:127-153 | 打开 FTDI 设备，设置 1.5Mbps 波特率、8N1、2ms 延迟 |
| `ConnectHardware()` | cpp:175-185 | 初始化 FTDI + 初始化所有 YM2163 芯片 |
| `DisconnectHardware()` | cpp:187-196 | 停止所有音符 + 关闭 FTDI |
| `CheckHardwareAutoConnect()` | cpp:198-228 | 每 500ms 轮询硬件，自动重连（最多 6 次重试） |

#### FTDI 数据协议 — `chip_control.cpp:230-247`

YM2163 使用 3 字节 SPI 数据包：`[chipIndex, 0x80, data]`

```cpp
void write_melody_cmd_chip(uint8_t data, int chipIndex) {
    uint8_t buf[3] = { (uint8_t)chipIndex, 0x80, data };
    FT_Write(g_ftHandle, buf, 3, &bytesWritten);
}
```

芯片地址映射：Slot0=0x00, Slot1=0x01, Slot2=0x02, Slot3=0x03

### 4.4 音符控制

#### play_note — `chip_control.cpp:417-485`

发送 5 条寄存器写入命令：

| 寄存器 | 数据 | 功能 |
|--------|------|------|
| 0x88+ch | timbre \| (envelope<<4) | 设置音色/包络 |
| 0x8C+ch | 0x0F \| (volume<<4) | 设置音量 |
| 0x84+ch | octave<<3 \| fnum_high | 设置八度/F高 |
| 0x80+ch | fnum_low | 设置 F低 |
| 0x84+ch | 0x40 \| octave<<3 \| fnum_high | **Key On** |

同时更新钢琴键状态：`g_pianoKeyPressed[keyIdx]=true`, `g_pianoKeyChipIndex[keyIdx]=chipIndex`

#### stop_note — `chip_control.cpp:487-524`

发送 2 条命令（Key Off）：

| 寄存器 | 数据 | 功能 |
|--------|------|------|
| 0x80+ch | fnum_low | 写入 F低 |
| 0x84+ch | octave<<3 \| fnum_high | **Key Off**（无 0x40 位） |

**不立即清除钢琴键状态**，让 `UpdateChannelLevels` 通过衰减自然淡出。

### 4.5 通道分配

| 函数 | 行号 | 策略 |
|------|------|------|
| `find_free_channel()` | cpp:316-337 | 优先找空闲通道；全满则停最早起音的 |
| `find_free_channel_slot3()` | cpp:339-359 | 在通道 12-15 中分配（2MHz 模式专用） |
| `find_channel_playing(note, octave)` | cpp:361-369 | 查找正在播放指定音高的通道 |
| `getNormalChannelUsage()` | cpp:387-394 | 返回正常通道使用率（0.0-1.0） |
| `CleanupStuckChannels()` | cpp:396-405 | 清理 >10 秒的卡死通道 |

### 4.6 ADSR 包络与电平计算

#### CalculateEnvelopeLevel — `chip_control.cpp:569-594`

**Attack/Sustain 阶段（active=true）：**

| Envelope | 名称 | Attack | Sustain 曲线 |
|----------|------|--------|-------------|
| 0 | Decay | 无 | exp(-t × 1.0) |
| 1 | Fast | 0.05s 线性上升 | 0.7+0.3×exp(-(t-0.05)×0.5) |
| 2 | Medium | 0.1s 线性上升 | 0.8+0.2×exp(-(t-0.1)×0.1) |
| 3 | Slow | 0.2s 线性上升 | 0.8+0.2×exp(-(t-0.2)×0.1) |

**Release 阶段（active=false）：**

| Envelope | 衰减速率 | 实际释放时间 |
|----------|----------|-------------|
| 0 | exp(-t × 50.0) | ~20ms（极快） |
| 1 | exp(-t × 25.0) | ~40ms（非常快） |
| 2 | exp(-t × 10.0) | ~100ms（快） |
| 3 | exp(-t × 4.0) | ~250ms（中速） |

#### UpdateChannelLevels — `chip_control.cpp:596-635`

每帧执行：
1. 计算目标级别（调用 CalculateEnvelopeLevel）
2. 平滑混合：`current + (target - current) × blendFactor`（blendFactor=0.5，重触发=0.7）
3. 更新 `g_pianoKeyLevel[keyIdx]` = channel.currentLevel
4. 当 level < 0.01 且通道不活跃时，清除钢琴键状态

### 4.7 鼓声控制

#### play_drum — `chip_control.cpp:536-553`

鼓声位掩码：BD=0x10, HC=0x08, SD=0x04, HHO=0x02, HHD=0x01

写入寄存器 0x90 触发鼓声。双芯片模式交替使用 Slot0/Slot1。

#### 鼓声衰减 — `chip_control.cpp:637-650`

`exp(-t × 5.0)` — 约 200ms 衰减。

### 4.8 频率表 — `chip_control.cpp:37-45`

| 音符 | C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|------|---|----|----|----|----|----|----|----|----|----|----|----|
| F-Number (C3-C6) | 951 | 900 | 852 | 803 | 756 | 716 | 674 | 637 | 601 | 567 | 535 | 507 |
| B2 F-Number | 1014 | | | | | | | | | | | |
| C7 F-Number | 475 | 450 | 426 | 401 | 378 | 358 | 337 | 318 | 300 | 283 | 267 | 0 |

### 4.9 力度映射 — `chip_control.cpp:654-666`

| 模式 | 0dB | -6dB | -12dB | Mute |
|------|-----|------|-------|------|
| 动态映射 | ≥threshold_0dB | ≥threshold_6dB | ≥threshold_12dB | <threshold_mute |
| 静态映射 | ≥96 | ≥64 | ≥32 | <32 |

### 4.10 YM2163 芯片初始化 — `chip_control.cpp:251-305`

`init_single_ym2163(chipIndex)` — 发送默认寄存器序列：
- 全通道 Key Off
- 音量设为 Mute
- 包络设为 Decay
- 音色清零
- 鼓声关闭
- 等待 50ms 稳定

### 4.11 钢琴键状态管理 — `chip_control.cpp:407-413`

```cpp
void ResetPianoKeyStates() {
    for (int i = 0; i < 73; i++) {
        g_pianoKeyPressed[i] = false;
        g_pianoKeyChipIndex[i] = -1;
        g_pianoKeyLevel[i] = 0.0f;
    }
}
```

### 4.12 全局设置变量 — `chip_control.h:66-150`

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `g_currentTimbre` | 4 (Piano) | 当前音色 |
| `g_currentEnvelope` | 1 (Fast) | 当前包络 |
| `g_currentVolume` | 0 (0dB) | 当前音量 |
| `g_enableSecondYM2163` | true | 启用第二块芯片 |
| `g_enableThirdYM2163` | false | 启用第三块芯片 |
| `g_enableFourthYM2163` | false | 启用第四块芯片 |
| `g_enableSlot3_2MHz` | false | Slot3 2MHz 模式（C8-B8） |
| `g_enableSlot3Overflow` | false | C4-B7 溢出到 2MHz 芯片 |
| `g_enableSustainPedal` | true | 踏板支持 |
| `g_sustainPedalActive` | false | 踏板当前状态 |
| `g_pedalMode` | 0 (Disabled) | 踏板模式 |
| `g_enableVelocityMapping` | true | MIDI 力度映射 |
| `g_enableDynamicVelocityMapping` | true | 动态力度分析 |

---

## 5. MIDI 播放器模块

### 文件：`src/midi_player.h` (177行) / `src/midi_player.cpp` (845行)

### 5.1 播放器状态 — `midi_player.h:70-89`

```cpp
struct MidiPlayerState {
    MidiFile midiFile;                     // 已加载的 MIDI 文件
    std::string currentFileName;           // 当前文件名
    bool isPlaying, isPaused;              // 播放/暂停状态
    int currentTick;                       // 当前事件索引
    time_point playStartTime, pauseTime;   // 起始/暂停时间
    milliseconds pausedDuration;           // 暂停累计时间
    double tempo;                          // 当前速度（μs/四分音符）
    int ticksPerQuarterNote;               // MIDI 分辨率
    LARGE_INTEGER perfCounterFreq;         // 高精度计时器频率
    LARGE_INTEGER lastPerfCounter;         // 上次计时器读数
    double accumulatedTime;                // 累计播放时间（μs）
    map<int, map<int, int>> activeNotes;   // 通道→音符→YM2163通道
};
```

### 5.2 高精度定时 — `midi_player.cpp:727-735`

使用 `QueryPerformanceCounter` 实现微秒级精度：

```cpp
QueryPerformanceCounter(&now);
double elapsedMicros = (double)(now.QuadPart - lastPerfCounter.QuadPart)
                      / (double)perfCounterFreq.QuadPart * 1000000.0;
lastPerfCounter = now;
accumulatedTime += elapsedMicros;
```

时间线计算：`eventTime = event.tick × (tempo / ticksPerQuarterNote)`

### 5.3 播放控制

| 函数 | 行号 | 功能 |
|------|------|------|
| `PlayMIDI()` | cpp:581-631 | 开始播放 / 从暂停恢复（重置 lastPerfCounter） |
| `PauseMIDI()` | cpp:633-640 | 暂停（记录 pauseTime，停止所有音符） |
| `StopMIDI()` | cpp:642-654 | 完全停止（重置所有状态） |
| `UpdateMIDIPlayback()` | cpp:720-836 | 主更新循环（处理事件、发送音符） |
| `LoadMIDIFile(filename)` | cpp:407-455 | 加载 MIDI 文件（支持超长路径） |

#### PlayMIDI 恢复路径 — `midi_player.cpp:583-587`

```cpp
if (g_midiPlayer.isPaused) {
    g_midiPlayer.isPaused = false;
    g_midiUserPaused = false;              // 清除用户暂停标志
    auto now = std::chrono::steady_clock::now();
    g_midiPlayer.pausedDuration += duration_cast<milliseconds>(now - pauseTime);
    QueryPerformanceCounter(&g_midiPlayer.lastPerfCounter);  // 重置计时器
}
```

### 5.4 MIDI 事件处理 — `midi_player.cpp:736-829`

在 `UpdateMIDIPlayback` 中循环处理：

| 事件类型 | 处理逻辑 | 行号 |
|----------|----------|------|
| Note On (velocity>0) | 转换 YM2163 音高，分配通道，播放 | 753-809 |
| Note On (velocity=0) | 视为 Note Off | 同上 |
| Note Off | 停止对应 YM2163 通道 | 810-817 |
| Tempo | 更新 `g_midiPlayer.tempo` | 818-820 |
| Controller 64 | 踏板控制 | 821-825 |

### 5.5 文件浏览器 — `midi_player.cpp:107-301`

**核心功能：**

| 函数 | 行号 | 功能 |
|------|------|------|
| `RefreshFileList()` | 107-150 | 扫描当前目录的 .mid/.midi 文件 |
| `NavigateToPath(path)` | 152-174 | 导航到指定路径，更新历史栈 |
| `NavigateBack()` | 176-183 | 后退 |
| `NavigateForward()` | 185-192 | 前进 |
| `NavigateToParent()` | 194-205 | 上级目录 |
| `InitializeFileBrowser()` | 303-311 | 初始化（加载历史，导航到 exe 目录） |

**FileBrowserState 结构** — `midi_player.h:36-67`

每个标签页（YM2163, OPL3, VGM）拥有独立的 FileBrowserState 实例。

**特性：**
- Win11 风格面包屑导航栏（`gui_renderer_impl.cpp:155-254`）
- 双击切换编辑模式
- 长文件名自动滚动显示
- 退出文件夹高亮（橙色）
- 播放文件高亮（绿色）
- 播放路径高亮（蓝色）
- 滚动位置记忆（每个路径独立）

### 5.6 文件夹历史 — `midi_player.cpp:237-301`

- 最多保存 20 个条目
- 自动去除重复、按最近打开排序
- 使用 `GetPrivateProfileString`/`WritePrivateProfileString` INI API
- 持久化到 `ym2163_config.ini` 的 `[MidiFolderHistory]` 节
- YM2163 和 OPL3 各自独立历史

### 5.7 播放列表 — `midi_player.cpp:496-577`

| 函数 | 行号 | 功能 |
|------|------|------|
| `GetNextMIDIFileIndex()` | 496-522 | 获取下一首（顺序/随机模式） |
| `GetPreviousMIDIFileIndex()` | 524-541 | 获取上一首 |
| `PlayNextMIDI()` | 543-559 | 切换到下一首（重置芯片+加载+播放） |
| `PlayPreviousMIDI()` | 561-577 | 切换到上一首 |

### 5.8 进度条跳转 — `gui_renderer_impl.cpp:69-137`

基于 `accumulatedTime` 的百分比定位：
1. 计算点击位置对应的 `targetTime`
2. 二分搜索目标 MIDI tick
3. 重置所有音符和活跃状态
4. 更新计时器基线
5. 调用 `RebuildActiveNotesAfterSeek()` 恢复按键状态

### 5.9 自动跳过开头静音 — `midi_player.cpp:599-625`

`FindFirstNoteEvent()` 扫描找到第一个 Note On 事件，跳过之前的空白。

### 5.10 全局媒体键 — `midi_player.cpp:315-336`

| 热键 ID | 行为 | 行号 |
|---------|------|------|
| `VK_MEDIA_PLAY_PAUSE` | 播放/暂停 | 318 |
| `VK_MEDIA_NEXT_TRACK` | 下一首 | 319 |
| `VK_MEDIA_PREV_TRACK` | 上一首 | 320 |

### 5.11 Unicode 工具函数 — `midi_player.cpp:55-103`

| 函数 | 行号 | 功能 |
|------|------|------|
| `WideToUTF8(wstr)` | 55-61 | 宽字符→UTF-8 |
| `UTF8ToWide(str)` | 63-69 | UTF-8→宽字符 |
| `UTF8CharCount(str)` | 71-82 | 统计 UTF-8 字符数（非字节数） |
| `UTF8CharOffset(str, charIdx)` | 84-94 | 字符索引→字节偏移 |
| `TruncateFolderName(name, maxLen)` | 96-103 | 截断文件夹名（保留首尾） |

---

## 6. YM2163 窗口渲染模块

### 文件：`src/gui_renderer.h` (67行) / `src/gui_renderer.cpp` (292行) / `src/gui_renderer_impl.cpp` (1,275行)

### 6.1 DX11 设备管理 — `gui_renderer.cpp:169-210`

| 函数 | 行号 | 功能 |
|------|------|------|
| `CreateDeviceD3D(hwnd)` | 169-203 | 创建 DXGI SwapChain、D3D11 Device/Context、RenderTargetView |
| `CleanupDeviceD3D()` | 205-210 | 安全释放所有 DX11 资源 |
| `WndProc(hwnd, msg, w, l)` | 214-282 | Windows 消息处理 |

#### WndProc 关键消息 — `gui_renderer.cpp:214-282`

| 消息 | 行号 | 处理 |
|------|------|------|
| `WM_SIZE` | 219-223 | 更新窗口尺寸 |
| `WM_ENTERSIZEMOVE` | 229-231 | 启动 MIDI 更新定时器 |
| `WM_EXITSIZEMOVE` | 233-235 | 停止定时器 |
| `WM_TIMER` | 239-243 | 窗口拖动期间保持 MIDI 播放 |
| `WM_HOTKEY` | 247-262 | 全局媒体键 |
| `WM_KEYDOWN/UP` | 264-270 | 键盘输入 |
| `WM_DESTROY` | 272-279 | 保存配置、断开硬件、退出 |

### 6.2 键盘输入 — `gui_renderer.cpp:100-165`

**HandleKeyPress** — `gui_renderer.cpp:100-140`

| 按键 | 行为 | 行号 |
|------|------|------|
| PageUp/PageDown | 八度 ±1（0-5） | 106-110 |
| ↑/↓ | 音量 ±1（0-3） | 111-112 |
| F1-F4 | 选择包络 | 113-114 |
| F5-F9 | 选择音色 | 115-116 |
| Numpad 1-5 | 鼓声 | 117-118 |
| Z X C V B N M / S D G H J K L / Q W E R T Y U I O P / 2 3 5 6 7 8 9 0 | 钢琴键盘映射 | 120-139 |

**HandleKeyRelease** — `gui_renderer.cpp:142-165`

找到对应通道并停止音符。不立即清除钢琴键状态（让衰减动画自然完成）。

### 6.3 YM2163 窗口模块 — `src/chip_window_ym2163.cpp/h`

封装 YM2163 窗口的初始化、更新、渲染（DockSpace 模式，自管理 ImGui::Begin/End）：

| 函数 | 行号 | 功能 |
|------|------|------|
| `YM2163Window::Init()` | cpp:16-19 | 初始化通道 + 连接硬件 |
| `YM2163Window::Update()` | cpp:27-33 | 硬件轮询 + 鼓状态 + 音量衰减 |
| `YM2163Window::Render()` | cpp:37-93 | `Begin("YM2163(DSG)")` + 内容 + `End()` |
| `YM2163Window::IsVisible()` | — | 基于 `Begin()` 返回值的可见性 |

#### 可见性自动暂停

```cpp
void YM2163Window::Render() {
    bool visible = ImGui::Begin("YM2163(DSG)", ...);
    if (!visible) {
        // 标签不活动，自动暂停 MIDI
        if (MidiPlayer::g_midiPlayer.isPlaying && !g_midiUserPaused) {
            s_midiAutoPaused = true;
            MidiPlayer::PauseMIDI();
        }
        ImGui::End();
        return;
    }
    // 标签激活，恢复播放
    if (s_midiAutoPaused && !g_midiUserPaused) {
        s_midiAutoPaused = false;
        MidiPlayer::PlayMIDI();
    }
    // ... 渲染内容 ...
    ImGui::End();
}
```

#### Render 内部布局 — `chip_window_ym2163.cpp:37-93`

```
┌─────────────┬──────────────────────────────────┐
│             │  钢琴键盘 (RenderPianoKeyboard)    │
│  控制面板    │──────────────────────────────────│
│ (Controls)   │  音量条 (RenderLevelMeters)        │
│             │──────────────────────────────────│
│             │  状态区 (RenderChannelStatus)       │
│  文件夹历史   │──────────────────────────────────│
│             │  MIDI 播放器 (RenderMIDIPlayer)     │
│             │──────────────────────────────────│
│             │  文件浏览器                         │
│  日志       │──────────────────────────────────│
│             │  日志 (RenderLog)                   │
└─────────────┴──────────────────────────────────┘
```

### 6.4 钢琴键盘渲染 — `gui_renderer_impl.cpp:417-597`

**布局参数** — `gui_renderer_impl.cpp:431-434`

```
白键尺寸: 20×100 像素
黑键尺寸: 12×60 像素
总白键: 36 (B2-B7) + 7 (C8-B8, 当 Slot3 2MHz 启用时)
居中显示
```

**芯片颜色** — `gui_renderer_impl.cpp:418-423`

| 芯片 | RGBA | 视觉颜色 |
|------|------|----------|
| Slot0 | (0.0, 1.0, 0.5, 1.0) | 绿色 |
| Slot1 | (0.5, 0.5, 1.0, 1.0) | 蓝色 |
| Slot2 | (1.0, 0.5, 0.5, 1.0) | 红色 |
| Slot3 | (1.0, 0.8, 0.2, 1.0) | 橙色 |

**动态衰减颜色** — `gui_renderer_impl.cpp:445-458`

颜色深浅 = `g_pianoKeyLevel[keyIdx] × (g_pianoKeyVelocity[keyIdx] / 127.0)`

按键时亮色（强度=1.0），释放后随 ADSR 包络衰减至暗色（强度→0），最后消失。

```
intensity = level × velocityFactor;
color = chipColor × (0.3 + 0.7 × intensity);
```

**钢琴键映射** — `chip_control.cpp:373-378`

| 范围 | keyIdx | 备注 |
|------|--------|------|
| B2 | 0 | 最低音 |
| C3-B7 | 1-61 | 标准范围 |
| C8-B8 | 61-72 | 仅 Slot3 2MHz |

**八度标注** — `gui_renderer_impl.cpp:503-507`

白键 C 标注 "C3"~"C7"（C 编号 = octave + 2）。

### 6.5 音量条渲染 — `gui_renderer_impl.cpp:601-733`

**布局** — `gui_renderer_impl.cpp:606-617`

4 个芯片组（Slot0-Slot3），每组 4 旋律通道 + 5 鼓通道。

**渐变配色** — `gui_renderer_impl.cpp:626-652`

```
Blue(0.0) ─── Green(0.33) ─── Yellow(0.66) ─── Red(1.0)
(0,100,255)   (0,255,100)    (255,255,0)     (255,100,0)
```

分段渲染：20 段，每段根据该段级别独立着色。

### 6.6 通道状态显示 — `gui_renderer_impl.cpp:737-802`

2×2 网格显示 4 个芯片状态：
- 活跃通道：显示音名、八度、音色、包络、音量
- 释放阶段：黄色闪烁 "CHx: Release"（1秒）
- 空闲通道：灰色 "CHx: ---"
- 鼓状态：一行显示 5 个鼓（BD/HC/SD/HO/HD）

### 6.7 控制面板 — `gui_renderer_impl.cpp:806-1060`

| 区域 | 行号 | 控件 |
|------|------|------|
| 八度控制 | 811-816 | Oct+/Oct-（0-5） |
| 音量控制 | 819-822 | Vol+/Vol-（0dB ~ Mute） |
| MIDI 模式 | 826-833 | Live Control / Config Mode |
| 力度映射 | 836-853 | 静态/动态映射开关 |
| 踏板 | 854-855 | CC64→包络切换 |
| 媒体键 | 859-863 | 全局键盘捕获开关 |
| 自动跳过静音 | 865-866 | 跳到第一个音符 |
| 芯片配置 | 869-972 | Slot0-3 启用/2MHz 模式 |
| 乐器编辑 | 976-1016 | 乐器 0-127 选择/保存/加载 |
| 包络选择 | 1019-1024 | Decay/Fast/Medium/Slow |
| 踏板模式 | 1028-1033 | Disabled/Piano/Organ |
| 音色选择 | 1037-1042 | String/Organ/Clarinet/Piano/Harpsichord |
| 鼓声 | 1046-1055 | BD/HC/SDN/HHO/HHD 按钮 |

### 6.8 MIDI 播放器控件 — `gui_renderer_impl.cpp:7-413`

- Play/Pause/Stop 按钮
- Previous/Next 导航
- 进度条（可拖动跳转，显示 MM:SS 时间）
- 顺序/随机播放模式
- 自动播放下一首

### 6.9 文件浏览器 — `gui_renderer_impl.cpp:142-412`

- 面包屑路径栏（自动缩略、双击编辑）
- 文件列表（高亮、悬停、滚动）
- 长文件名滚动显示（30px/s）
- 文件夹历史下拉（`gui_renderer_impl.cpp:1221-1253`）

### 6.10 日志面板 — `gui_renderer_impl.cpp:1064-1106`

- 可折叠显示
- 自动滚动开关
- 清除按钮
- 从 `YM2163::GetLogBuffer()` 读取日志内容

### 6.11 频率调谐窗口 — `gui_renderer_impl.cpp:1108-1219`

- C3-C6 基础频率（12 个输入框）
- B2 特殊频率
- C7 八度频率（12 个输入框）
- 保存/加载到 `ym2163_config.ini`

---

## 7. OPL3 窗口模块

### 文件：`src/opl3_renderer.h` (10行) / `src/opl3_renderer.cpp` (600行) / `src/opl3_window.cpp/h` (91+17行)

### 7.1 OPL3 窗口模块 — `src/opl3_window.cpp`

| 函数 | 行号 | 功能 |
|------|------|------|
| `OPL3Window::Init()` | 15-17 | 初始化 |
| `OPL3Window::Update()` | 25-28 | 空实现（OPL3 无硬件轮询） |
| `OPL3Window::Render()` | 32-85 | `Begin("YMF262(OPL3)")` + 内容 + `End()` |

### 7.2 OPL3 渲染函数 — `opl3_renderer.cpp`

| 函数 | 行号 | 功能 |
|------|------|------|
| `RenderOPL3Controls()` | 193-249 | OPL3 控制面板 |
| `RenderOPL3PianoKeyboard()` | 251-290 | OPL3 钢琴键盘 |
| `RenderOPL3LevelMeters()` | 292-308 | OPL3 音量条 |
| `RenderOPL3ChannelStatus()` | 310-325 | OPL3 通道状态 |
| `RenderOPL3MIDIPlayer()` | 327-596 | OPL3 MIDI 播放器（复用 MidiPlayer） |
| `RenderOPL3Log()` | 598-600 | OPL3 日志 |

OPL3 窗口复用 `MidiPlayer::g_midiPlayer` 进行 MIDI 播放。

### 7.3 OPL3 文件夹历史

使用 `GetPrivateProfileString`/`WritePrivateProfileString` INI API，持久化到 `opl3_config.ini` 的 `[Opl3FolderHistory]` 节。

### 7.4 OPL3 文件浏览器 — `opl3_renderer.cpp:39-169`

拥有独立的文件浏览器状态，但与 YM2163 共享 `MidiPlayer` 基础设施。

---

## 8. VGM/libvgm 播放器模块

### 文件：`src/vgm_window.h` (58行) / `src/vgm_window.cpp` (8,340行)

这是项目最大的模块，实现完整的 VGM/VGZ/S98/GYM/DRO 芯片音乐播放器。

### 8.1 支持格式

| 格式 | 扩展名 | 解析器 |
|------|--------|--------|
| VGM | .vgm, .vgz | vgm_parser.cpp + libvgm PlayerA |
| S98 | .s98 | vgm_parser.cpp + libvgm |
| GYM | .gym | libvgm |
| DRO | .dro | libvgm |

### 8.2 支持的芯片（40+） — `vgm_window.cpp:3089-4612`

**FM 系列：**

| 芯片 | 通道数 | 仿真核心 |
|------|--------|----------|
| YM2151 (OPM) | 8 | fmopn.c |
| YM2203 (OPN) | 6 (3FM+3SSG) | fmopn.c |
| YM2608 (OPNA) | 16 | fmopn.c |
| YM2610/2610B | 14/16 | fmopn.c |
| YM2612 (OPN2) | 6 | fmopn.c |
| YM2413 (OPLL) | 9+5rhythm | ym2413intf.c |
| YM3526 (OPL) | 9 | adlibemu_opl2.c |
| YM3812 (OPL2) | 9+5rhythm | adlibemu_opl2.c |
| Y8950 | 10+5rhythm+ADPCM | adlibemu_opl2.c |
| YMF262 (OPL3) | 18 | adlibemu_opl3.c |
| YMF271 (OPX) | 12 | ymf271intf.c |
| YMF278B (OPL4) | 18+1ADPCM | ymf278b.c |

**PSG/脉冲波：**

| 芯片 | 通道数 | 仿真核心 |
|------|--------|----------|
| AY8910 (SSG) | 3 | emu2149.c |
| SN76489 | 4 (3t+1n) | sn76496.c |
| HuC6280 (PC Engine) | 6 | huc6280.c |
| NES APU | 5 (2p+1t+1n+1dmc) | nes_apu.c |
| Game Boy DMG | 4 (2s+1w+1n) | gb.c |

**其他：**

SAA1099, POKEY, K051649 (SCC), OKIM6258/6295, RF5C68, YMZ280B, MultiPCM, uPD7759, K054539, C140, K053260, QSound, SCSP, VSU, ES5503/5506, X1-010, C352, GA20, SegaPCM, 32X PWM

### 8.3 影子寄存器系统 — `vgm_window.cpp:1238-2164`

重放 VGM 事件同步影子寄存器状态，支持：
- 40+ 芯片类型的寄存器跟踪
- KeyOn/KeyOff 事件检测
- 双芯片实例识别（指针地址区分）
- 每通道可视化状态（ChVizState）

### 8.4 核心渲染函数

| 函数 | 行号 | 功能 |
|------|------|------|
| `Init()` | 950 | 初始化音频系统 + 配置加载 |
| `Shutdown()` | 994 | 清理资源 |
| `LoadFile(path)` | 1002-1132 | 加载 VGM 文件 |
| `RenderTab()` | — | `Begin("libvgm")` + 内容 + `End()`（从 main.cpp 提取） |
| `RenderControls()` | 2824-3002 | 传输控制 + GD3 标签显示 |
| `RenderPianoArea()` | 5240-5504 | 7 八度钢琴键盘（C1-B7） |
| `RenderLevelMeterArea()` | 4614-4929 | 电平表条 |
| `RenderScopeArea()` | 4931-5238 | 多行示波器 |
| `RenderStatusArea()` | 5529-7907 | 寄存器状态显示 |
| `RenderFileBrowserPanel()` | 2168-2474 | 独立文件浏览器 |
| `RenderLogPanel()` | 7913-8058 | 日志面板 |
| `RenderChipAliasWindow()` | 2550-2615 | 芯片别名设置 |
| `RenderScopeSettingsWindow()` | 2638-2822 | 示波器设置（宽度/幅度/偏移/AC模式） |

### 8.5 BuildLevelMeters — `vgm_window.cpp:3089-4612`

核心电平表计算函数，1524 行。对每个活跃芯片：
1. 从影子寄存器读取 TL（总音量级别）
2. 计算 MIDI 音符号（用于钢琴键盘映射）
3. 检测 KeyOn/KeyOff 事件
4. 应用颤音（PMS）和震音（AMS）
5. 应用用户定义的芯片别名
6. 返回 `LevelMeterEntry` 数组

### 8.6 示波器 — `vgm_window.cpp:4931-5238` + `modizer_viz.cpp:125-358`

**布局：** 固定宽度（可调 40-150px），每行最多 11 通道，超过自动换行。

**波形渲染：** 使用 `ModizerViz::DrawChannel()` 从语音缓冲区读取波形数据。

**示波器设置窗口** — `vgm_window.cpp:2638-2822`：
- 每芯片独立宽度/幅度/采样数/偏移设置
- 边沿对齐、静音清除选项
- AC 模式（AY8910/NES_APU）
- 通道颜色自定义

### 8.7 钢琴键盘 — `vgm_window.cpp:5240-5504`

- 7 八度范围（C1-B7, MIDI 24-107）
- 多芯片颜色混合
- LFO 指示器：PMS（水平抖动=颤音）、AMS（垂直脉冲=震音）

### 8.8 配置持久化

| 函数 | 行号 | 文件 |
|------|------|------|
| `SaveConfig()` | 533-609 | `vgm_config.ini` |
| `LoadConfig()` | 613-744 | `vgm_config.ini` |
| `SavePlayerState()` | 751 | `vgm_config.ini` |
| `SaveChipAliases()` | — | `vgm_config.ini` |
| `SaveFolderHistory()` | — | `vgm_config.ini` |

---

## 9. Gigatron Tracker 播放器模块

### 文件

| 文件 | 行数 | 用途 |
|------|------|------|
| `src/gigatron_window.cpp` | ~2,700 | Gigatron 窗口 UI（文件浏览、钢琴、示波器、波形编辑器） |
| `src/gigatron_window.h` | 15 | `GigatronWindow` 命名空间公共 API |
| `src/gigatron/gigatron_emu.c` | ~310 | 仿真核心（振荡器累加、soundTable、自定义波表、PCM 输出） |
| `src/gigatron/gigatron_emu.h` | 165 | 状态结构体、寄存器定义、Ratio Counter、函数声明 |
| `src/gigatron/winmm.c` | ~100 | WinMM 音频输出（ring buffer, BUFFER_SIZE=8192） |
| `src/gigatron/audio_output.h` | 20 | 音频输出接口（虚基类） |
| `src/gigatron/fnum_table.h` | ~100 | 频率表 FNUM_TABLE[96]（8 八度 × 12 音） |

### 仿真核心架构

Gigatron 使用 4 通道 TTL 音频，每通道 4 个寄存器：

| 寄存器 | 地址 | 功能 |
|--------|------|------|
| OSC (振荡器相位) | `0xFE/0xFF` | 14-bit 累加器，按 KEY 值递增 |
| KEY (频率步进) | `0xFC/0xFD` | 14-bit 频率值，`adrFnumH` 写入时 ×4 |
| WAVX (波形选择) | `0xFB` | 与索引异或：0=Noise, 1=Triangle, 2=Pulse, 3=Sawtooth |
| WAVA (波形幅度) | `0xFA` | int8_t，64=最大音量，0-63 改音色，65-127 音量递减 |

波形索引计算：`i_idx = (osc >> 7) & 0xFC; sample = soundTable[i_idx ^ wavX]`

### 双波表系统

| 波表 | 类型 | 精度 | 说明 |
|------|------|------|------|
| `soundTable[256]` | `uint8_t` | 6-bit (0-63) | 原始 Gigatron 硬件波表，4 类波形各 64 点 |
| `customWaveTable[256]` | `uint16_t` | 6/8/12/16-bit | 高精度自定义波表，开关控制 |

- `useCustomWaveTable=false`：原始路径，6-bit clamp，8-bit 中心 PCM 转换
- `useCustomWaveTable=true`：自定义路径，归一化累加 → 0.5 中心 int16 映射

### 音频处理管线

```
振荡器相位累加 → 波表查表 → 4 通道累加 (samp)
    ↓
audio_bit_depth 量化 (4/6/8/12/16-bit)
    ↓
DC 偏移移除 (可选, IIR 高通, alpha=0.99)
    ↓
PCM 转换 → int16 立体声 → WinMM 输出 (44100Hz)
```

### wavA 音量映射

wavA 实际使用范围为 64-127：
- **wavA=64**：最大音量 (level=1.0)
- **wavA=96**：中等音量 (level=0.5)
- **wavA=127**：静音 (level=0.0)
- **wavA 0-63**：改变音色（直流偏移），不常用

公式：`level = 1.0 - (wavA - 64) / 63.0`，clamp [0, 1]

### UI 功能

| 功能 | 说明 |
|------|------|
| 文件浏览器 | 支持 .c / .gbas.c 文件，文件夹导航，历史记录 |
| 钢琴键盘 | 从 C1 (MIDI 24) 到 C8，颜色亮度匹配 wavA 音量 |
| 电平表 | 4 通道垂直 VU 表，带峰值衰减 |
| 示波器 | 互相关触发 + 边缘对齐 + AC 耦合，VGM 同款设置面板 |
| 波形编辑器 | 鼠标手绘波形，9 种预设，4 种位深 (6/8/12/16) |
| 寄存器表 | ImGui 可调列宽表格，实时显示 4 通道寄存器状态 |
| 播放控制 | Play/Stop/Pause，速度调节，段落选择，帧定位 |

### 配置文件

- `gigatron_config.ini`：所有设置（INI 格式）
  - `[Settings]`：BitDepth, VolumeScale, PlaybackSpeed, ShowScope...
  - `[ScopeSettings]`：Samples, SearchWindow, EdgeAlign, ACMode...
  - `[WaveEditor]`：UseCustom, Bits
  - `[GtFolderHistory]`：文件夹历史
- `gigatron_custom_wave.wtab`：自定义波形表（二进制，1 字节 bits + 256×uint16 数据）

---

## 10. YM2413 (OPLL) VGM 播放器模块

### 文件：`src/ym2413_window.h` (25行) / `src/ym2413_window.cpp` (~2800行)

### 核心源文件

| 文件 | 行数 | 用途 |
|------|------|------|
| `src/ym2413_window.cpp` | ~2,800 | YM2413 OPLL VGM 播放器窗口 |
| `src/ym2413_window.h` | 25 | `YM2413Window` 命名空间公共 API |

### 10.1 功能概览

YM2413 (OPLL) 是 Yamaha 的低成本 FM 合成芯片，内建 15 种乐器音色 + 1 种自定义音色，支持 9 旋律通道 + 5 节奏通道（BD/SD/TOM/HH/CYM）。

### 10.2 寄存器影子跟踪

- 寄存器 0x00-0x38 完整影子：`s_regShadow[64]`
- Key-on/bit4 上升沿/下降沿检测（`s_prevKeyOn[9]`）
- 节奏通道独立检测（register 0x0E bit4-0）

### 10.3 节奏通道音量

| 通道 | 寄存器 | 位 | 说明 |
|------|--------|-----|------|
| BD | 0x36 | & 0x0F | 低 nibble |
| SD | 0x37 | & 0x0F | 低 nibble |
| TOM | 0x38 | >> 4 | 高 nibble |
| HH | 0x37 | >> 4 | 高 nibble |
| CYM | 0x38 | & 0x0F | 低 nibble |

### 10.4 快进/Seek

- `MuteHardwareOnly()`: 仅静音硬件，不清影子寄存器
- `ApplyShadowState()`: 快进完成后从影子恢复完整芯片状态
- 快进过程逐命令重放，保持硬件时序连续

### 10.5 曲目切换静音

静音顺序：key-off → rhythm off → TL=0x0F → rhythm silence
- TL 写入 0x0F（低 nibble），非 0xF0
- Rhythm silence: 0x36=0x0F, 0x37=0xFF, 0x38=0xFF

### 10.6 钢琴键盘指示器

| 指示器 | 动画 | 幅度 | 说明 |
|--------|------|------|------|
| VIB | 整条键高度左右摆动 | ±0.14 半音 | 硬件 14 cent |
| AM | 键顶部上下脉冲条 | ±0.3 | 硬件 4.8dB |
| 滑音 | 整条键高度水平位移 | - | key-on 期间频率变化，>1 半音不算 |

- 指示器使用纯通道颜色（比键面更深饱和）
- LFO 固定 3.98Hz，AM/VIB 从 patch ROM bit8/9 读取

### 10.7 颜色系统

- 14 通道独立颜色（9 旋律 + 5 节奏）
- `kChColorsCustom[14]` 自定义颜色（0=使用默认）
- ColorEdit4 UI + 逐通道 Reset + Reset All
- 持久化到 `ym2413_config.ini` [Colors] 段

### 10.8 参考源码

| 源码 | 参考内容 |
|------|----------|
| MDPlayer | 节奏通道 key-on/volume、切歌静音、seek、patch ROM AM/VIB |
| MegaGRRL | 切歌寄存器写入顺序 |
| libvgm | 钢琴键盘指示器动画效果（PMS/AMS） |

---

## 11. 配置管理模块

### 文件：`src/config_manager.h` (49行) / `src/config_manager.cpp` (185行)

### 9.1 配置文件布局

| 文件 | 内容 |
|------|------|
| `bin/ym2163_config.ini` | 频率表、Slot 配置、128 乐器映射、鼓配置、MIDI 文件夹历史 |
| `bin/opl3_config.ini` | OPL3 文件夹历史 |
| `bin/vgm_config.ini` | VGM 播放器状态、UI 设置、示波器配置、芯片别名、文件夹历史 |
| `bin/imgui.ini` | DockSpace 布局（ImGui 内建管理） |

### 9.2 数据结构 — `config_manager.h:16-35`

```cpp
struct InstrumentConfig {
    std::string name;
    int wave;       // 0=none, 1=String, 2=Organ, 3=Clarinet, 4=Piano, 5=Harpsichord
    int envelope;   // 0=Decay, 1=Fast, 2=Medium, 3=Slow
    int pedalMode;  // 0=disabled, 1=Piano, 2=Organ
};

struct DrumConfig {
    std::string name;
    std::vector<uint8_t> drumBits;
};
```

### 9.3 配置函数

| 函数 | 行号 | 配置文件 |
|------|------|----------|
| `InitConfigPaths()` | cpp:21-33 | 设置 INI 路径（exe 目录） |
| `LoadFrequenciesFromINI()` | cpp:54-73 | 频率表（12×4 八度 + B2 + C7） |
| `SaveFrequenciesToINI()` | cpp:37-52 | 保存频率表 |
| `LoadSlotConfigFromINI()` | cpp:93-100 | 芯片 Slot 启用配置 |
| `SaveSlotConfigToINI()` | cpp:77-91 | 保存 Slot 配置 |
| `LoadMIDIConfig()` | cpp:104-181 | 乐器映射（128 乐器 × 音色/包络/踏板模式） + 鼓配置 |

---

## 12. VGM 文件解析模块

### 文件：`src/vgm_parser.h` (94行) / `src/vgm_parser.cpp` (466行)

### 10.1 数据结构 — `vgm_parser.h:18-92`

```cpp
struct GD3_TAGS {
    std::string trackNameEn, trackNameJp;
    std::string gameNameEn, gameNameJp;
    std::string systemNameEn, systemNameJp;
    std::string authorEn, authorJp;
    std::string releaseDate, creator, notes;
};

struct VgmEvent {
    UINT32 tick;
    UINT8 chipType;
    UINT8 chipIndex;
    UINT8 reg, data;
    bool isKeyOn;
};

class VGMFile {
    VGM_HEADER header;
    GD3_TAGS tags;
    std::vector<VgmEvent> events;
    // ...
};
```

### 10.2 解析函数

| 函数 | 行号 | 功能 |
|------|------|------|
| `VGMFile::Load(dLoad)` | cpp:147-190 | 加载文件（自动检测 VGM/VGZ/S98） |
| `VGMFile::ParseHeader()` | cpp:246-274 | 解析 VGM 头部 |
| `VGMFile::LoadTags()` | cpp:276-316 | 解析 GD3 标签 |
| `VGMFile::ParseVGMCommands()` | cpp:332-460 | 解析 VGM 命令流 |
| `VGMFile::ParseS98Commands()` | cpp:462-466 | 解析 S98 命令流 |
| `InflateGzip()` | cpp:73-110 | VGZ 解压缩 |
| `VGM_CMD_INFO[256]` | cpp:36-70 | 命令长度/芯片类型查找表 |

---

## 13. 可视化模块（Modizer）

### 文件：`src/modizer_viz.h` (40行) / `src/modizer_viz.cpp` (358行)

### 11.1 ModizerViz 类 — `modizer_viz.h:9-38`

```cpp
class ModizerViz {
    static const int BUFFER_LEN = 4096;
    int8_t* m_voice_prev_buff;     // 语音缓冲区
    int m_voice_ofs[256];          // 读偏移
    int m_voice_prev_write_ptr[256]; // 上次写入指针

    void DrawChannel(ImDrawList* dl, float x, float y, float w, float h,
                     int8_t* voice_buff, int voice_ch, // ...);
};
```

### 11.2 DrawChannel — `modizer_viz.cpp:125-358`

从环形缓冲区读取波形数据，渲染为示波器线条：
- 支持缩放和偏移
- 支持边沿对齐
- 支持静音时清除
- 234 行核心渲染逻辑

---

## 14. 第三方库

### 12.1 ImGui (docking 分支 v1.92.8 WIP)

**路径：** `imgui/`

| 文件 | 用途 |
|------|------|
| `imgui.h/cpp` | 核心库（窗口、控件、布局、DockSpace） |
| `imgui_internal.h` | 内部 API（DockBuilder API） |
| `imgui_draw.cpp` | 自定义绘图（ImDrawList） |
| `imgui_tables.cpp` | 表格控件 |
| `imgui_widgets.cpp` | 标准控件 |
| `imgui_impl_dx11.cpp` | DirectX 11 后端 |
| `imgui_impl_win32.cpp` | Win32 后端 |

### 12.2 midifile (MIDI 解析库)

**路径：** `midifile/`

| 文件 | 用途 |
|------|------|
| `MidiFile.h/cpp` | MIDI 文件读写 |
| `MidiEvent.h/cpp` | MIDI 事件处理 |
| `MidiEventList.h/cpp` | 事件列表 |
| `MidiMessage.h/cpp` | MIDI 消息解析 |

### 12.3 FTDI Driver

**路径：** `ftdi_driver/`

| 文件 | 用途 |
|------|------|
| `ftd2xx.h` | API 头文件 |
| `ftd2xx64.dll` | 64 位驱动 DLL |
| `libftd2xx.a` | MinGW 静态库 |

通信参数：1.5Mbps, 8N1, 无流控, 2ms 延迟

### 12.4 libvgm-modizer

**路径：** `libvgm-modizer/`

| 子目录 | 内容 |
|--------|------|
| `emu/cores/` | 40+ 芯片仿真实现 |
| `player/` | PlayerA、VGM/S98/GYM/DRO 播放器 |
| `audio/` | 多平台音频输出驱动 |

支持格式：VGM v1.0-1.70, VGZ (gzip), S98, GYM, DRO

### 12.5 scope_core_lib

**编译目标：** `scope_core_lib`（CMakeLists.txt:93-159）

提供 `scope_find_slot()` 和 `scope_find_slot_by_index()` 函数，将 VGM 通道映射到 Modizer 语音缓冲区，供示波器使用。

---

## 15. 构建系统

### CMakeLists.txt — 根目录

**项目名：** `DenjhangMusicPlayerV16`
**输出目录：** `${CMAKE_SOURCE_DIR}/bin`
**可执行文件：** `denjhang_music_player.exe`

**编译目标：**

| 目标 | 类型 | 源文件 |
|------|------|--------|
| `imgui_lib` | 静态库 | imgui/*.cpp |
| `midifile_lib` | 静态库 | midifile/src/*.cpp |
| `scope_core_lib` | 静态库 | libvgm-modizer 子集 |
| `denjhang_music_player` | 可执行文件 | src/*.cpp |

**项目源文件列表 — CMakeLists.txt:64-76**

```cmake
set(APP_SOURCES
    src/chip_control.cpp
    src/config_manager.cpp
    src/midi_player.cpp
    src/gui_renderer.cpp
    src/opl3_renderer.cpp
    src/vgm_parser.cpp
    src/vgm_window.cpp
    src/modizer_viz.cpp
    src/chip_window_ym2163.cpp
    src/opl3_window.cpp
    src/main.cpp
)
```

**链接库 — CMakeLists.txt:161-185**

- `imgui_lib`, `midifile_lib`, `scope_core_lib`
- `ftdi_driver/amd64/libftd2xx.a`（FTDI）
- `libvgm-player.a`, `libvgm-audio.a`, `libvgm-emu.a`, `libvgm-utils.a`
- `d3d11`, `dxgi`, `d3dcompiler`, `dwmapi`（DirectX 11）
- `ws2_32`, `gdi32`, `comdlg32`, `dsound`, `winmm`（Windows）
- `ole32`, `uuid`（COM）
- `z`, `iconv`（第三方）

### build_v16.sh

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

输出：`bin/denjhang_music_player.exe`

---

## 16. 版本演进

| 版本 | 日期 | 里程碑 |
|------|------|--------|
| v1.0 | 2026-01-17 | 初始原型，基本 YM2163 通信 |
| v2.0 | 2026-01-18 | ImGui 基础 GUI |
| v3.0 | 2026-01-19 | 钢琴键盘接口 |
| v4.0 | 2026-01-20 | 双芯片支持 |
| v5.0 | 2026-01-22 | MIDI 播放器集成 |
| v6.0 | 2026-01-25 | Unicode/CJK 支持 |
| v7.0 | 2026-01-26 | 完整文件浏览器 + 文件夹历史 |
| v8.0 | 2026-01-27 | 生产级发布（力度映射、精确计时） |
| v9.0 | 2026-01-28 | 功能完整版（动态力度分析、Win11 地址栏） |
| v14.0 | 2026-03-30 | VGM 播放器 + 多芯片可视化 |
| v14.1 | 2026-04-01 | VGM 可视化修复 |
| v15.0 | 2026-04-05~20 | 多芯片示波器、配置统一 |
| **v16.0** | **2026-04-28~05-04** | **DockSpace 多窗口、SN76489、Gigatron、YM2413 OPLL、SPFM 硬件管理** |

---

## 附录：全局变量索引

### 钢琴键状态 — `gui_renderer.h:30-35`

| 变量 | 类型 | 说明 |
|------|------|------|
| `g_pianoKeyPressed[73]` | bool | 按键是否按下 |
| `g_pianoKeyVelocity[73]` | int | 按键力度（0-127） |
| `g_pianoKeyLevel[73]` | float | 视觉强度（0.0-1.0），由 UpdateChannelLevels 驱动 |
| `g_pianoKeyChipIndex[73]` | int | 所属芯片（0-3, -1=无） |
| `g_pianoKeyFromKeyboard[73]` | bool | 是否来自 PC 键盘 |
| `g_pianoKeyOnSlot3_2MHz[73]` | bool | 是否在 Slot3 2MHz 上 |

### DX11 设备 — `gui_renderer.h:11-18`

| 变量 | 类型 | 说明 |
|------|------|------|
| `g_pd3dDevice` | ID3D11Device* | D3D11 设备 |
| `g_pd3dDeviceContext` | ID3D11DeviceContext* | 设备上下文 |
| `g_pSwapChain` | IDXGISwapChain* | 交换链 |
| `g_mainRenderTargetView` | ID3D11RenderTargetView* | 渲染目标 |
