# OKIM6258 / OKIM6295 示波器适配记录

**日期**：2026-04-05
**版本**：v15
**作者**：Denjhang
**状态**：完成

## 背景

OKIM6258（MSM6258）和 OKIM6295（MSM6295）是 OKI 半导体生产的 ADPCM 语音合成芯片，
广泛用于街机和家用游戏机。两个芯片的 scope buffer 写入代码已存在，但使用旧版手动搜索
`m_voice_ChipID[]` 方式定位偏移，且存在构建配置遗漏和 buffer 写入 bug。

## 修改清单

### 1. okim6258.c（1 通道 ADPCM）

**文件**：`libvgm-modizer/emu/cores/okim6258.c`

#### 1a. 修正 include 路径（行 230）
```c
// 修改前
#include "../../../../../src/ModizerVoicesData.h"
// 修改后
#include "ModizerVoicesData.h"
```

#### 1b. 替换旧的手动搜索为 scope_register_chip（行 247-264）
```c
// 修改前：遍历 m_voice_ChipID[] 查找偏移
int m_voice_ofs=-1;
int m_total_channels=1;
for (int ii=0;ii<=SOUND_MAXVOICES_BUFFER_FX-m_total_channels;ii++) {
    if (m_voice_ChipID[ii]==m_voice_current_system) {
        m_voice_ofs=ii+(m_voice_current_systemSub?m_voice_current_systemPairedOfs:0);
        m_voice_current_total=m_total_channels;
        break;
    }
}

// 修改后：使用 scope 注册表
int m_total_channels=1;
ScopeChipSlot *scope_slot = scope_register_chip("OKIM6258", 0, m_total_channels, m_voice_current_samplerate);
int m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
```

buffer 写入代码（`m_voice_buff[m_voice_ofs+0][...]=LIMIT8((sample>>8))`）无需修改。

### 2. okim6295.c（4 通道 ADPCM）

**文件**：`libvgm-modizer/emu/cores/okim6295.c`

#### 2a. 修正 include 路径 + 移除 static 变量（行 64-68）
```c
// 修改前
#include "../../../../../src/ModizerVoicesData.h"
static int m_voice_ofs;
static int64_t smplIncr;

// 修改后
#include "ModizerVoicesData.h"
// static 变量移除，改为 update 函数内局部变量
```

#### 2b. 替换旧的手动搜索为 scope_register_chip（行 315-334）
```c
// 修改后
int m_total_channels=4;
ScopeChipSlot *scope_slot = scope_register_chip("OKIM6295", 0, m_total_channels, m_voice_current_samplerate);
int m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
int64_t smplIncr = ...;
```

#### 2c. 修复 generate_adpcm 内 buffer 写入 bug

**原始 bug**：buffer 写入用 `m_voice_ofs`（全局 static，对应 voice 0），
但指针更新用 `m_voice_ofs+i`（voice i），导致 voice 1-3 的 buffer 数据写入
到 voice 0 的位置，指针却更新到了 voice 1-3。

**修复方案**：给 `generate_adpcm()` 增加 `m_voice_ofs` 和 `smplIncr` 参数，
由 `okim6295_update()` 调用时传入 `m_voice_ofs + i`。

```c
// 修改后函数签名
static void generate_adpcm(okim6295_state *chip, okim_voice *voice,
    DEV_SMPL *buffer, UINT32 samples, int m_voice_ofs, int64_t smplIncr)

// 调用处
generate_adpcm(chip, &chip->voice[i], outputs[0], samples, m_voice_ofs + i, smplIncr);
// 删除旧的 m_voice_ofs++ 自增逻辑
```

#### 2d. 修复 vgm_last_note/vol 的索引
```c
// 修改前（所有 voice 都写到同一个索引）
vgm_last_note[m_voice_ofs] = ...;
// 修改后（每个 voice 写入正确偏移）
vgm_last_note[m_voice_ofs + i] = ...;
```

### 3. CMakeLists.txt

添加 3 个源文件到 scope_core_lib：
```cmake
${SCOPE_CORE_DIR}/okim6258.c
${SCOPE_CORE_DIR}/okim6295.c
${SCOPE_CORE_DIR}/okiadpcm.c
```

### 4. build_v15.sh

添加 ar d 移除预编译库冲突 .o：
```bash
ar d "$PREBUILT_LIB" okim6258.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" okim6295.c.o 2>/dev/null || true
ar d "$PREBUILT_LIB" okiadpcm.c.o 2>/dev/null || true
```

## 技术参数

| 参数 | OKIM6258 | OKIM6295 |
|------|----------|----------|
| scope_name | "OKIM6258" | "OKIM6295" |
| 通道数 | 1 | 4 |
| buffer 移位 | >>8 | >>8 |
| DC 偏移 | 无 | 无 |
| 输出范围 | -32768..32767 (10/12bit << 4) | -32768..32767 (signal*vol/2) |
| 写入模式 | 每通道独立指针 | 每通道独立指针（通过参数传递） |
| 设备 ID | DEVID_OKIM6258 (0x17) | DEVID_OKIM6295 (0x18) |
| 仿真文件 | okim6258.c | okim6295.c + okiadpcm.c |

## 前端（已存在，无需修改）

| 层级 | OKIM6258 | OKIM6295 |
|------|----------|----------|
| GetModizerVoiceCount | return 1 | return 4 |
| scopeName | "OKIM6258" | "OKIM6295" |
| BuildLevelMeters | s_okim6258_viz[2][1] | s_okim6295_viz[2][4] |
| Shadow 寄存器 | s_shadowOKIM6258[2][0x08] | s_shadowOKIM6295[2][0x10] |
| 芯片标签 | "OKIM6258" | "OKIM6295" |
