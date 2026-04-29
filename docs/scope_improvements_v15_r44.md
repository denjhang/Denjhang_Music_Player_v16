# v15 示波器改进记录

**日期**：2026-04-13
**版本**：v15 R44
**作者**：Denjhang

---

## 11. 文件浏览器平滑自动滚动

### 背景

播放控件切换曲目时，文件浏览器需要自动滚动确保当前播放文件可见。

### 需求

1. 平滑插值滚动动画
2. 仅滚动1-2个条目的距离，不跳跃太快
3. 当前曲目始终可见
4. 切换上一曲/下一曲时，如果超出可视范围才滚动
5. 用户离开文件夹后停止跟踪

### 实现

- **预扫描渲染索引**：在渲染循环前扫描 `s_fileList`，找到正在播放文件的实际渲染索引
  - 考虑过滤器跳过的条目，确保索引与实际显示位置匹配
  - 基于 `entry.fullPath == s_currentPlayingFilePath` 匹配

- **窗口高度获取**：使用 `ImGui::GetWindowSize().y` 获取 BeginChild 区域的实际高度
  - 解决 VGM 播放器控件折叠时高度计算不准确的问题

- **静态变量**：
  - `s_scrollAnimY` — 当前平滑滚动位置
  - `s_trackedFolderPath` — 被跟踪的文件夹路径

- **LoadFile()** — 设置 `s_trackedFolderPath` 开始跟踪当前文件夹

- **RenderFileBrowser()** — 平滑滚动逻辑：
  - 只有当前文件夹与跟踪文件夹一致时才滚动
  - 检测播放文件是否在可视区域外（上方或下方）
  - 每帧最多滚动 1.5 个条目高度
  - 使用 0.2 插值系数实现平滑动画

- **NavigateTo()** — 切换文件夹时清除跟踪状态

### 文件

`vgm_window.cpp` — 静态变量, LoadFile(), RenderFileBrowser(), NavigateTo()
**状态**：完成

---

## 10. AY8910 噪音使能位修复

### Bug

AY8910 噪音钢琴键盘在噪音关闭时仍然显示琴键。

### 根因

AY8910 mixer 寄存器 (0x07) 的 enable 位是 **active low**：
- bit 0-2: tone enable (0=on, 1=off)
- bit 3-5: noise enable (0=on, 1=off)

代码错误地使用 `(mix & (0x08 << ch))` 检测噪音启用，实际检测的是禁用状态。

### 修复

改为 `!(mix & (0x08 << ch))` 检测噪音启用（bit=0 时启用）。

### 文件

`vgm_window.cpp` — BuildLevelMeters() AY8910 noise channel
**状态**：完成

---

## 9. 文件浏览器过滤器 (File Browser Filter)

### 背景

VGM 文件夹历史记录已有过滤器（`s_histFilter` + `InputTextWithHint` + tolower 匹配），
文件浏览器没有过滤器，用户需要过滤时只能滚动查找。

### 实现

- **标题栏过滤器**：在 "VGM File Browser" 标题栏右侧添加 `Filter...` 输入框
  - 使用 `TreeNodeEx` + `AllowOverlap` 标志，与文件夹历史记录布局一致
- **过滤逻辑**：tolower 子串匹配，与历史记录过滤器相同
- **上级目录始终显示**：`..` 条目不过滤
- **自动清空**：切换目录时自动清空过滤器，避免干扰

### 文件

`vgm_window.cpp` — NavigateTo(), RenderInlinePanel(), RenderFileBrowser()
**状态**：完成

---

## 7. 通道颜色自定义 (Channel Color Customization)

### 背景

此前每个芯片每个通道的颜色由 `shiftHue()` 自动生成（基于通道序号进行色相偏移），
用户无法自定义。颜色统一应用于示波器边框、钢琴键盘高亮、音量条。

### 实现

- **ScopeChipSettings** 添加 `ImU32 channel_colors[32]` 数组，存储每通道颜色覆盖值
  - `0` = 使用默认 `shiftHue` 自动配色
  - 非 `0` = 跳过 `shiftHue`，直接使用用户设定色
- **add() lambda** 修改：优先检查 `channel_colors[groupChIdx]`，有覆盖值时使用用户色
- **Scope Settings UI**：每芯片展开项底部新增 "Channel Colors" 区域
  - 每通道显示一个 `ImGui::ColorEdit4` 内联颜色编辑条 + 通道标签
  - 未设定颜色时显示默认色 + "(auto)" 标签
  - 每通道旁含 "Reset" 按钮恢复默认
  - "Reset All Colors" 按钮清除所有通道覆盖
- **配置持久化**：新增 `[ScopeColors]` 配置段
  - 格式：`0xDD=RRGGBBAA,RRGGBBAA,...`（逗号分隔，`00000000`=默认）
  - 只写入有自定义颜色的芯片行，减少配置文件膨胀

### 配置格式

```
[ScopeColors]
0x02=FF50C0FF,00FF50FF,00000000,00000000,00000000,00000000,00000000
0x12=FFA040FF,00000000,00000000,00000000
```

### 文件

`vgm_window.cpp` — ScopeChipSettings, add() lambda, RenderScopeSettingsWindow(), SaveConfig/LoadConfig
**状态**：完成

---

## 8. AY8910 噪音/包络通道钢琴键盘映射

### 背景

AY8910 有一个白噪音发生器和一个包络发生器，它们不是独立输出通道，
而是叠加在 3 个方波通道上。此前这两者没有钢琴键盘映射。

### 噪音通道

寄存器 0x06 控制噪音周期（5位，0-31），寄存器 0x07 控制各通道噪音使能（bit 3/4/5）。

emu2149 噪音频率：`noise_freq = (reg6 & 0x1F) << 1`（范围 2-62，0 时 fallback=2）。
内部时钟 `clk/8`，LFSR 每 64 个内部时钟周期移位一次。

```
噪音频率 = (clk/8) / (64 * noise_freq) = clk / (512 * noise_freq)
```

典型范围（clk=1773400）：
- noise_freq=2: 1731 Hz ≈ A6
- noise_freq=62: 55.8 Hz ≈ A1

### 包络通道

R38 已添加包络频率钢琴键盘映射，但音量硬编码为 1.0f。
改为绑定使用包络模式（0x08+ch bit4 置位）的通道中的最大音量 `(vol & 0x0F) / 15.0f`。

### 音量绑定

噪音和包络通道不是独立输出，其音量取决于使用它们的方波通道：
- **噪音**：取噪音使能（0x07 bit3/4/5）通道中的最大音量
- **包络**：取包络模式（0x08+ch bit4 置位）通道中的最大音量
- 多个通道使用同一功能时，取最大音量的一个通道
- 钢琴键盘颜色深浅直接关联实际音量，与其他通道一致

### 示波器过滤

噪音和包络通道没有独立的示波器 voice buffer（`voice_ch = -1`），
在 RenderScopeArea 中过滤掉这些通道，不显示示波器框。
仅保留钢琴键盘映射和寄存器显示。

### 文件

`vgm_window.cpp` — BuildLevelMeters() AY8910 case, RenderScopeArea() scopeMeters 过滤
**状态**：完成

---

## 5. AY8910 钢琴键盘频率修正

### 背景

AY8910 音调频率计算使用了 `ayClock / (8.0 * period)`，但根据 emu2149.c 的硬件模拟
（内部时钟 `clk/8`，12位计数器 `0x1000`），正确的除数应为 16。
旧公式比 YM2203 SSG 的 `clk/(32*period)` 高 2 个八度（16 倍），导致音符偏移。

### 修改

- **BuildLevelMeters**: `8.0f` → `16.0f`（下降一个八度）
- **寄存器查看器**: 同步修改

### 文件

`vgm_window.cpp` — BuildLevelMeters() AY8910 case, RenderStatusArea() 寄存器表格

---

## 6. AY8910 包络低音频率显示

### 背景

AY8910 的包络发生器可以产生低频周期波形（如锯齿波/三角波），常用于低音效果。
YASP 的 ay2opm 工具使用公式 `freq = clock / (16 * envelope_period * steps)` 计算包络频率，
其中 steps 取决于包络形状（锯齿波=32，三角波=64）。

### 实现

- **钢琴键盘**: 在 AY8910 的 3 个音调通道之后添加包络频率通道（标签 "E0"，橙色）
- **检测条件**: 任一通道的音量寄存器 bit4 置位（包络模式）且包络周期 < 200
- **频率公式**: `ayClock / (8 * efrq * steps)`
- **颜色**: `IM_COL32(255,160,60,255)` 橙色，区分于音调通道的蓝色
- **寄存器查看器**: Env Freq 行显示音符名 + 频率（Hz）

### 包络形状与 steps 对应

| Shape | Type | Steps |
|-------|------|-------|
| 8,9,11,12,13,15 | Sawtooth | 32 |
| 10,14 | Triangle | 64 |

### 文件

`vgm_window.cpp` — BuildLevelMeters() AY8910 case, RenderStatusArea() 寄存器信息表
**状态**：完成

## 改进概述

本次改进针对示波器系统的三个问题进行了修复和增强：

1. PCM 芯片静音时波形残留跳动
2. 切换曲目时旧波形残留
3. AY8910/YM2149 (SSG) 高音量时波形截顶

---

## 1. PCM 芯片静音时波形清零 (Clear on Silence)

### 背景

旧版本 (R15) 的示波器每帧重置缓冲区指针 (`m_voice_current_ptr[channel] = 0`)，
当没有新样本数据写入时直接不显示波形。新版本使用持续增长的环形缓冲区，
导致 PCM 芯片（SegaPCM、OKIM6295 等）在静音时旧波形数据残留在缓冲区中并持续跳动。

### 方案

在 `DrawChannel()` 函数中添加静音检测逻辑：
- 追踪每帧的 `write_ptr` 变化（`m_voice_prev_write_ptr[]` 数组）
- 如果 `write_ptr` 与上一帧相同，说明没有新数据写入
- 此时返回只显示空框（背景+零线，不绘制波形）
- 检测在互相关触发**之后**执行，不影响波形稳定性

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `modizer_viz.h` | 添加 `#include <cstdint>`，`m_voice_prev_write_ptr[]` 成员变量，`legacy_mode` 参数 |
| `modizer_viz.cpp` | 静音检测逻辑，`Init()`/`ResetOffsets()` 初始化新数组 |
| `vgm_window.cpp` | `ScopeChipSettings` 添加 `legacy_mode` 字段，设置 UI 添加 "Clear on Silence" 复选框 |

### 使用方法

打开 Scope Settings 窗口，为每个芯片勾选 "Clear on Silence" 选项。
建议为以下 PCM 芯片启用：SegaPCM、OKIM6295、OKIM6258、YM2608 ADPCM、YM2610 ADPCM。

---

## 2. 切换曲目时清除波形

### 方案

在 `LoadFile()` 函数中调用 `s_scope.ResetOffsets()`，清除所有通道的
持久偏移和静音检测状态，确保切换曲目时波形从空白状态开始。

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `vgm_window.cpp` | `LoadFile()` 中 `ResetShadowRegisters()` 之后添加 `s_scope.ResetOffsets()` |

---

## 3. SSG 示波器右移量可调 (SSG Scope Shift)

### 背景

AY8910/emu2149 (SSG) 示波器写入缓冲区时使用硬编码的 `>> 6` 右移。
emu2149 的信号链为：`ch_out = voltbl[volume] << 5`，最大值 = 0xFF × 32 = 8160。
经 `>> 6` 后为 127.5，刚好达到 LIMIT8 上限 (127)。
因此当 volume ≥ 10 时波形已经截顶，降低 amplitude 滑块无法恢复丢失的精度。

### 方案

添加全局变量 `g_ssg_scope_shift`（范围 0-10，默认 6）替代硬编码的 `>> 6`。
用户可在 Controls 面板中通过 "SSG Shift" 滑块调节。

| Shift 值 | 最大幅度 (emu2149) | 效果 |
|----------|-------------------|------|
| 6 (默认) | 127 (截顶) | 与之前一致 |
| 8 | 31 | 高音量时不截顶，细节可见 |
| 10 | 7 | 波形很小但永不截顶 |

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `libvgm-modizer/emu/cores/ModizerVoicesData.h` | 声明 `extern int g_ssg_scope_shift` |
| `modizer_viz.cpp` | 定义 `int g_ssg_scope_shift = 6` |
| `libvgm-modizer/emu/cores/emu2149.c` | 两处 `LIMIT8(val>>6)` → `LIMIT8(val>>g_ssg_scope_shift)` |
| `libvgm-modizer/emu/cores/ay8910.c` | `LIMIT8(val >> 6)` → `LIMIT8(val >> g_ssg_scope_shift)` |
| `vgm_window.cpp` | Controls 面板添加 "SSG Shift" 滑块，SaveConfig/LoadConfig 持久化 |

---

## 4. 配置持久化修复

### Bug

`LoadConfig()` 解析 `[ScopePerChip]` 时，`while (tok && n < 6)` 把浮点数 `width` ("90.0")
也当作整数解析到 `vals[5]`，导致 `legacy_mode` 值丢失、width/amplitude 错位。

### 修复

将整数解析限制为 `n < 5`（只解析 samples,offset,search_window,edge_align,ac_mode），
然后解析2个浮点数（width,amplitude），最后解析 legacy_mode。

### 配置格式

```
[ScopePerChip]
0x02=441,0,735,1,1,90.0,3.0,0
     │  │  │   │ │  │    │    └─ legacy_mode (0/1)
     │  │  │   │ │  │    └─ amplitude (float)
     │  │  │   │ │  └─ width (float)
     │  │  │   │ └─ ac_mode (int)
     │  │  │   └─ edge_align (int)
     │  │  └─ search_window (int)
     │  └─ offset (int)
     └─ samples (int)

[PlayerState]
SSGScopeShift=6
```
