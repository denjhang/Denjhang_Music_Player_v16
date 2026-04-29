# 如何给芯片添加示波器功能

## 概述

YM2163 Piano v15 的示波器系统分为两层：
1. **仿真核心层**（libvgm-modizer）— 采集芯片的实时音频数据
2. **前端渲染层**（vgm_window.cpp）— 将音频数据可视化为波形

添加新芯片的示波器需要修改这两层。

---

## 第一层：仿真核心层（libvgm-modizer）

### 1.1 理解 ScopeChipSlot 注册表

**文件**：`libvgm-modizer/emu/cores/ModizerVoicesData.h` 和 `scope_chip_reg.c`

每个芯片实例在启动时必须向全局注册表注册自己：

```cpp
// 数据结构
typedef struct {
    const char *chip_name;      // e.g. "YM2151", "YM2608"
    int chip_inst;              // 实例索引（0, 1, ...）
    int num_channels;           // 总通道数
    int slot_base;              // 在全局 m_voice_buff[] 中的起始索引
    int active;                 // 是否活跃
    int num_groups;             // 通道组数量
    ScopeChGroup groups[SCOPE_MAX_GROUPS];  // 通道组描述
} ScopeChipSlot;

// 通道组（例如 "FM 1-6", "SSG 1-3"）
typedef struct {
    int type;           // SCOPE_CH_FM, SCOPE_CH_SSG, SCOPE_CH_ADPCM_A 等
    int count;          // 该组的通道数
    int start;          // 该组在芯片内的起始索引
    const char *label;  // 标签前缀，如 "FM", "SSG"
} ScopeChGroup;
```

### 1.2 在芯片仿真核心中注册

**示例**：YM2151 的注册（在 `libvgm-modizer/emu/cores/ym2151.c` 中）

```cpp
// 在芯片初始化时调用
ScopeChipSlot *slot = scope_register_chip("YM2151", chip_inst, 8, samplerate);
if (slot) {
    // 定义通道组
    slot->groups[0].type = SCOPE_CH_FM;
    slot->groups[0].count = 8;
    slot->groups[0].start = 0;
    slot->groups[0].label = "FM";
    slot->num_groups = 1;
}
```

**关键点**：
- `chip_name` 必须与前端的映射名称一致（见下文）
- `num_channels` 是该芯片的总通道数
- `slot_base` 由注册表自动计算（所有已注册芯片的通道总和）
- 通道组用于在前端显示时分类（FM、SSG、ADPCM-A 等）

### 1.3 在芯片更新函数中写入音频数据

每个通道的音频数据写入全局缓冲区 `m_voice_buff[]`：

```cpp
// 在芯片的每个采样周期调用
int voice_idx = slot->slot_base + local_channel_index;
if (voice_idx >= 0 && voice_idx < SOUND_MAXVOICES_BUFFER_FX) {
    // 写入采样数据（-128 到 127）
    m_voice_buff[voice_idx][m_voice_current_ptr[voice_idx]] = (signed char)sample;
    m_voice_current_ptr[voice_idx]++;
    if (m_voice_current_ptr[voice_idx] >= SOUND_BUFFER_SIZE) {
        m_voice_current_ptr[voice_idx] = 0;  // 环形缓冲区
    }
}
```

**关键点**：
- 所有通道共享同一个写入指针（`shared-ofs` 模式），消除通道间漂移
- 缓冲区是环形的，自动循环
- 采样值必须是 signed char（-128 到 127）

---

## 第二层：前端渲染层（vgm_window.cpp）

### 2.1 在 BuildLevelMeters() 中添加芯片映射

**文件**：`vgm_window.cpp`，函数 `BuildLevelMeters()`

这个函数为每个活跃的芯片创建 `LevelMeterEntry` 条目，包含：
- 芯片名称和标签
- 通道数量和类型
- 颜色、音量、音符等信息

**示例**：添加新芯片 "YMW258"

```cpp
case DEVID_YMW258: {
    // YMW258 有 16 个 FM 通道
    for (int ch = 0; ch < 16; ch++) {
        LevelMeterEntry m;
        m.chip_label = "YMW258";
        m.chip_abbrev = "YMW";
        m.label = "FM";  // 或根据通道类型设置
        m.color = IM_COL32(0, 200, 0, 255);  // 绿色（FM 芯片）
        m.group_start = (ch == 0) ? 1 : 0;
        m.keyon = /* 从寄存器读取 */;
        m.level = /* 计算音量 */;
        m.note = /* 计算音符 */;
        meters.push_back(m);
    }
    break;
}
```

### 2.2 在 RenderScopeArea() 中添加芯片名称映射

**文件**：`vgm_window.cpp`，函数 `RenderScopeArea()`，约第 4254 行

这里将设备类型 ID 映射到注册表中的芯片名称：

```cpp
switch (chipType) {
    case DEVID_YM2151:  scopeName = "YM2151";  break;
    case DEVID_YM2203:  scopeName = "YM2203";  break;
    case DEVID_YM2608:  scopeName = "YM2608";  break;
    case DEVID_YM2610:  scopeName = "YM2610";  break;
    case DEVID_YM2612:  scopeName = "YM2612";  break;
    case DEVID_AY8910:  scopeName = "SSG";     break;
    case DEVID_SN76496: scopeName = "SN76489"; break;
    // 添加新芯片
    case DEVID_YMW258:  scopeName = "YMW258";  break;
    default: break;
}
```

**关键点**：
- `scopeName` 必须与仿真核心中的 `chip_name` 完全一致
- 通过 `scope_find_slot(scopeName, chipInst)` 查找该芯片的注册槽
- 返回的 `slot->slot_base` 是该芯片在全局缓冲区中的起始索引

### 2.3 处理特殊通道映射（如 SSG、ADPCM）

某些芯片（如 YM2608）包含多个子芯片（FM + SSG + ADPCM-A + ADPCM-B）。
这些子芯片可能单独注册，需要特殊处理：

```cpp
// YM2608 的 SSG 通道（本地索引 6-8）映射到单独的 "SSG" 注册槽
if (chipType == DEVID_YM2608 && localCh >= 6 && localCh < 9) {
    isSSGCh = true;
    ssgLocalCh = localCh - 6;
    ScopeChipSlot *ssgSlot = scope_find_slot("SSG", chipInst);
    if (ssgSlot) {
        vc = ssgSlot->slot_base + ssgLocalCh;  // 重新映射到 SSG 的缓冲区
    }
}
```

---

## 第三层：CMakeLists.txt 编译配置

### 3.1 在 scope_core_lib 中启用芯片仿真

**文件**：`CMakeLists.txt`，约第 100-130 行

如果新芯片的仿真核心需要修改，添加到 `scope_core_lib` 的源文件列表：

```cmake
add_library(scope_core_lib STATIC
    ${SCOPE_CORE_DIR}/fmopn.c
    ${SCOPE_CORE_DIR}/ym2151.c
    ${SCOPE_CORE_DIR}/scope_chip_reg.c
    ${SCOPE_CORE_DIR}/emu2149.c
    ${SCOPE_CORE_DIR}/ay8910.c
    ${SCOPE_CORE_DIR}/sn76496.c
    # 添加新芯片的核心文件
    ${SCOPE_CORE_DIR}/ymw258.c
)
```

### 3.2 在编译定义中启用芯片

```cmake
target_compile_definitions(scope_core_lib PRIVATE
    SNDDEV_YMW258  # 启用 YMW258 仿真
    # ... 其他定义
)
```

---

## 完整工作流示例：添加 YMW258 示波器

### 步骤 1：修改仿真核心

**文件**：`libvgm-modizer/emu/cores/ymw258.c`（或相关文件）

在芯片初始化函数中：
```cpp
void YMW258_Init(...) {
    // ... 其他初始化代码
    ScopeChipSlot *slot = scope_register_chip("YMW258", chip_inst, 16, samplerate);
    if (slot) {
        slot->groups[0].type = SCOPE_CH_FM;
        slot->groups[0].count = 16;
        slot->groups[0].start = 0;
        slot->groups[0].label = "FM";
        slot->num_groups = 1;
    }
}
```

在每个采样周期的更新函数中：
```cpp
void YMW258_Update(...) {
    // ... 生成采样数据
    for (int ch = 0; ch < 16; ch++) {
        int voice_idx = slot->slot_base + ch;
        m_voice_buff[voice_idx][m_voice_current_ptr[voice_idx]] = (signed char)sample[ch];
        m_voice_current_ptr[voice_idx]++;
        if (m_voice_current_ptr[voice_idx] >= SOUND_BUFFER_SIZE) {
            m_voice_current_ptr[voice_idx] = 0;
        }
    }
}
```

### 步骤 2：修改前端

**文件**：`vgm_window.cpp`

在 `BuildLevelMeters()` 中添加：
```cpp
case DEVID_YMW258: {
    for (int ch = 0; ch < 16; ch++) {
        LevelMeterEntry m;
        m.chip_label = "YMW258";
        m.chip_abbrev = "YMW";
        m.label = "FM";
        m.color = IM_COL32(0, 200, 0, 255);
        m.group_start = (ch == 0) ? 1 : 0;
        // ... 填充其他字段
        meters.push_back(m);
    }
    break;
}
```

在 `RenderScopeArea()` 中添加：
```cpp
case DEVID_YMW258: scopeName = "YMW258"; break;
```

### 步骤 3：更新编译配置

**文件**：`CMakeLists.txt`

```cmake
add_library(scope_core_lib STATIC
    ${SCOPE_CORE_DIR}/fmopn.c
    ${SCOPE_CORE_DIR}/ym2151.c
    ${SCOPE_CORE_DIR}/scope_chip_reg.c
    ${SCOPE_CORE_DIR}/emu2149.c
    ${SCOPE_CORE_DIR}/ay8910.c
    ${SCOPE_CORE_DIR}/sn76496.c
    ${SCOPE_CORE_DIR}/ymw258.c  # 新增
)

target_compile_definitions(scope_core_lib PRIVATE
    # ... 其他定义
    SNDDEV_YMW258  # 新增
)
```

### 步骤 4：编译和测试

```bash
cd YM2163_Piano_v15_Release
bash build_v15.sh
```

加载包含 YMW258 的 VGM 文件，示波器应该显示 16 个绿色的 FM 通道波形。

---

## 关键设计原则

### 1. **通道隔离**
- 每个通道有独立的环形缓冲区
- 所有通道共享同一个写入指针（`shared-ofs` 模式）
- 这样可以消除通道间的时间漂移

### 2. **芯片名称一致性**
- 仿真核心中的 `chip_name` 必须与前端的 `scopeName` 完全一致
- 建议使用官方芯片型号（如 "YM2151"、"YMW258"）

### 3. **通道组分类**
- 使用 `ScopeChGroup` 将通道分组（FM、SSG、ADPCM-A 等）
- 前端根据通道类型分配颜色和标签

### 4. **采样率处理**
- 仿真核心采样率可能与显示采样率（44.1kHz）不同
- 使用固定点运算进行精确的采样率转换

### 5. **波形稳定化**
- 使用互相关触发（cross-correlation）对齐波形相位
- 确保示波器显示稳定的波形而不是抖动

---

## 常见问题

### Q1：为什么我的芯片示波器不显示？
**A**：检查以下几点：
1. 芯片是否在 `BuildLevelMeters()` 中添加了条目？
2. `scopeName` 是否与仿真核心的 `chip_name` 一致？
3. 仿真核心是否调用了 `scope_register_chip()`？
4. 采样数据是否正确写入 `m_voice_buff[]`？

### Q2：示波器显示但波形很弱或很强？
**A**：调整采样数据的缩放因子。在仿真核心中：
```cpp
// 如果波形太弱，乘以放大因子
int sample = (raw_sample * 2) & 0xFF;
m_voice_buff[voice_idx][...] = (signed char)sample;
```

### Q3：如何处理多实例芯片（如 4 片 YM2163）？
**A**：每个实例有不同的 `chip_inst` 值：
```cpp
scope_register_chip("YM2163", 0, 4, samplerate);  // 第一片
scope_register_chip("YM2163", 1, 4, samplerate);  // 第二片
scope_register_chip("YM2163", 2, 4, samplerate);  // 第三片
scope_register_chip("YM2163", 3, 4, samplerate);  // 第四片
```

前端会自动为每个实例分配不同的 `slot_base`。

---

## 参考资源

- `libvgm-modizer/emu/cores/ModizerVoicesData.h` — 数据结构定义
- `libvgm-modizer/emu/cores/scope_chip_reg.c` — 注册表实现
- `libvgm-modizer/emu/cores/fmopn.c` — YM2151/YM2203/YM2608/YM2610/YM2612 的示例实现
- `vgm_window.cpp` — 前端渲染逻辑
