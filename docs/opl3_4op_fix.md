# OPL3 4-op 模式寄存器显示修复

**日期**: 2026-04-04
**版本**: v15

## 问题描述

OPL3 (YMF262) 在 4-op 模式下，示波器通道映射和寄存器显示不对应：

1. **示波器**: 只有低通道（ch0-2, ch9-11）显示波形，高通道（ch3-5, ch12-14）没有波形（正确，因为 `is_4op_attached`）
2. **寄存器显示**: 所有 18 个通道都分开显示，没有体现 4-op 配对关系（不正确）

### 4-op 配对规则

OPL3 的 4-op 模式通过寄存器 0x104（port 1, 0x04）控制：
- bit 0: ch0 + ch3 配对（第一个 4-op 通道）
- bit 1: ch1 + ch4 配对（第二个 4-op 通道）
- bit 2: ch2 + ch5 配对（第三个 4-op 通道）
- bit 3: ch9 + ch12 配对（第四个 4-op 通道）
- bit 4: ch10 + ch13 配对（第五个 4-op 通道）
- bit 5: ch11 + ch14 配对（第六个 4-op 通道）

当启用 4-op 模式时：
- 低通道（ch0-2, ch9-11）是主通道，输出声音
- 高通道（ch3-5, ch12-14）被附加，不单独输出

## 解决方案

### 修改位置
`vgm_window.cpp` 中 `DEVID_YMF262` 的寄存器显示部分

### 修改内容

1. **读取 4-op 使能寄存器**:
   ```cpp
   UINT8 fouren = s_shadowYMF262[c][1][0x04];
   ```

2. **检测通道是否是 4-op 配对的一部分**:
   ```cpp
   int pair_bit = (ch < 3) ? (p*3 + ch) : -1;
   bool is4op_low  = (pair_bit >= 0) && ((fouren >> pair_bit) & 1);
   bool is4op_high = (ch >= 3 && ch <= 5) && ((fouren >> (p*3 + ch-3)) & 1);
   ```

3. **跳过高通道**（在低通道中组合显示）:
   ```cpp
   if (is4op_high)
       continue;
   ```

4. **组合显示 4-op 通道**:
   - 标题显示为 "CH1+4 (4OP)" 表示 4-op 配对
   - OP 参数表显示 4 行（MOD, CAR, MOD2, CAR2）
   - 最后一行显示 "4OP" 标签和配对通道的 FB/CNT 值

### 修改前

```
CH1
├── Note | FB | CN | Pan
│   B6     0    0   LR
├── OP 参数表 (MOD, CAR)
CH4
├── Note | FB | CN | Pan
│   B6     0    0   LR
├── OP 参数表 (MOD, CAR)
```

### 修改后

```
CH1+4 (4OP)
├── Note | FB | CN | Pan
│   B6     0    0   LR
├── OP 参数表 (MOD, CAR, MOD2, CAR2)
│   ... (4 行)
├── 4OP | FB=0 | CNT=1
```

## 示波器行为

示波器核心代码 `adlibemu_opl_inc.c` 已经正确处理了 4-op 模式：
- `is_4op_attached` 标志正确设置
- 高通道在音频输出循环中被跳过（`continue;`）
- 只有低通道输出到 `m_voice_buff[]`

前端无需修改示波器映射，因为核心已经正确输出了。

## 测试验证

1. 使用启用 4-op 模式的 VGM 文件测试
2. 确认寄存器显示中：
   - 4-op 配对通道组合显示为 "CHx+y (4OP)"
   - 高通道不再单独显示
   - OP 参数表显示 4 个操作器
3. 确认示波器波形与寄存器显示一致

## 相关文档

- [OPL 系列示波器支持](opl_oscilloscope_support.md)
- [v15 示波器添加指南](v15_oscilloscope_guide.md)
