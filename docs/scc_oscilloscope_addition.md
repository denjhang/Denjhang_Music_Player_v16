# SCC (K051649) 示波器添加记录

> 本文档记录 YM2163 Piano GUI v15 中为 Konami SCC (K051649) 芯片添加示波器功能的完整过程。
> 开发时间：2026-04-04，AI 模型：GLM-5-Turbo，一次通过。

---

## 一、芯片背景

Konami SCC (K051649) 是 Konami 为 MSX 开发的 5 通道波形合成芯片，每个通道有独立的 32 字节波形 RAM（8 位有符号数据），可自定义波形。变体 K052539 (SCC+) 通道 5 不再与通道 4 共享波形 RAM。

### 芯片参数

| 参数 | 值 |
|------|-----|
| 通道数 | 5 |
| 波形 RAM | 每通道 32 字节，8 位有符号 |
| 音量范围 | 0-15（4 位） |
| 频率范围 | 0-0xFFF（12 位），0-8 时通道停止 |
| 输出类型 | 有符号（不需要 DC 偏移） |
| 典型时钟 | 1,789,772 Hz |

---

## 二、修改内容

### 1. 仿真核心：k051649.c

**文件**：`libvgm-modizer/emu/cores/k051649.c`

#### 1.1 替换旧式注册为新式 ScopeChipSlot

旧代码使用 Modizer 的原始 `m_voice_ChipID` 线性搜索：

```c
// 旧代码（删除）
int m_voice_ofs=-1;
int m_total_channels=5;
for (int ii=0;ii<=SOUND_MAXVOICES_BUFFER_FX-m_total_channels;ii++) {
    if (m_voice_ChipID[ii]==m_voice_current_system) {
        m_voice_ofs=ii+...;
        break;
    }
}
```

新代码使用 v14 的 ScopeChipSlot 注册表：

```c
// 新代码
ScopeChipSlot *scope_slot = scope_register_chip("SCC", 0, 5, m_voice_current_samplerate);
if (scope_slot && scope_slot->num_groups == 0) {
    scope_slot->groups[0] = (ScopeChGroup){SCOPE_CH_DAC, 5, 0, "S"};
    scope_slot->num_groups = 1;
}
int m_voice_ofs = scope_slot ? scope_slot->slot_base : -1;
```

#### 1.2 重构音频生成循环 + shared-ofs 模式

**关键架构决策**：SCC 的原始代码是「外层通道 → 内层采样」的嵌套循环结构：

```c
// 旧结构
for (j = 0; j < 5; j++) {        // 外层：通道
    for (i = 0; i < samples; i++) { // 内层：采样
        voice[j].counter += step;
        ...
    }
}
```

这种结构无法实现 shared-ofs 模式的 scope buffer 写入（因为每个通道的采样是独立推进的，无法在同一次内循环中获取所有通道的当前采样值）。

**重构为「外层采样 → 内层通道」**：

```c
// 新结构
for (i = 0; i < samples; i++) {    // 外层：采样
    DEV_SMPL ch_out[5];
    for (j = 0; j < 5; j++) {      // 内层：通道
        voice[j].counter += steps[j];
        if (voice[j].key) {
            ch_out[j] = voice[j].waveram[...] * voice[j].volume >> 4;
            mix += ch_out[j];
        }
    }
    // shared-ofs scope buffer write（在采样循环内部）
    if (m_voice_ofs >= 0) {
        int64_t ofs_start = m_voice_current_ptr[m_voice_ofs + 0];
        int64_t ofs_end = ofs_start + smplIncr;
        for (;;) {
            for (int jj = 0; jj < 5; jj++) {
                m_voice_buff[m_voice_ofs + jj][...] = LIMIT8(ch_out[jj] >> 2);
            }
            ofs_start += 1 << MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
            if (ofs_start >= ofs_end) break;
        }
        for (int jj = 0; jj < 5; jj++)
            m_voice_current_ptr[m_voice_ofs + jj] = ofs_end;
    }
}
```

**为什么必须在内层循环中写入**：scope buffer 的波形必须和实际音频输出严格同步。在循环外写入时，`voice[jj].counter` 未推进（落后一帧）或已推进完（超前一帧），都会导致波形不匹配。

#### 1.3 Buffer 缩放参数

| 步骤 | 值 | 说明 |
|------|-----|------|
| 原始输出 | `waveram[offs] * volume` | [-128,127] * [0,15] = [-1920, 1905] |
| >>4 缩放 | [-120, 119] | 芯片 11 位数字输出 |
| scope >>2 | [-30, 29] | LIMIT8 范围内，波形饱满 |

#### 1.4 修复 include 路径

```c
// 旧路径（不存在于 v15 项目结构中）
#include "../../../../../src/ModizerVoicesData.h"

// 修复为同目录引用（通过 CMake include_directories 解析）
#include "ModizerVoicesData.h"
```

旧路径在预编译库编译时可能存在（Modizer 原始项目结构），但在 v15 项目中 `src/` 目录不存在。旧代码能编译通过是因为旧式 YOYOFR 代码只用了 `m_voice_ChipID` 等全局变量声明（即使 header 未找到，这些变量可能在其他地方声明），而新代码需要 `ScopeChipSlot` 等类型定义。

### 2. 前端：vgm_window.cpp

**GetModizerVoiceCount** 添加 SCC：
```cpp
case DEVID_K051649: return 5;   // 5 Waveform
```

**RenderScopeArea** 添加 scopeName 映射：
```cpp
case DEVID_K051649: scopeName = "SCC";  break;
```

注意：BuildLevelMeters 中 SCC 的条目、芯片标签、ChVizState 等在 v14 已经实现，无需修改。

### 3. 编译配置：CMakeLists.txt

```cmake
add_library(scope_core_lib STATIC
    ...
    ${SCOPE_CORE_DIR}/sn76496.c
    ${SCOPE_CORE_DIR}/k051649.c    # 新增
)
```

### 4. 预编译库处理

```bash
ar d libvgm-emu.a k051649.c.o
```

---

## 三、与 Claude Opus 4.6 / Haiku 4.5 的对比

Claude Opus 4.6 和 Haiku 4.5 在同一任务上多轮对话消耗数十美金 token 后仍无法完成，具体表现：
- 修改的代码编译通过但示波器窗口无法显示任何波形
- 多轮修改均无法定位问题根因
- 最终放弃

**GLM-5-Turbo 一次通过的原因**：
1. 先研究现有芯片（YM2612/2151/SN76489/AY8910）的完整实现模式，理解 shared-ofs 架构
2. 正确识别 k051649.c 中旧式 YOYOFR 代码与新式 ScopeChipSlot 的差异
3. 发现并修复 include 路径问题（`../../../../../src/` → 同目录引用）
4. 重构音频生成循环结构（通道外→采样内），使 scope buffer 写入能在采样循环内部同步执行
5. 严格遵循三步模板：仿真核心 → 前端映射 → 编译配置，不遗漏任何一步

---

## 四、经验总结

### 给新芯片添加示波器的标准三步模板

1. **仿真核心**：`scope_register_chip()` + shared-ofs buffer 写入（必须在采样循环内部）
2. **前端**：`GetModizerVoiceCount()` + `RenderScopeArea()` scopeName 映射
3. **编译**：scope_core_lib 添加 .c + `ar d` 预编译库

### shared-ofs 的正确位置

scope buffer 写入**必须**在音频采样的内层循环中，读取当前采样周期已计算好的通道输出值。写入在循环外会导致波形与音频不同步。

### include 路径

使用同目录 `#include "ModizerVoicesData.h"` 而非跨目录绝对路径，依赖 CMake 的 `target_include_directories` 解析。

---

*文档生成时间：2026-04-04*
*AI 模型：GLM-5-Turbo*
