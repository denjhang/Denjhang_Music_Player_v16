# SN76496 可视化修复记录

## 症状

加载 Alien3-SMS 文件夹中的 VGM 文件时，钢琴和电平表完全没有可视化输出。
调试日志显示：

```
Events parsed: 0
[Parser] dataOfs=0x0 dataEnd=0x893D GetSize=35319
[Parser] EARLY RETURN: filePos=0x0 GetSize=35319
```

以及噪音通道音量条始终为满（即使文件未写入噪音寄存器）。

---

## 根本原因（共三处）

### Bug 1：VGM v1.50 data offset 字段为 0 时未使用默认值

**文件**：`vgm_parser.cpp` — `ParseHeader()`

VGM v1.50 规范规定：0x34 处的相对偏移字段若为 0，表示使用默认值 0x40。
但 `ReadRelOfs()` 遇到字段值为 0 时直接返回 0，`ParseHeader` 未处理此特例，
导致 `_header->dataOfs = 0`，`ParseVGMCommands` 在起点就触发 EARLY RETURN。

**修复**：
```cpp
// 修复前
if (_header->fileVer >= 0x150)
    _header->dataOfs = ReadRelOfs(_fileData, 0x34);
else
    _header->dataOfs = 0x40;

// 修复后
if (_header->fileVer >= 0x150)
{
    _header->dataOfs = ReadRelOfs(_fileData, 0x34);
    if (!_header->dataOfs) _header->dataOfs = 0x40; // spec: field=0 means default 0x40
}
else
    _header->dataOfs = 0x40;
```

---

### Bug 2：VGM_CMD_INFO 表 0x20-0x27 段注释导致表错位

**文件**：`vgm_parser.cpp` — `VGM_CMD_INFO[]`

K005289 芯片不在 libvgm-modizer 中，本想注释掉其条目，但注释写在了同一行内，
实际上只定义了 0x20、0x21、0x22 三个条目，0x23-0x27 的五个条目被注释掉，
导致整个表从 0x23 开始全部错位，所有后续命令的 `chipType` 全部对应错误。

症状：日志中出现大量奇怪的 `chip_type=0x01~0x28` 事件，但 `chip_type=0x00`（SN76496）为零。

**修复**：
```cpp
// 修复前（错误地注释掉了5个条目）
{3, DEVID_AY8910, 0}, {2, 0, 0}, {3, 0, 0}, // K005289 not in modizer {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0},

// 修复后（补全全部8个条目，注释移至行尾）
{3, DEVID_AY8910, 0}, {2, 0, 0}, {3, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, // 0x20-0x27: 20=AY8910 22=K005289(2-byte, not in modizer)
```

---

### Bug 3：SN76496 衰减寄存器初始值为 0（最大音量）而非 0xF（静音）

**文件**：`vgm_window.cpp` — `ResetShadowRegisters()`

`memset` 将 `s_shadowSN76496` 全部初始化为 0。SN76496 的衰减寄存器值 0 = 最大音量，
0xF = 静音。上电复位时硬件实际为静音（0xF），但软件初始为 0，
导致文件加载后噪音通道音量条始终显示为满。

**修复**：
```cpp
memset(s_shadowSN76496, 0, sizeof(s_shadowSN76496));
// SN76496 power-on: all attenuation registers = 0xF (silent)
for (int c = 0; c < 2; c++)
    for (int ch = 0; ch < 4; ch++)
        s_shadowSN76496[c][ch*2+1] = 0xF;
```

---

## 修复后日志（正常）

```
Events parsed: 11437
[Parser] dataOfs=0x40 dataEnd=0x6348 GetSize=25590
    chip_type=0x00: 11437 events
```

---

## 教训

- `ReadRelOfs` 返回 0 有两种含义：字段本身为 0（需默认值），或偏移确实为 0（无效）。调用方需区分处理。
- 在已有 C++ 初始化列表的行内插入行内注释时，必须确保注释不会截断有效条目。
- 硬件寄存器的上电默认值不一定是 0，需查阅数据手册确认初始状态。
