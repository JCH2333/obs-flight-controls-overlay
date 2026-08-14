# OBS Flight Controls Overlay

中文名：**OBS 飞行外设输入叠加层**

将飞行摇杆、油门、脚舵和手轮的 DirectInput 模拟轴，实时绘制为简约的 OBS 画面叠加层。插件在 OBS 的来源列表中显示为 **外设轴映射**；每个来源独立保存设备、轴、校准与外观设置，可在同一场景同时展示多套飞行外设。

> 当前开发目标为 Windows x64 与 OBS Studio 32.2.1 x64。只支持 Windows “游戏控制器”中以 DirectInput 模拟轴公开的设备。

## 功能

- 四种显示模式：
  - **摇杆**：正方形 XY 平面与实时圆点。
  - **油门**：一至四条独立的线性轴。
  - **脚舵**：下方居中的方向舵轴，上方两条独立刹车轴。
  - **手轮**：一条居中的横向转向轴。
- 每个轴旁均提供 **转动检测**：在 3 秒内移动目标控制轴，插件会自动完成绑定。
- 摇杆模式支持 X/Y 单独反向、中心校准、4 秒自动量程、死区与平滑。
- 每个来源独立绑定设备，不会控制 Windows 鼠标、发送按键或移动其他 OBS 来源。
- 设备断开时保留叠加区域并显示“离线”；重连到同一设备后自动恢复。
- 可设置显示名称、强调色、整体不透明度、圆点尺寸和安全边距。
- 支持空闲自动隐藏：开始操作后立即显示，静止指定时间后隐藏。
- 原生 C++ OBS 视频来源，不依赖浏览器来源、独立窗口或 Dock。

## 安装

后续发布包会包含下列目录结构。请先关闭 OBS，再将文件复制到 OBS 安装目录：

```text
obs-flight-axis-overlay.dll
data/
  obs-plugins/
    obs-flight-axis-overlay/
      locale/
        en-US.ini
        zh-CN.ini
```

复制规则：

```text
obs-flight-axis-overlay.dll
  -> <OBS 安装目录>\obs-plugins\64bit\

data\obs-plugins\obs-flight-axis-overlay\
  -> <OBS 安装目录>\data\obs-plugins\obs-flight-axis-overlay\
```

完成后启动 OBS。不要在 OBS 运行期间替换 DLL。

## 使用方法

### 1. 添加来源

1. 在目标场景的“来源”列表中点击添加。
2. 选择 **外设轴映射**。
3. 为该来源命名并打开属性。
4. 在“设备与轴”中选择要显示的外设。

只有 DirectInput 实际公开模拟轴的设备会出现在列表中。设备暂时断开时，已保存的绑定仍会保留为“已绑定设备（离线）”。

### 2. 选择显示模式

在“显示模式”中选择与外设相符的布局：

| 模式 | 轴配置 | 画面表现 |
| --- | --- | --- |
| 摇杆（XY 平面） | X 轴、Y 轴 | 正方形平面中的圆点 |
| 油门（多轴） | 轴 1 至轴 4，可按需留空 | 多条独立线性位置条 |
| 脚舵（三轴） | 轴 1 为方向舵，轴 2/3 为左右刹车 | 下方横向方向舵，上方两条竖向刹车 |
| 手轮（单轴） | 轴 1 | 横向、以中心为零点的位置条 |

方向舵与手轮的中点为零，轴值会向左右两侧展开。油门和刹车则以线性位置显示。

### 3. 用“转动检测”绑定轴

推荐通过检测来绑定，避免不同设备的轴命名或排列方式造成误选：

1. 在目标轴右侧点击 **转动检测**。
2. 在 3 秒内只移动要绑定的一个控制轴，最好推过较大的行程。
3. 插件会选择变化最明显的轴并写入当前选择项。
4. 如果显示“未检测到明显轴变化”，重新点击检测并加大该轴的运动幅度。

摇杆模式的 X 轴和 Y 轴可分别检测。油门、脚舵和手轮模式中，每个线性轴也都有独立的检测按钮。

### 4. 校准摇杆

“校准与手感”只在摇杆模式显示：

1. 让摇杆停在自然中位，点击 **设为当前中心**。
2. 点击 **自动量程（4 秒）**。
3. 在 4 秒内将摇杆完整推到 X/Y 两个方向的端点。
4. 如校准结果不理想，点击 **重置校准** 恢复标准 DirectInput 范围。

默认死区为 3%，默认平滑为 0 毫秒。若画面方向与操作习惯相反，可分别启用 **反向 X 轴** 或 **反向 Y 轴**。

### 5. 调整画面与自动隐藏

在“显示”中可修改标签、强调色、整体不透明度、圆点大小和安全边距。“整体不透明度”会同时作用于标签、边框、轨道、状态点和轴位置指示；`1.00` 为完全不透明，`0.00` 为完全隐藏。建议继续使用 OBS 自带的变换功能调整来源的位置、缩放和图层顺序。

启用 **空闲时自动隐藏** 后：

- “启动时隐藏”决定该来源在 OBS 启动后是否先保持隐藏。
- “空闲多少秒后隐藏”默认 15 秒，可设为 1 至 300 秒。
- 任一已绑定轴出现约 1% 以上变化时，来源会立即重新显示。

## 兼容性与限制

- 仅支持 **64 位 Windows**。
- 当前目标版本为 **OBS Studio 32.2.1 x64**。
- 输入层使用 DirectInput 8，不额外解析 Raw HID 报告，也不使用 XInput。
- 如果设备在 Windows 的“游戏控制器”工具中未作为标准 DirectInput 模拟轴暴露，插件不会尝试猜测厂商私有协议。
- V1 仅用于直播与录制画面的可视化，不会执行输入注入或系统控制。

## 从源码构建

### 依赖

- Visual Studio 2022 或 Visual Studio 2022 Build Tools，包含 MSVC x64 C++ 工具链与 Windows SDK。
- CMake 3.28 或更高版本。
- 与目标 OBS 匹配的完整 OBS 开发 SDK，或用于本机测试的 OBS 32.2.1 源码与运行时安装。

使用完整 OBS SDK：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SDK_PREFIX="C:\SDK\obs"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

只有 OBS 运行时安装、需要进行本机测试时，可启用运行时导入库回退。源码版本必须与运行时 OBS 匹配：

```powershell
cmake -S . -B build-runtime -G "Visual Studio 17 2022" -A x64 `
  -DOBS_USE_RUNTIME_IMPORT_LIB=ON `
  -DOBS_SOURCE_ROOT="C:\src\obs-studio-32.2.1" `
  -DOBS_RUNTIME_ROOT="C:\Program Files\obs-studio"
cmake --build build-runtime --config Release
ctest --test-dir build-runtime -C Release --output-on-failure
```

运行时回退仅适用于本机测试。正式发布构建应使用完整 OBS 开发 SDK。

## 本机部署

配置时指定本机 OBS 的安装目录：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SDK_PREFIX="C:\SDK\obs" `
  -DOBS_LOCAL_INSTALL_ROOT="C:\Program Files\obs-studio"
cmake --build build --config Release --target deploy-local
```

`deploy-local` 会在发现 OBS 正在运行时停止执行；它只复制本插件的 DLL 与数据目录，不会删除或覆盖其他插件文件。

## 验证范围

已覆盖的自动化测试包括 DirectInput 范围归一化、端点与中心映射、反向、死区重标定、平滑、异常校准范围及单侧量程。实际设备还应在目标 OBS 版本中验证轴检测、断线重连、自动隐藏和多来源并行显示。

## 开发状态

此项目当前处于 V1 开发阶段。发布前会提供版本号、发行说明和适用于 OBS 的构建产物。
