# Denjhang's Music Player - Complete Version History

## Version Timeline

**v1.0** → **v2.0** → **v3.0** → **v4.0** → **v5.0** → **v6.0** → **v7.0** → **v8.0** → **v9.0** → **v14.0** → **v14.1** → **v15.0** → **v16.0**

---

## v16.0 (2026-04-28 ~ 2026-04-29)

### 架构重构

#### Chrome 风格可拖拽多窗口标签系统（2026-04-29）
- ✨ 升级 ImGui 到 docking 分支（v1.92.8 WIP），原生 DockSpace + Viewports
- ✨ 自定义 ChipTab 枚举 + 任务栏按钮替换为 ImGui DockSpace
- ✨ 三个窗口作为标签页：`YM2163(DSG)`、`YMF262(OPL3)`、`libvgm`
- ✨ 左右拖拽标签改变顺序，上下拖拽拆分为独立 Windows 系统窗口
- ✨ 拆分窗口带系统标题栏，支持 Windows Dock/贴靠功能
- ✨ 独立窗口可拖回主窗口重新合并
- 🔧 `io.ConfigViewportsNoDecoration = false` 显示 OS 标题栏
- 🔧 布局自动保存到 `imgui.ini`（ImGui docking 内建功能）
- 🔧 Render() 签名从 `Render(ImVec2, ImVec2)` 改为无参数，自管理 ImGui::Begin/End
- 🔧 新增 `VgmWindow::RenderTab()` 从 main.cpp 提取 VGM 标签渲染逻辑
- 📄 包含 `imgui_internal.h` 用于 DockBuilder API

#### 项目重组（2026-04-29）
- ✨ 程序改名为 **Denjhang's Music Player** v16
- 🔧 源码移入 `src/`，编译产物到 `bin/`，文档到 `docs/`，旧版到 `legacy/`
- 🔧 源文件重命名：`ym2163_control` → `chip_control`，`ym2163_window` → `chip_window_ym2163`
- 🔧 CMakeLists.txt 项目名 `DenjhangMusicPlayerV16`，输出 `denjhang_music_player.exe`
- 🔧 配置文件按窗口拆分为 4 个 INI：
  - `ym2163_config.ini` — YM2163 窗口（tuning、Slot、128 乐器、MIDI 文件夹历史）
  - `opl3_config.ini` — OPL3 窗口（文件夹历史）
  - `vgm_config.ini` — VGM 窗口（播放器状态、UI、示波器、文件夹历史）
  - `imgui.ini` — DockSpace 布局
- 🔧 MIDI/OPL3 文件夹历史改用 `GetPrivateProfileString` INI API（避免合并后的纯文本冲突）

#### 窗口模块化解耦（2026-04-28）
- 🔧 YM2163、OPL3、libvgm 三个标签页独立为模块
- 📄 新文件：`ym2163_window.cpp/h`、`opl3_window.cpp/h`、`window_interface.h`
- 🔧 `main.cpp` 简化为窗口管理器 + DockSpace 宿主

#### 基于可见性的自动暂停/恢复（2026-04-29）
- ✨ 基于 `ImGui::Begin()` 返回值检测窗口可见性（标签不活动 = 不可见）
- ✨ 标签隐藏时自动暂停 MIDI/VGM，标签激活时自动恢复
- ✨ 区分用户暂停（`g_midiUserPaused`）和自动暂停（`s_midiAutoPaused`）
- 🔧 `PlayMIDI()` 恢复时重置 `g_midiUserPaused = false`
- 🔧 删除旧的 `ChipTab` 枚举、`g_activeChipTab`、`g_chipTabNames`、`TASKBAR_HEIGHT`
- 🔧 删除旧的任务栏渲染代码和标签切换暂停逻辑
- 🔧 删除 `SaveMainState()`/`LoadMainState()`（布局由 `imgui.ini` 管理）

#### 标签页切换自动暂停/恢复（2026-04-28，已被可见性暂停替代）
- 🐛 修复：`PlayMIDI()` 恢复时重置 `lastPerfCounter`，防止进度条跳变
- 🐛 修复：`ResetPianoKeyStates()` 实际清空钢琴键状态（原为空函数）

### 可视化改进

#### 钢琴键盘芯片颜色
- ✨ 按键着色对应芯片 Slot 颜色：Slot0=绿, Slot1=蓝, Slot2=红, Slot3=橙
- 🔧 `g_pianoKeyChipIndex[73]` 追踪按键所属芯片

#### 钢琴键盘动态衰减颜色
- ✨ 颜色深浅随力度和衰减时间实时变化
- 🔧 新增 `g_pianoKeyLevel[73]` 数组，由 `UpdateChannelLevels()` 每帧更新
- 🔧 颜色深浅 = `channelLevel × velocity/127`，跟随 ADSR 包络变化
- 🔧 释放后颜色自然衰减至消失（不再突然消失）
- 🔧 `stop_note()` 不再立即清除钢琴键状态，由衰减逻辑自动清除
- 🔧 衰减速率与音量条一致（Decay=50, Fast=25, Medium=10, Slow=4）

#### 音量条配色与衰减
- 🔧 恢复 v12 渐变配色：Blue→Green→Yellow→Red
- 🔧 衰减加快：Decay=50, Fast=25, Medium=10, Slow=4，平滑因子 0.5

#### 钢琴键盘修复
- 🐛 修复 v15 白键/黑键八度编号不匹配

### Bug 修复
- 🐛 `PauseMIDI()` 覆盖自动暂停标志 → 改为直接操作播放状态
- 🐛 `g_midiUserPaused` 链接错误 → 移到全局作用域
- 🐛 MIDI 恢复后进度条跳变 → 重置 `lastPerfCounter`
- 🐛 曲目切换钢琴键残留 → 实现 `ResetPianoKeyStates()`
- 🐛 FTDI 波特率错误（3Mbps→1.5Mbps）修复硬件无声（v15 移植）

### 修改文件清单
| 文件 | 操作 |
|------|------|
| `imgui/*` | **替换** — 升级到 docking 分支 v1.92.8 WIP |
| `src/window_interface.h` | 新建→修改 - 窗口接口，Render() 无参数签名 |
| `src/chip_window_ym2163.cpp/h` | 新建→修改 - DockSpace 模式，自管理 Begin/End，可见性暂停 |
| `src/opl3_window.cpp/h` | 新建→修改 - DockSpace 模式，自管理 Begin/End |
| `src/vgm_window.h` | 修改 - 新增 RenderTab() 声明 |
| `src/vgm_window.cpp` | 修改 - 新增 RenderTab() 实现（从 main.cpp 提取） |
| `src/main.cpp` | **大幅重构** - DockSpace 替换自定义标签，Viewports 多窗口渲染 |
| `src/midi_player.cpp` | 修改 - PlayMIDI() 恢复时重置 g_midiUserPaused；MIDI 文件历史改用 INI API |
| `src/midi_player.h` | 修改 - 添加全局暂停标志 |
| `src/chip_control.cpp` | 修改 - 钢琴键衰减、芯片颜色 |
| `src/chip_control.h` | 修改 - ChannelState 添加 previousLevel |
| `src/config_manager.cpp` | 修改 - 配置文件路径更新 |
| `src/gui_renderer.cpp` | 修改 - 键盘释放不再直接清除状态 |
| `src/gui_renderer.h` | 修改 - 添加 g_pianoKeyLevel 声明 |
| `src/gui_renderer_impl.cpp` | 修改 - 动态衰减颜色渲染、tooltip 更新 |
| `src/opl3_renderer.cpp` | 修改 - OPL3 文件夹历史改用 INI API |
| `CMakeLists.txt` | 修改 - 项目名 DenjhangMusicPlayerV16，输出到 bin/ |

---

## v15.0 (2026-04-05 ~ 2026-04-20)

### VGM 播放器增强
- ✨ 多芯片示波器支持（YM2151/OPM, YM2203/OPN, YM2608/OPNA, YM2610, YM2612, AY8910, SN76489, SCC, YM3526/OPL, YM3812/OPL2, Y8950, YMF262/OPL3）
- ✨ OPL 系列示波器：4op 修正、多实例支持
- ✨ 双芯片示波器改进（指针地址区分实例）
- ✨ 示波器自动换行布局（固定宽度 40-150px，每行最多 11 通道）
- ✨ 统一配置文件 `vgm_player.ini`
- ✨ VGM 进度条跳转
- ✨ BuildLevelMeters 寄存器显示（MDPlayer 风格）
- ✨ 频率公式修复（oplNote、GB DMG、AY8910、K051649、C6280）

---

---

## v14.1 (2026-04-01)

### VGM 可视化系统重大修复

#### Bug Fixes
- 🐛 **YM2413 旋律通道**：修复重复音符触发不闪烁，使用 MDPlayer 的 `key_on_event` 单帧脉冲机制
- 🐛 **YM2413 鼓声通道**：修复动画迟钝问题，将边沿检测从渲染帧移至事件处理，与 MDPlayer 完全一致
- 🐛 **AY8910 噪音通道**：修复纯噪音模式不显示钢琴键，使用 R6 噪音周期计算音高

#### 技术改进
- 🔧 **影子寄存器架构**：完善循环检测（`PLAYTIME_LOOP_EXCL`），确保第二循环后可视化继续更新
- 🔧 **SUS 位处理**：`keyoff && !sus` 立即清零 decay，符合硬件行为（参考 MDPlayer）
- 🔧 **初始状态修正**：`s_ym2413KeyOff` 初始化为 `true`，确保第一次 keyon 就能触发

#### 渲染端改进
- 🔧 YM2413 旋律：`keyoff` 快速衰减 `*=0.60f`，`keyon` 慢速衰减 `*=0.90f`
- 🔧 YM2413 鼓声：改用 `ChVizState` 结构，复用 `key_on_event` 机制
- 🔧 AY8910 噪音：频率公式 `clock / (16 * noise_period * 8)`，下移 3 个八度映射钢琴键

#### Documentation
- 📄 `docs/vgm_visualization_fixes_v14.md` — VGM 可视化系统修复完整技术文档

---

## v14.0 (2026-03-30) - Current Release

### Major Features
- ✨ **VGM/VGZ/S98 播放器**：集成 libvgm-modizer，支持芯片音乐文件播放
- ✨ **VGM 事件解析器**：自定义解析器提取寄存器写入事件，支持 GD3 标签（中英文）
- ✨ **多芯片电平表**：每通道独立音量条，颜色随音量动态变暗（亮度系数 0.4~1.0）
- ✨ **钢琴键芯片映射**：芯片频率实时映射到钢琴键，颜色亮度随音量调制（0.5~1.0）
- ✨ **SN76496 噪音可视化**：动态标签（W0-W3/P0-P3），白噪音/周期噪音分色显示
- ✨ **芯片别名窗口**：可为每个芯片实例自定义显示名称

### Bug Fixes
- 🐛 修复 VGM v1.50 data offset=0 未回退默认值，导致解析 0 事件
- 🐛 修复 `VGM_CMD_INFO[]` 行内注释截断条目，导致命令表从 0x23 起全部错位
- 🐛 修复 SN76496 衰减寄存器初始化为 0（最大音量）而非 0xF（静音）
- 🐛 修复编译警告 `-Wmisleading-indentation`，实现零警告编译

### Documentation
- 📄 `docs/vgm_player_v14.md` — VGM 播放器功能与编译方法完整文档
- 📄 `docs/bugfix_sn76496_visualization.md` — SN76496 可视化三处 Bug 根因与修复记录

---

## v9.0 (2026-01-28)

### Major Features
- ✨ **Dynamic Drum Allocation System**: Drums alternate between two chips in dual-chip mode
- ✨ **File Browser Highlight System**: Yellow highlight for exited folders, blue for playing paths
- ✨ **Filename Scrolling Display**: Auto-scrolling for long filenames on hover/select
- ✨ **Scroll Position Memory**: Remembers scroll position when navigating folders
- ✨ **Long Path Support**: Supports paths exceeding 260 characters
- ✨ **Win11-Style Address Bar**: Breadcrumb navigation with folder name abbreviation
- ✨ **Dynamic Volume Mapping**: Intelligent MIDI velocity analysis and mapping

### Improvements
- 🔧 Progress bar seek optimization (fixed no response/burst notes issue)
- 🔧 Track switch optimization (clears residual piano keys)
- 🔧 Enhanced Unicode filename support
- 🔧 Single-click unified file browser operation
- 🔧 Improved error messages for file loading

### Bug Fixes
- 🐛 Fixed drum UI showing on both chips simultaneously
- 🐛 Fixed progress bar seek issues
- 🐛 Fixed piano key residual when switching tracks
- 🐛 Fixed folder highlight flashing
- 🐛 Fixed scroll position not restored
- 🐛 Fixed long path loading errors

---

## v8.0 (2026-01-27)

### Features
- MIDI playback with high-precision timing
- Dual YM2163 chip support (8 channels total)
- Real-time piano keyboard visualization
- Channel status display
- Velocity mapping support
- Sustain pedal support

### Improvements
- Improved MIDI event processing
- Better channel allocation
- Enhanced UI responsiveness

---

## v7.0 (2026-01-26)

### Features
- File browser with navigation history
- MIDI folder history
- Auto-play next track
- Sequential/random playback modes
- Global media key support

### Improvements
- Win11-style UI design
- Better file organization
- Improved navigation

---

## v6.0 (2026-01-25)

### Features
- Unicode path support
- CJK font support
- Multi-language file names
- Improved file browser

### Bug Fixes
- Fixed Unicode display issues
- Fixed path handling bugs

---

## v5.0 (2026-01-22)

### Features
- MIDI player integration
- Progress bar with seek support
- Time display (MM:SS format)
- Play/Pause/Stop controls

### Improvements
- Better MIDI timing
- Smooth progress animation

---

## v4.0 - v4.5 (2026-01-20 to 2026-01-21)

### Features (v4.0)
- Dual chip support
- Channel allocation system
- Envelope control
- Wave/timbre selection

### Improvements (v4.1-v4.5)
- High DPI support
- Crash fixes
- Frequency table improvements
- UI refinements

---

## v3.0 (2026-01-19)

### Features
- Piano keyboard interface
- Real-time note visualization
- Octave control
- Volume control

### Improvements
- Better keyboard mapping
- Improved visual feedback

---

## v2.0 (2026-01-18)

### Features
- Basic GUI with ImGui
- Note playback
- Simple controls

---

## v1.0 (2026-01-17)

### Features
- Initial release
- Basic YM2163 communication
- Simple note playback

---

## Feature Evolution Summary

### Audio Engine
- v1.0: Basic note playback
- v3.0: Piano keyboard interface
- v4.0: Dual chip support
- v5.0: MIDI playback
- v8.0: High-precision timing
- v9.0: Dynamic drum allocation

### File Handling
- v5.0: Basic file loading
- v6.0: Unicode support
- v7.0: File browser with history
- v9.0: Long path support, scrolling filenames

### User Interface
- v2.0: Basic ImGui interface
- v3.0: Piano keyboard visualization
- v4.0: Channel status display
- v7.0: Win11-style design
- v9.0: Advanced highlighting, scrolling text

### MIDI Features
- v5.0: Basic MIDI playback
- v6.0: Better timing
- v8.0: Velocity mapping
- v9.0: Dynamic volume mapping, precise seeking

---

## Statistics

- **Total Versions**: 9 major releases
- **Development Period**: January 17-28, 2026 (12 days)
- **Total Features Added**: 50+
- **Total Bug Fixes**: 30+
- **Code Size**: ~4000 lines → ~3800 lines (optimized)
- **Supported Formats**: MIDI (.mid, .midi)
- **Supported Chips**: YM2163 (1-2 chips, 4-8 channels)

---

## Key Milestones

1. **v1.0**: First working prototype
2. **v3.0**: Piano keyboard interface
3. **v4.0**: Dual chip support breakthrough
4. **v5.0**: MIDI player integration
5. **v6.0**: Unicode support
6. **v7.0**: Complete file browser
7. **v8.0**: Production-ready release
8. **v9.0**: Feature-complete with advanced UI

---

## Technical Achievements

### Performance
- High-precision timing (microsecond accuracy)
- Efficient channel allocation
- Smooth UI rendering
- Low CPU usage

### Compatibility
- Windows 10/11 support
- Unicode/CJK support
- Long path support
- High DPI support

### User Experience
- Intuitive interface
- Responsive controls
- Visual feedback
- Error handling

---

## Future Development

### Planned Features
- Playlist support
- MIDI editing
- Recording capabilities
- More chip support
- Advanced tuning interface
- Spectrum analyzer

### Potential Improvements
- Performance optimization
- More file formats
- Cloud integration
- Mobile version

---

**Project Start**: January 17, 2026
**Current Version**: v16.0
**Last Updated**: April 29, 2026
**Status**: Active Development
