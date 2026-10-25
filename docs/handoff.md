# PadDrawBoard 当前交接

## 2026-09-13：最新本地快照

本快照包含上一轮冻结恢复、Android 可靠性/资源释放、Windows 混合显卡视频管线、MFT 引用生命周期、协议遥测和测试补丁。新增或更新的重点组件包括：Android 有界发送队列与四帧视频入口 FIFO、解码器等待 IDR/有限重建状态机、Windows 帧 pacing 与编码器待处理输入门控、adapter-scoped H.264 MFT 选择、输出 sample 复用和异步 MFT 关闭、视频内存探针，以及对应的原生/Android/延迟分析测试。

本地验证结果：

- `python -m unittest discover -s tools/latency -p "test_*.py" -v`：7/7 通过。
- MinGW 原生构建（`PDB_BUILD_TESTS=ON`）：成功；CTest：7/7 通过。
- Android `testDebugUnitTest assembleDebug assembleDebugAndroidTest`：构建成功。
- `git diff --check`：通过；尚未把设备安装、真实显示链路或一小时 soak 结果外推为本地自动化验证结论。

下一位接手者先检查 `git status --short --branch` 和最新提交，再观察 GitHub Actions。`cert_backup/` 仅是本机证书备份，已加入忽略规则，不应上传；`build/`、`out/`、`artifacts/` 仍是本地生成目录。

## 2026-09-12 16:39：冻结回归修正并实机恢复

查明日常路径仍在运行 8 月旧版 Windows 程序并发生崩溃；已备份并更新 `D:\PadDrawBoard`。同时纠正上一轮 Android 单帧入口与过度重建导致的恢复循环，改为四帧有界 FIFO、等待输入槽、参考帧丢失仅等待 IDR。最终 APK 已安装，Android 33 项单测通过；一分钟七次实机采样持续产帧、丢帧 0、重置计数不再增长，私有内存约 112–114 MiB。详见 [冻结复查](freeze-recovery-20260912.md)。此前安装受限状态和单帧入口方案已被本节取代。

## 2026-09-12：Android 可靠性与遥测补丁

已分点修复视频/发送入口队列、参考帧恢复、初始化失败资源释放，以及丢帧统计和端到端延迟误判。Android 单测 32 项、延迟工具 7 项通过；APK 与设备测试 APK 已构建。HyperOS 拒绝 USB 安装，设备失败路径测试与新版本实机复测尚未完成，正等待用户确认安装提示。详细实现、设备安装状态和继续命令见 [本轮修复记录](android-reliability-fixes.md)。以下较早记录按日期保留，当前状态以本节和对应报告为准。

## 2026-09-06：内存增长根因已修复

下文历史记录中“NVIDIA MFT 自身泄漏”的归因已被新证据纠正：本机 MinGW 的 `ComPtr::Attach()` 额外 AddRef，导致输出 sample、事件集合和 CodecAPI 引用无法释放。已改为明确接管引用并补充异步 MFT 关闭，保留 NVIDIA 硬件编码。120 秒反复 IDR 压力测试通过，便携版已连接平板验证。完整证据、剩余门槛和后续优化见 [内存分析](memory-analysis.md)。

以下保留 2026-08-15 的历史交接，若结论冲突以上述新记录为准。

> 本节是当前事实来源。下方“历史交接”保留早期基线记录，其中部分状态已被本轮实机修复取代。

## 当前结论

- Windows 与 Android 客户端已经在一台 Xiaomi Pad 7 类设备上完成实机联调。
- 控制、视频和输入三个 ADB reverse 通道均已建立并保持 `Established`；端口仍为 48100、48101、48102。
- 笔输入、手指拖动/点击、屏幕旋转后的坐标映射、视频显示和熄屏恢复均完成针对性修复。
- Windows 托盘界面已精简并中文化；Windows 与 Android 均已接入新的 PadDrawBoard 应用图标。
- 本轮最后验证：Windows CTest 7/7 通过，Android `testDebugUnitTest` 与 `assembleDebug` 通过，Debug APK 已覆盖安装并成功启动。
- 接手会话复验（2026-08-15 凌晨）：Windows（MinGW）构建通过、CTest 7/7；Android `testDebugUnitTest` 与 `assembleDebug` 通过。
- AMD 780M 核显 + NVIDIA RTX 4060 独显机器上的 15-25 秒永久冻结已经修复：实机连续运行超过两分钟，视频约 62 fps、无断流、无队列增长；完整根因与验证记录见下文专节。
- 仍有一个独立的 NVIDIA Media Foundation MFT 内存增长问题：编码开启时私有内存约增加 1.5 MB/s；捕获单独运行稳定，20/80 Mbps 斜率相同，应用侧遥测、协议和 sample 所有权修复均不改变斜率。正式一小时稳定性验收前必须解决或通过进程隔离/其他编码后端规避。

架构、构建前置条件和完整验收方法不在此重复，分别参见：

- `docs/architecture.md`
- `README.md`
- `docs/testing.md`

## 本轮实现范围

### 输入与触控

- Android 端按一次手势的首个落点决定是否由绘图区捕获，后续 MOVE/UP 不会因为滑入状态栏而丢失。
- MotionEvent 历史样本按 MOVE 发送，多指 POINTER_DOWN/POINTER_UP 会给每个 pointer 生成正确动作。
- Android 发送输入和控制消息改为各自的单线程执行器，避免在 UI 线程同步写 socket 导致触摸或笔事件触发崩溃。
- Windows 按时间戳和 pointer ID 分组注入历史触控样本，避免同一批次出现重复 pointer ID。
- Android View 已经提供旋转后的坐标，Windows 端不再重复旋转；横竖屏位置映射由此恢复正常。
- 防误触只在笔尖实际按下/移动时抑制手指。笔抬起后，手指仍可用于 Windows 触摸拖动和点击，但不会冒充笔迹。

### 会话与视频

- 新增客户端认证握手看门狗：ADB 启动成功但认证 socket 在超时内未建立时，会刷新一次性 token 并重新拉起 Android Activity，修复熄屏恢复后的 `invalid or missing session authentication token`。
- Media Foundation H.264 输出处理现在覆盖 FORMAT_CHANGE、输出类型重新协商、异步 MFT 事件和空输出重试，降低编码器重启及黑屏概率。
- 相关决策逻辑均补充原生单元测试；不要在没有硬件复测的情况下删去 stream-change 分支。

### Windows 界面与品牌资源

- 托盘状态、连接信息、显示器、码率、防误触和诊断工具已中文化；仅保留当前确实有功能实现的选项。
- Logo 源文件位于 `assets/branding/`。
- `tools/generate_app_icons.py` 可从透明 Logo 重新生成 Windows ICO、Android 普通/圆形/自适应图标。
- Windows 资源脚本会把图标嵌入 EXE，托盘和窗口类也加载同一资源；Android Manifest 使用 `@mipmap/ic_launcher`。

## 当前构建产物与实机状态

- Windows 构建输出：桌面端目标 `PadDrawBoard.exe`，位于本地构建目录（`out\*` 或 `build\`，随各机惯例而定）。
- Android Debug APK：`android/app/build/outputs/apk/debug/app-debug.apk`（标准 Gradle 输出路径，各机一致；2026-08-14 已重新构建验证）。
- `build/`、`out/` 与 `artifacts/` 是本地生成目录，不应加入 Git；打包与便携测试产物均位于 `artifacts/` 下。

最近一次部署后，Windows 和 Android 进程均正常运行，三个通道均为已连接状态。设备序列号和任何本机账户信息不要写入仓库或日志样例。

**注意**：本交接记录覆盖当前代码快照及已知门槛；下次会话仍应先运行 `git status --short --branch` 确认工作区。`cert_backup/` 属于本机材料，不得加入提交。

## 混合显卡视频排查记录（2026-08-15）

### 冻结根因与修复

- 冻结进程转储先后显示视频线程阻塞在 `IDXGIOutputDuplication::ReleaseFrame` 和 `AcquireNextFrame`，同时 NVIDIA NVENC MFT 工作线程位于驱动调用内。
- `VideoLoop` 原先提交异步编码输入后立即再次进入 Desktop Duplication，只在下一次捕获成功后才轮询编码输出；这会让桌面复制与 NVENC 在同一驱动队列内互相等待。
- 现已改为每轮先轮询编码器；有待完成输入时禁止再次进入 `AcquireNextFrame`，输出完成后才恢复捕获。D3D11 设备同时启用 `ID3D10Multithread::SetMultithreadProtected(TRUE)`。
- H.264 MFT 仅从捕获设备对应的 adapter LUID 通过 `MFTEnum2` 枚举，禁止混合显卡机器选中另一适配器上的硬件编码器。
- 临时高频阶段日志和循环探针已删除；保留低频适配器/编码器选择诊断。

### 实机验证

- 第一轮连续运行超过两分钟，序号到 8098；保留窗口约 62.24 fps，最大输出间隔 39.05 ms，capture-to-encode 中位数 13.92 ms、p95 15.09 ms，客户端视频延迟中位数 4.28 ms、p95 7.99 ms，丢帧 0、会话错误 0。
- 删除临时探针后的第二轮约 62.32 fps，最大间隔 32.17 ms，客户端中位数 4.16 ms、p95 9.37 ms，丢帧 0、无错误。
- UU 远程安装后新增 `GameViewer Virtual Display Adapter`，同一次实机会话仍正常；UU 不是本次冻结根因。

### 尚未解决：NVIDIA MFT 内存增长

- 正常编码时进程私有内存约线性增加 1.5 MB/s；capture-only 20 秒仅 65.35→65.73 MB，已排除 DXGI 捕获。
- 关闭高频遥测、丢弃编码输出且不做协议序列化/网络发送时斜率不变，已排除日志、协议、TCP 与 Android 解码。
- 20 Mbps 与 80 Mbps 斜率相同，说明增长不随码流大小变化。
- 已修复并测试 caller-provided 输出 sample 复用、替换 `pSample` 的 COM 所有权、输入 sample 在 `MFT_INPUT_STREAM_DOES_NOT_ADDREF` 条件下的安全复用；实机斜率仍不变。
- 系统软件 H.264 MFT 在当前 D3D 零拷贝输入配置下无可用候选（`E_NOTIMPL`）。若继续做软件回退，需要新增 GPU→CPU 回读与 CPU NV12 输入，这属于显著的延迟/架构取舍。
- 推荐下一步优先选择视频工作进程隔离并按内存/时间安全轮换，或引入另一套系统/厂商编码后端；不要在主进程内无限运行当前 NVENC MFT。

## 建议后续验证

1. 在 Blender 中分别验证笔压、连续笔划、单指点击/拖动和多指手势，确认应用级行为与 Windows 原生触摸注入一致。
2. 先解决或隔离 NVIDIA MFT 内存增长，再完成 `docs/testing.md` 中的一小时稳定性测试；当前只完成了两分钟级实机回归。
3. 在另一台不同 DPI/方向的 Android 平板上复测坐标、状态栏手势边界和自适应图标裁切。
4. 继续核实 Xiaomi Focus Pen 的按键与悬停硬件事件；未观测到的能力不得标记为支持。
5. 推送后观察 GitHub Actions；若 Windows 编码器测试在其他驱动环境失败，优先保留并分析失败 stage 与 HRESULT。
6. 若本地 SDK/工具链位置发生过迁移，用新位置完整跑一次 Windows 打包脚本，确认 cmake 与 platform-tools 引用正常。
7. 重启 adb 并复测三通道，确认控制、视频、输入通道恢复正常。
8. 为 NVIDIA MFT 内存增长实现有界恢复方案；在完成一小时测试前不要宣称长期稳定性达标。
9. 在纯 Intel 核显参考机上复跑一轮验证，确认混合显卡冻结修复没有破坏原有环境。
10. 推送后检查 `git status --short --branch` 与 GitHub Actions，确认 `cert_backup/` 保持被忽略且不在提交内容中。

## 建议技能

- `github:github`：查看仓库、PR 和 issue 状态。
- `github:gh-fix-ci`：处理推送后的 GitHub Actions 失败。
- `computer-use:computer-use`：需要复核 Windows 托盘或其他原生 UI 时使用。
- `agent-reach`：需要查询设备、驱动或平台官方资料时使用；只用于读取互联网内容。
- `handoff`：下一次会话结束前再次压缩更新本交接。

## 历史交接（早期 v0.1 基线）

## 1. 目标

- 在 Windows 与 Android 之间建立可用的 PadDrawBoard 实时板书/输入链路。
- 通过 USB ADB 三通道传输控制、视频与输入数据；遥测复用控制通道。
- 在 Windows 端使用 DXGI、D3D11 与 Media Foundation（MF H.264）完成采集/编码，在 Android 端使用 MediaCodec 完成解码/呈现。
- 支持 Windows Ink 输入架构，并保留后续笔按钮、悬停和三键发布能力的扩展空间。
- 提供可构建、可测试、可打包的 v0.1 工程基线。

## 2. 非目标

- v0.1 不承诺 penButtons、hover 或三键发布门槛已完成。
- v0.1 不承诺 Blender 全套流程已完成。
- v0.1 不承诺 1 小时 soak 测试已完成。
- 本文不将未完成项描述为已通过，也不扩展未验证的性能或兼容性结论。

## 3. 仓库地图

- `desktop`：Windows 端应用、采集/编码、输入注入与传输实现。
- `android`：Android 端应用、MediaCodec 解码/呈现及触控笔采集实现。
- `protocol`：Windows 与 Android 共用的协议、消息和通道定义。
- `tools`：构建、测试、打包、诊断或辅助工具。

## 4. 核心架构

### USB ADB 三通道

桌面端只监听 `127.0.0.1:48100/48101/48102`，通过 ADB reverse 建立控制、视频和输入三条 TCP 通道。遥测、时钟同步和请求 IDR 等消息复用控制通道。

### Windows 端：DXGI + D3D11 + MF H.264

Windows 端以 DXGI/D3D11 获取和处理图像，再交由 Media Foundation 的 H.264 编码链路生成视频数据，通过 USB ADB 视频通道发送至 Android。

### Android 端：MediaCodec

Android 端使用 MediaCodec 接收视频通道中的 H.264 数据，完成解码并输出到呈现链路。

### Windows Ink

Android 采集笔迹、压力、倾斜、距离和触控样本，经输入通道发送到桌面端；桌面端使用 `CreateSyntheticPointerDevice` / `InjectSyntheticPointerInput` 注入 Windows Ink。笔按钮与悬停能力仍属于未完成门槛。

## 5. 开发环境

- Windows 11 x64
- Visual Studio 2022
- MSVC 143
- Windows 11 SDK
- Android API 36
- Android NDK 28
- CMake 3.22
- JDK
- Android Platform Tools 36.0.2

## 6. 构建、测试与打包命令

### Windows 构建与 CTest

当前已验证的开发构建使用 Android SDK 附带的 CMake 3.22.1 和 MinGW：

```powershell
$env:PATH = "C:\Users\<用户名>\AppData\Local\Android\Sdk\cmake\3.22.1\bin;D:\mingw-w64\bin;$env:PATH"
cmake -S . -B out\connection-fix -G "MinGW Makefiles" -DPDB_BUILD_TESTS=ON
cmake --build out\connection-fix --parallel
ctest --test-dir out\connection-fix --output-on-failure
```

### Android Gradle

```powershell
Set-Location android
.\gradlew.bat assembleDebug
.\gradlew.bat test
```

### Windows 打包

```powershell
$env:PATH = "C:\Users\<用户名>\AppData\Local\Android\Sdk\cmake\3.22.1\bin;D:\mingw-w64\bin;C:\Program Files (x86)\NSIS;$env:PATH"
.\packaging\build-windows-packages.ps1 `
  -SourceDirectory (Get-Location).Path `
  -BuildDirectory (Join-Path (Get-Location).Path 'out\release-current') `
  -OutputDirectory (Join-Path (Get-Location).Path 'artifacts\release-current\windows') `
  -Configuration Release `
  -Generator 'MinGW Makefiles'
```

## 7. 实机 Pad7 结果

- 首 IDR：2.24s
- 三通道：3.18s
- 视频遥测：3.87ms
- RTT：6.9ms
- 丢帧：0
- CTest：7/7

以上为已记录结果；未将其外推为其他设备、网络条件或更长时间运行下的保证。

## 8. 关键修复清单

- 单实例互斥和 `SO_EXCLUSIVEADDRUSE`，避免多个桌面实例争抢同一端口。
- 修复 ADB 会话 token 启动竞态，并以 `am start -S` 确保 Android 使用新 token。
- 限制 ADB 子进程句柄继承，防止桌面退出后 ADB 继续占用三条监听 socket。
- 修复 Media Foundation H.264 的输出/输入类型设置顺序和硬件编码器兼容性。
- 为 BGRA 捕获纹理补齐 D3D11 视频处理器要求的绑定标志。
- 视频发送连续背压超过 2 秒时结束会话并重连，避免假 `CONNECTED`。
- 同时依据 `CleanPoint` 和 Annex-B NAL type 5 识别 IDR。
- 按接口要求以 `VT_UI4/1` 设置 ForceKeyFrame，保证静止桌面也能从首帧解码。
- 设备选择时忽略标准 `emulator-*`，但仍拒绝多台物理设备。

## 9. 配置与遥测

- 配置文件：`%APPDATA%\PadDrawBoard\config.json`
- 遥测文件：`%APPDATA%\PadDrawBoard\telemetry.jsonl`

## 10. 故障排查

### 状态为 CONNECTED 但黑屏

- 确认视频通道已建立且仍有数据流入。
- 检查 Windows DXGI/D3D11 获取、MF H.264 编码和 Android MediaCodec 解码链路。
- 查看 `%APPDATA%\PadDrawBoard\telemetry.jsonl`，对照首 IDR 与视频遥测记录定位卡点。
- 重新连接设备并确认 ADB 三通道均已恢复。

### 状态为 RECONNECTING

- 检查 USB 连接、设备授权和 Platform Tools/ADB 状态。
- 确认三个 ADB 通道是否都能重新建立。
- 若反复重连，保留遥测文件并记录发生时间、设备状态及 ADB 输出后再定位。

### 启动慢

- 重点检查首 IDR 生成、视频编码器初始化和三通道建立耗时。
- 对照已记录的 Pad7 基线：首 IDR 2.24s、三通道 3.18s。
- 避免将一次启动耗时直接认定为普遍性能结论。

### 端口占用

- 查找占用相关本地端口的进程并停止冲突实例。
- 确认没有遗留的 PadDrawBoard 或 ADB 转发进程。
- 重新启动 ADB/应用后再次建立三通道；不要在未确认占用者时盲目终止系统进程。

## 11. 未完成门槛

- `penButtons=[]`
- `hover=false`
- 三键发布阻塞
- Blender 全套未完成
- 1 小时 soak 未完成

这些项目仍是发布前门槛，不得标记为已通过。

## 12. 维护优先级

1. 先解决三键发布阻塞及 `penButtons=[]`、`hover=false` 门槛。
2. 完成并验证 Blender 全套流程。
3. 完成 1 小时 soak 测试并记录结果。
4. 保持 USB ADB 三通道、视频首 IDR、重连和启动耗时的回归验证。
5. 维护 CTest、Android Gradle 构建与 Windows 打包链路。
6. 继续完善配置、遥测和故障诊断信息。

## 13. Windows 最终产物校验

目录：`artifacts\release-current\windows`

- EXE SHA256：`638863fa5389b5cf474646831bfc18d270687a0dce9ddc420a48b8bec944419d`
- ZIP SHA256：`c671dc08b99cdee8be07255ff82c9d76742b1e66c22067b7e9441fea2f337308`
