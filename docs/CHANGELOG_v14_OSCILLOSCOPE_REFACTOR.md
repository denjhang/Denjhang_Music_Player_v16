# YM2163 Piano v14 - 示波器区域重构更新

## 更新日期
2026-04-04

## 概述
本次更新对示波器显示系统进行了全面重构，实现了示波器背景容器与示波器本身的完全解耦，提供了更灵活的多行示波器显示和独立的尺寸控制。

## 主要改进

### 1. 示波器背景与示波器本身解耦
- **问题**：之前分割栏调整会影响示波器本身的大小，导致示波器尺寸和背景容器高度耦合
- **解决**：
  - 示波器背景是一个独立的滚动容器，由分割栏控制其高度
  - 示波器本身的宽度和高度由左侧边栏的滑块独立控制
  - 分割栏只调整背景容器的显示空间，不影响示波器尺寸

### 2. 音量条和示波器模式完全分离
- **问题**：之前两种模式会同时占用空间，导致布局混乱
- **解决**：
  - 在main.cpp中根据GetScopeMode()动态选择显示哪个区域
  - 示波器模式：只显示VGM_ScopeBackground和分割栏
  - 音量条模式：只显示VGM_LevelMeterArea
  - 两种模式完全独立，不会相互干扰

### 3. 动态行数计算
- **问题**：之前行数计算不够灵活，未使用的行仍占用空间
- **解决**：
  - 根据实际通道数和宽度动态计算需要的行数：`numRows = (n + chPerRow - 1) / chPerRow`
  - 每一行都预留芯片标签高度（kChipLabelH）
  - 未使用的行完全隐藏，不占用空间
  - 所有行共用左侧的宽度参数（s_scopeWidth）

### 3.1 按芯片组动态换行（新增）
- **改进**：实现了智能的芯片组换行算法
- **规则**：
  - 按芯片组（而非单个通道）来分配行
  - 如果空间足够，允许多个芯片在同一行显示
  - 如果某个芯片的通道数超过剩余空间，整个芯片组移到下一行
  - 确保同一芯片的所有通道始终在同一行显示
- **实现**：
  - 构建ChipGroup结构体，记录每个芯片组的起始索引和通道数
  - 使用贪心算法将芯片组分配到行中
  - 每行的通道数不超过maxChPerRow（根据可用宽度计算）

### 4. 左侧边栏控制
- **新增功能**：
  - "Scope Width"滑块：控制每个示波器小窗口的宽度（40-150px）
  - "Scope Height"滑块：控制每个示波器小窗口的高度（30-150px）
  - 所有示波器小窗口使用相同的宽度和高度设定

### 5. 布局高度计算优化
- **问题**：之前topSectionHeight计算不正确，导致分割栏下方有大片空白
- **解决**：
  - 根据模式动态计算topSectionHeight
  - 示波器模式：`topSectionHeight = pianoHeight + s_scopeBackgroundHeight + 4`
  - 音量条模式：`topSectionHeight = pianoHeight + s_levelMeterHeight`
  - VGM播放器（文件浏览器和日志）能正确占用剩余空间

## 技术细节

### 文件修改

#### vgm_window.h
- 添加GetScopeMode()函数声明
- **新增**：添加GetScopeBackgroundHeight()和SetScopeBackgroundHeight()函数声明
- **新增**：添加SavePlayerState()函数声明用于配置持久化

#### vgm_window.cpp
- 添加s_scopeHeight静态变量（默认60.0f）
- **新增**：添加s_scopeBackgroundHeight静态变量（默认200.0f）
- 在RenderControls()中添加"Scope Height"滑块
- 重写RenderScopeArea()：
  - 创建独立的滚动背景容器
  - 使用s_scopeHeight控制每行示波器高度
  - **新增**：按芯片组动态换行算法
    - 构建ChipGroup结构体数组，记录每个芯片组的信息
    - 使用贪心算法将芯片组分配到行中
    - 计算每行的最大通道数（maxChPerRow）
    - 如果芯片组无法放入当前行，整个芯片组移到下一行
  - 动态计算行数和总高度
  - 背景容器始终有垂直滚动条
- 在SaveConfig()中保存ScopeBackgroundHeight
- 在LoadConfig()中加载ScopeBackgroundHeight（范围验证：100-600）
- **新增**：添加GetScopeBackgroundHeight()和SetScopeBackgroundHeight()函数实现
- **新增**：将SavePlayerState()从static改为public，用于外部调用
- 添加GetScopeMode()函数实现

#### main.cpp
- 修改libvgm标签页布局：
  - 根据GetScopeMode()动态选择显示区域
  - 动态计算topSectionHeight
  - 分割栏只在示波器模式下显示
  - **新增**：使用GetScopeBackgroundHeight()和SetScopeBackgroundHeight()函数管理分割栏位置
  - **新增**：拖动分割栏时调用SavePlayerState()保存配置

### 关键常量
- kChipLabelH = 18.0f：芯片标签高度
- kBarGap = 2.0f：示波器间隙
- kGroupGap = 10.0f：芯片组间隙
- kGroupPad = 3.0f：组框内边距
- kMaxChPerRow = 11：每行最大通道数

### 芯片组换行算法详解

#### 数据结构
```cpp
struct ChipGroup {
    int startIdx;      // 芯片组在scopeMeters中的起始索引
    int endIdx;        // 芯片组的结束索引
    int channelCount;  // 该芯片组的通道数
};

struct Row {
    std::vector<int> groupIndices;  // 该行包含的芯片组索引
    int totalChannels;              // 该行的总通道数
};
```

#### 算法流程
1. **构建芯片组**：遍历scopeMeters，按group_start标记分组
2. **计算最大容量**：根据availW和s_scopeWidth计算每行最多能放多少通道
3. **分配芯片组到行**：
   - 对每个芯片组，检查是否能放入当前行
   - 如果当前行的总通道数 + 新芯片组的通道数 <= maxChPerRow，则加入当前行
   - 否则，创建新行并将该芯片组放入
4. **渲染**：按行渲染，每行内的芯片组按顺序显示

#### 优势
- **保持芯片完整性**：同一芯片的所有通道始终在同一行
- **空间利用率高**：允许多个芯片在同一行（如果空间足够）
- **自适应布局**：根据可用宽度自动调整每行的芯片数量
- **清晰的视觉分组**：每个芯片组有独立的边框

## 使用说明

### 调整示波器尺寸
1. 在左侧边栏找到"Scope Width"和"Scope Height"滑块
2. 拖动滑块调整示波器小窗口的宽度和高度
3. 所有示波器小窗口会同时更新

### 调整示波器显示空间
1. 在示波器区域下方找到分割栏（灰色横条）
2. 拖动分割栏上下移动，调整示波器背景容器的高度
3. 背景容器会自动显示滚动条以容纳多行示波器

### 切换模式
1. 点击左侧边栏的"Scope"按钮切换示波器/音量条模式
2. 按钮变绿表示示波器模式，灰色表示音量条模式

## 配置保存
- 示波器宽度、高度、模式等设置会自动保存到vgm_player.ini
- **新增**：分割栏位置（示波器背景容器高度）也会自动保存
- 下次启动时会自动恢复之前的设置
- 拖动分割栏时会实时保存位置

## 编译信息
- 编译成功，无错误
- 仅有几个未使用变量的警告（不影响功能）

## 向后兼容性
- 本次更新不影响其他功能
- 旧的配置文件会自动升级，新增的ScopeHeight和ScopeBackgroundHeight使用默认值

## 持久化配置详解

### 配置文件格式（vgm_player.ini）
```ini
[PlayerState]
CurrentPath=...
LoopCount=2
MasterVolume=1.0000
ShowScope=1
ScopeWidth=90.0
ScopeHeight=60.0
ScopeBackgroundHeight=200.0
```

### 持久化流程
1. **保存**：当用户调整分割栏位置时，SetScopeBackgroundHeight()更新内存值，然后SavePlayerState()将其写入vgm_player.ini
2. **加载**：应用启动时，LoadConfig()从vgm_player.ini读取ScopeBackgroundHeight，并进行范围验证（100-600）
3. **验证**：所有值都经过范围检查，确保UI布局合理

### 相关函数
- `GetScopeBackgroundHeight()`：获取当前分割栏位置
- `SetScopeBackgroundHeight(float height)`：设置分割栏位置（自动范围限制）
- `SavePlayerState()`：保存所有播放器状态和UI设置到配置文件
