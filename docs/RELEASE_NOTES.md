# Denjhang's Music Player v16 Release Notes

## Release Date
April 28, 2026 (Updated May 6, 2026)

## Version
v16.0 (Build 2026-05-06)

## Major Improvements

### Chrome-Style Dockable Multi-Window System (NEW)
- 升级 ImGui 到 docking 分支（v1.92.8 WIP），原生支持 DockSpace + Viewports
- 自定义任务栏标签系统替换为 ImGui DockSpace
- 三个窗口作为标签页显示：`YM2163(DSG)`、`YMF262(OPL3)`、`libvgm`
- 左右拖拽标签可改变标签顺序
- 上下拖拽标签拆分为独立 Windows 系统窗口（带系统标题栏）
- 独立窗口支持 Windows 系统 Dock/贴靠功能
- 独立窗口可拖回主窗口重新合并
- 布局自动保存到 `imgui.ini`，重启后恢复
- 窗口模块 Render() 重构：自管理 ImGui::Begin/End，无参数签名

### Visibility-Based Auto Pause/Resume (NEW)
- 基于 ImGui::Begin() 返回值检测窗口可见性
- 标签不活动或窗口隐藏时自动暂停 MIDI/VGM 播放
- 标签重新激活或窗口显示时自动恢复播放
- 区分用户暂停和自动暂停：用户手动暂停后切回不会自动恢复

### Window Module Decoupling (NEW)
- YM2163、OPL3、libvgm 三个标签页独立为模块（`ym2163_window`, `opl3_window`, `vgm_window`）
- `main.cpp` 简化为窗口管理器
- 新增 `VgmWindow::RenderTab()` 从 main.cpp 提取 VGM 标签渲染逻辑

### Piano Keyboard Dynamic Color (NEW)
- 按键颜色对应芯片 Slot：Slot0=绿, Slot1=蓝, Slot2=红, Slot3=橙
- 颜色深浅随力度(velocity)和 ADSR 衰减实时变化
- 释放后颜色自然衰减至消失（不再突然消失）
- 新增 `g_pianoKeyLevel[73]` 驱动视觉强度

### Volume Meter Improvements
- 恢复 v12 渐变配色：Blue→Green→Yellow→Red
- 衰减加快：Decay=50, Fast=25, Medium=10, Slow=4

### SN76489 (DCSG) Hardware Window (NEW)
- SPFM Light 接口驱动 SN76489 芯片，VGM 文件实时硬件播放
- T6W28 (NeoGeoPocket) 双芯片模式：slot0 方波 + slot1 噪音独立转发
- 钢琴键盘 VGM blendKey 着色（音量→颜色渐变）
- 噪音通道钢琴显示（白噪音固定映射、周期性噪音频率计算、shift 三档映射）
- 10 通道独立颜色（Tone0/1/2 + Periodic/White Noise × 2 芯片），自定义颜色持久化
- 双芯片音量条、寄存器表格，Dual Chip 模式下仅显示实际发送到硬件的数据
- 硬件测试（音阶/琶音/和弦/音量扫描/噪音）+ 通道控制面板

### Gigatron Tracker Player Module (NEW)
- Gigatron TTL 4 通道音频仿真（振荡器相位累加 + 波表查表）
- 4 种内置波形（Noise/Triangle/Pulse/Sawtooth）+ 自定义波表编辑器
- 高精度波表支持（6/8/12/16-bit），鼠标手绘 + 9 种预设
- 钢琴键盘（C1-C8）、4 通道 VU 电平表、示波器（互相关触发 + AC 耦合）
- `.c` / `.gbas.c` 文件解析，文件浏览器 + 文件夹历史
- 播放控制：速度调节、段落选择、自动跳过静音

### YM2413 (OPLL) VGM Player Module (NEW)
- YM2413 寄存器影子跟踪 + key-on/bit4 上升沿/下降沿检测
- 节奏通道（BD/SD/TOM/HH/CYM）独立 key-on 检测与音量提取
- 钢琴键盘：9 旋律通道 + 5 节奏通道，VIB/AM/滑音指示器
  - VIB: 整条键高度左右摆动（±0.14 半音，硬件 14 cent）
  - AM: 键顶部上下脉冲条（±0.3，硬件 4.8dB）
  - 滑音: key-on 期间连续频率变化，>1 半音跳变不算滑音
- 14 通道颜色自定义（ColorEdit4 UI），INI 持久化
- 指令刷新模式（Flush Mode）：Register-Level（每次寄存器写后 flush）/ Command-Level（每条 VGM 命令后 flush）
- 定时器模式（Timer Mode）：H-Prec Sleep / Hybrid Sleep / MM-Timer / VGMPlay / Optimized VGMPlay
  - H-Prec: waitable timer + spin-wait 高精度定时
  - Hybrid: Sleep + spin-wait 混合，减少 CPU 占用
  - MM-Timer: timeSetEvent 多媒体定时器
  - VGMPlay: 1ms periodic multimedia timer + QPC sample counting
  - Optimized VGMPlay: VGMPlay + lookahead 批量预读连续寄存器写入，减少 USB 事务
  - **推荐 VGMPlay 模式**：使用 timeSetEvent 周期定时器，系统保证回调优先级，即使 CPU 满载也能被调度；QPC sample counting 自动补偿被抢占的延迟，不会丢拍。模式 0/1/2 的 sleep 精度依赖 CPU 空闲，忙时 Sleep(1) 可能变为 ~15ms 导致音频断续。Optimized VGMPlay 与 VGMPlay 定时精度相同，密集寄存器写入场景（快速琶音等）下更高效，一般场景用 VGMPlay 即可
- 快进模式：逐命令重放，`MuteHardwareOnly` 保留影子状态
- 曲目切换静音：key-off → rhythm off → TL=0x0F → rhythm silence
- 示波器波形显示、寄存器表格、GD3 标签

### AY8910 (PSG) Hardware Window (NEW)
- SPFM Light 接口驱动 AY8910 芯片，VGM 文件实时硬件播放
- AY8910 寄存器影子跟踪（R0-R13），Key-on/key-off 音量边沿检测
- 钢琴键盘：3 旋律通道（Tone A/B/C）+ 噪音通道，blendKey 着色
- 噪音通道频率计算与音符映射，Portamento 滑音可视化
- 4 通道独立颜色（Tone A/B/C + Noise），自定义颜色持久化
- 双芯片音量条、寄存器表格、示波器波形显示
- GD3 标签、循环播放、快进模式、循环淡出、无循环自动切曲
- AY8910 必须用 `spfm_write_reg` 单次写入（非双写地址锁存模式）

### SPFM Light Hardware Manager (NEW)
- 统一 SPFM Light 硬件连接/断开管理窗口
- 4 Slot 芯片类型配置（YM2413/AY8910/SN76489/None）
- Slot 预设持久化：按芯片组合自动保存/恢复到 `bin/slot_presets.ini`
- 双芯片 VGM 自动检测（header clock bit30）并分配 Slot
- 多窗口连接状态同步：共享 `s_connected`，一个窗口操作影响所有窗口

### Unified VGM Playback Engine (NEW)
- **单线程解析 + 多芯片分发**：只有一个统一播放线程，消除多窗口节奏不同步
- 回调注册机制：每个窗口在 Init() 时注册寄存器写入/硬件写入/flush 回调
- 支持 YM2413 (0x51)、AY8910 (0xA0)、SN76489 (0x50/0x30) 芯片分发
- QPC+Sleep(1) 高精度定时，每帧最大 1 秒安全上限
- 共享进度：所有窗口 UI 读取 `VGMSync::GetCurrentSamples()` 同一值
- SN76489 特殊处理：`SnCmdHandler` 封装完整 T6W28/latch/mute/PeriodicNoiseFix 逻辑

## Bug Fixes
- Fixed MIDI resume progress bar jump (lastPerfCounter not reset)
- Fixed piano key residual after track switch (ResetPianoKeyStates was empty)
- Fixed PauseMIDI() overriding auto-pause flag
- Fixed g_midiUserPaused linker error (namespace scope)
- Fixed piano keyboard octave numbering mismatch (v15 regression)
- Fixed T6W28 Dual Chip 0x30 forwarding: ch2 tone volume incorrectly sent to slot1
- Fixed T6W28 Dual Chip 0x30 data byte leaking ch0/ch1 to slot1
- Fixed T6W28 Dual Chip 0x50 shadow state updating ch3 despite not sending to hardware
- Fixed 2nd chip reset: activate channels before muting
- Fixed getChColor slot1 noise color index mapping overflow
- Fixed periodic noise frequency: LFSR output = square wave / 16, down 3 octaves
- Fixed Gigatron wavA volume mapping: actual range 64-127 (64=max, 127=mute)
- Fixed custom wave table silence on all bit depths: mod instead of clamp for wavA offset
- Fixed Gigatron audio clipping: volume scaling + saturation limiting
- Fixed YM2413 tab not persisting on startup (DockBuilder overriding imgui.ini layout)
- Fixed YM2413 rhythm channel key-on detection and volume extraction (wrong nibble)
- Fixed YM2413 fast-forward losing instrument info (mute clearing shadow state)
- Fixed YM2413 song transition click sound (TL 0xF0 modifying patch, wrong silence sequence)
- Fixed unified playback thread `last` QPC counter uninitialized, causing instant file dump on first iteration
- Fixed `NotifyFileOpened` missing from 9 trigger points (double-click/Next/Prev in 3 windows)
- Fixed `s_connected` mutual exclusion: `SyncConnectionState` checked `GetActiveChipType()` making only one window "connected" at a time
- Fixed `!s_connected` resetting `s_vgmPlaying` every frame, killing playback on hardware disconnect

## Project Reorganization (April 29, 2026)
- 程序正式改名为 **Denjhang's Music Player** v16
- 源码按功能分类到 `src/` 目录，编译产物到 `bin/` 目录
- 文档统一到 `docs/`，旧版文件归档到 `legacy/`
- 配置文件按窗口拆分：`ym2163_config.ini`、`opl3_config.ini`、`vgm_config.ini`
- `imgui.ini` 保留用于 DockSpace 布局持久化

### Directory Structure
```
Denjhang_Music_Player_v16/
├── src/          ← 所有源码（25 个文件）
│   ├── sn76489/  ← SN76489 驱动子目录（spfm.h, spfm_lite.c, sn76489.h）
├── bin/          ← 可执行文件 + 配置文件（5 个 INI）
│   ├── denjhang_music_player.exe
│   ├── ym2163_config.ini   # YM2163 窗口配置
│   ├── opl3_config.ini     # OPL3 窗口配置
│   ├── vgm_config.ini      # VGM 窗口配置
│   ├── sn76489_config.ini  # SN76489 窗口配置（颜色、shift 映射）
│   └── imgui.ini          # DockSpace 布局
├── docs/         ← 所有文档
├── legacy/       ← v11-v15 旧版文件
├── imgui/ ftdi_driver/ midifile/ libvgm-modizer/  ← 第三方库
└── tools/ vgms/
```

## Building from Source
```bash
cd Denjhang_Music_Player_v16
./build_v16.sh
```

## System Requirements
- Windows 10/11 (64-bit)
- SPFM hardware with YM2163 chip(s)
- FTDI USB driver

---

# YM2163 Piano GUI v9 Release Notes

## Release Date
January 28, 2026

## Version
v9.0 (Build 2026-01-28)

## Major Improvements

### Dynamic Volume Mapping (NEW)
- **Automatic MIDI Analysis**: Analyzes velocity distribution when loading MIDI files
- **Intelligent Mapping**:
  - Most common velocities → -6dB and -12dB
  - Peak velocities (95th percentile) → 0dB
  - Very low velocities → Mute
- **Adaptive Control**: Provides better volume balance for different MIDI files
- **Logging**: Detailed velocity analysis logged for debugging

### Unicode File Name Support (ENHANCED)
- **CJK Font Support**: Full support for Chinese, Japanese, Korean characters
- **Font Merging**: Combines multiple fonts for complete character coverage
  - Primary: Microsoft YaHei (中文)
  - Secondary: Malgun Gothic (한글)
  - Tertiary: MS Gothic (日本語)
- **UTF-8 Handling**: Proper string handling throughout application
- **File Browser**: Correctly displays international file names

### Win11-Style Address Bar (NEW)
- **Breadcrumb Navigation**: Click directory buttons to navigate
- **Smart Truncation**: Shows "..." for long paths, prioritizes current directory
- **Dual Mode**:
  - Breadcrumb mode (default): Click buttons to navigate
  - Text input mode: Double-click to enter path directly
- **Up Arrow Button**: Quick navigation to parent directory

### File Browser Improvements (ENHANCED)
- **Accurate Width Calculation**: Prevents button text overflow
- **Smart Layout**: Prioritizes rightmost (current) directory visibility
- **UTF-8 String Handling**: Uses std::string for proper multi-byte character support
- **Better Spacing**: Improved button sizing and alignment

### Chip Reset on Track Switch (NEW)
- **Automatic Reset**: YM2163 chips reset when switching tracks
- **Eliminates Residual Sound**: Clears all lingering notes and sounds
- **Dual Chip Support**: Works with both Slot0 and Slot1
- **Smooth Transitions**: Clean audio transitions between tracks

### History Directory Sorting (ENHANCED)
- **Time-Based Sorting**: Most recently opened directories appear first
- **Automatic Reordering**: Revisiting a directory moves it to the top
- **Smart Caching**: Keeps up to 20 most recent MIDI folders
- **Persistent Storage**: Saved to ym2163_folder_history.ini

### Time-Driven MIDI Progress (CONFIRMED)
- **High-Precision Timing**: Uses QueryPerformanceCounter (microsecond accuracy)
- **Smooth Animation**: Progress bar moves smoothly without jumps
- **Accurate Display**: Shows MM:SS format for current and total time
- **Seek Support**: Drag progress bar to quickly jump to any position

## Bug Fixes
- Fixed layout width changes when toggling Slot1
- Fixed file browser Unicode character display
- Fixed double-click file loading without chip reset
- Fixed address bar focus loss issues
- Fixed progress bar calculation accuracy

## Building from Source
```bash
cd YM2163_Piano_v9_Release
./build_v9.sh
```
