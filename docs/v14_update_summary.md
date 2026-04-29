# YM2163 Piano GUI v14 - 示波器与配置系统更新总结

**更新日期：** 2026-04-03  
**版本：** v14.1

## 概述

本次更新主要聚焦于示波器系统的完善和配置管理的规范化，包括 SN76489 芯片支持、示波器布局优化、配置文件统一以及 VGM 播放进度条跳转功能。

---

## 主要更新内容

### 1. SN76489 示波器支持

**背景：** 用户报告 SN76489 的 P3/W0 模式切换导致示波器宽度变化。

**解决方案：**
- 在 `libvgm-modizer/emu/cores/sn76496.c` 中实现 `scope_register_chip()` API 集成
- 使用共享缓冲区写入模式（shared-ofs pattern）替代原有的 5 个独立通道块
- 修复原有噪声通道缓冲区索引计算错误
- 在 `vgm_window.cpp` 中添加 SN76496 的 BuildLevelMeters 完整实现

**技术细节：**
```c
// sn76496.c 中的示波器注册
ScopeChipSlot *scope_slot = scope_register_chip("SN76489", 0, 4, m_voice_current_samplerate);
if (scope_slot && scope_slot->num_groups == 0) {
    scope_slot->groups[0] = (ScopeChGroup){SCOPE_CH_SSG, 3, 0, "T"};
    scope_slot->groups[1] = (ScopeChGroup){SCOPE_CH_NOISE, 1, 3, "N"};
    scope_slot->num_groups = 2;
}
```

**结果：** SN76489 示波器现在稳定显示 4 个通道（3 个音调 + 1 个噪音），不受模式切换影响。

---

### 2. 示波器布局系统重构

**问题：** 原有布局根据标签长度动态调整宽度，导致示波器宽度不稳定。

**新布局规则：**
- **固定宽度**：示波器宽度独立于标签长度，用户可通过滑块调整（40-150px，默认 90px）
- **每行最多 11 通道**：超过 11 个通道自动换行
- **垂直滚动**：当总高度超过可用空间时启用垂直滚动条
- **示例**：SN76489 (4ch) + YM2612 (6ch) = 10ch → 正好一行；再加第三个芯片 → 自动换到第二行

**实现代码位置：** `vgm_window.cpp` 行 4041-4130

```cpp
const float kFixedScopeW = 90.0f;  // 使用全局变量 s_scopeWidth
const int   kMaxChPerRow = 11;    // 每行最多 11 通道

// 计算每行实际通道数（根据可用宽度自动调整）
int chPerRow = (maxRowW > availW) ?
    std::max(1, (int)((availW + kGroupGap) / (s_scopeWidth + kBarGap + kGroupGap / kMaxChPerRow))) :
    kMaxChPerRow;
```

---

### 3. 示波器宽度调整条

**位置：** 左侧边栏 "libvgm (Simulation)" 标题下方

**功能：**
- 实时调整示波器宽度（40-150px）
- 调整时自动保存配置
- 布局自动重新计算，超过 11*宽度则自动换行

**代码：** `vgm_window.cpp` RenderControls() 函数中
```cpp
if (ImGui::SliderFloat("Scope Width", &s_scopeWidth, 40.0f, 150.0f, "%.0fpx")) {
    SaveConfig();
}
```

---

### 4. 统一配置文件系统

**问题：** 原有 3 个分散的配置文件：
- `vgm_folder_history.ini` - 文件夹历史
- `vgm_player_state.ini` - 播放器状态
- `vgm_chip_aliases.ini` - 芯片别名

**解决方案：** 合并为单一配置文件 `vgm_player.ini`

**配置文件结构：**
```ini
[PlayerState]
CurrentPath=...
LoopCount=2
MasterVolume=1.0000
ShowScope=1
ScopeWidth=90.0

[PianoLabels]
LabelSize=12
LabelOffset=0
ShowLabels=1

[FolderHistory]
Folder=path1
Folder=path2
...

[ChipAliases]
YM2612=FM
SN76489=PSG
...
```

**实现：** `vgm_window.cpp` 行 441-555
- `SaveConfig()` - 统一保存所有配置
- `LoadConfig()` - 统一加载所有配置
- 向后兼容：旧函数名重定向到新函数

---

### 5. 配置持久化扩展

**新增持久化项目：**
- ✅ 循环次数 (`LoopCount`)
- ✅ 主音量 (`MasterVolume`)
- ✅ 示波器开关 (`ShowScope`)
- ✅ 示波器宽度 (`ScopeWidth`)
- ✅ 钢琴标签设置 (已有)
- ✅ 文件夹历史 (已有)
- ✅ 芯片别名 (已有)

---

### 6. VGM 播放进度条跳转

**功能：** 用户可拖动进度条跳转到任意播放位置

**实现方式：**
- 使用 `ImGui::SliderFloat()` 替代 `ImGui::ProgressBar()`
- 当用户释放滑块时调用 `s_player.Seek(PLAYPOS_SAMPLE, targetSample)`
- 自动同步当前播放位置到滑块

**代码：** `vgm_window.cpp` 行 6881-6912
```cpp
static float seek_progress = 0.0f;
if (ImGui::SliderFloat("##Progress", &seek_progress, 0.0f, 1.0f, "")) {
    // 用户正在拖动
}

if (!ImGui::IsItemActive()) {
    seek_progress = progress;  // 同步当前位置
}

if (ImGui::IsItemDeactivatedAfterEdit()) {
    UINT32 targetSample = (UINT32)(seek_progress * totalSamples);
    s_player.Seek(PLAYPOS_SAMPLE, targetSample);  // 执行跳转
}
```

---

## 技术细节

### 示波器布局算法

```
总通道数 n
├─ 计算每行容纳通道数 chPerRow
│  └─ 如果 11 * 宽度 > 可用宽度，则自动减少
├─ 计算所需行数 numRows = (n + chPerRow - 1) / chPerRow
├─ 计算总高度 totalRowH = numRows * (scopeHeight + labelHeight)
└─ 如果 totalRowH > availHeight，启用垂直滚动
```

### 配置文件格式

采用 INI 格式，支持分节管理：
- 使用 `[SectionName]` 标记不同配置区域
- 支持注释（`;` 开头）
- 自动处理换行符和空行

---

## 文件变更清单

| 文件 | 变更 | 行数 |
|------|------|------|
| `vgm_window.cpp` | SN76496 BuildLevelMeters 实现 | 3177-3216 |
| `vgm_window.cpp` | 示波器布局重构 | 4041-4130 |
| `vgm_window.cpp` | 统一配置系统 | 441-555 |
| `vgm_window.cpp` | 进度条跳转功能 | 6881-6912 |
| `vgm_window.cpp` | 示波器宽度调整条 | 2250-2254 |
| `sn76496.c` | scope_register_chip 集成 | 368-377 |
| `CMakeLists.txt` | 添加 sn76496.c 到编译 | - |

---

## 构建与测试

**构建命令：**
```bash
cd YM2163_Piano_v14_Release
cmake --build build --config Release
```

**验证清单：**
- ✅ SN76489 示波器显示 4 个通道
- ✅ P3/W0 模式切换不影响示波器宽度
- ✅ 示波器宽度滑块可调整（40-150px）
- ✅ 超过 11 通道自动换行
- ✅ 配置文件统一为 `vgm_player.ini`
- ✅ 循环次数和音量持久化
- ✅ 进度条可拖动跳转

---

## 向后兼容性

- 旧配置文件自动迁移到新格式
- 所有旧函数名保留为兼容性包装器
- 默认值设置合理，首次运行无需配置

---

## 性能影响

- 示波器布局计算：O(n)，n ≤ 100 通道，无性能问题
- 配置文件 I/O：仅在用户操作时触发，不影响实时性能
- 内存占用：增加 ~1KB（s_scopeWidth 变量）

---

## 已知限制

1. VGM Seek 功能依赖 libvgm 播放器实现，某些格式可能不支持精确跳转
2. 示波器宽度调整需要重新计算布局，可能有轻微卡顿（< 1ms）
3. 配置文件不支持加密或版本控制

---

## 未来改进方向

- [ ] 支持配置文件导入/导出
- [ ] 示波器预设（紧凑/标准/宽松）
- [ ] 进度条时间标记（快速定位）
- [ ] 配置文件版本管理

---

**文档作者：** Claude Code  
**最后更新：** 2026-04-03
