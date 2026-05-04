# Denjhang's Music Player v16 Release Notes

## Release Date
April 28, 2026 (Updated May 4, 2026)

## Version
v16.0 (Build 2026-05-04)

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
- 快进模式：逐命令重放，`MuteHardwareOnly` 保留影子状态
- 曲目切换静音：key-off → rhythm off → TL=0x0F → rhythm silence
- 示波器波形显示、寄存器表格、GD3 标签

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
