# Android 队列、资源释放和遥测修复（2026-09-12）

> 更新：下文记录初次补丁及当时安装限制。16:39 已完成 APK 安装，并修正单帧入口/过度重建的冻结回归；当前采用四帧有界 FIFO，普通参考帧丢失不重建 codec，单测 33 项。当前状态、APK 哈希和实机结果以 [冻结复查](freeze-recovery-20260912.md) 为准。

## 范围与验收

本轮按用户授权分点修复四类已确认问题。工程决策为分阶段局部修复，不改变三通道协议、默认端口、硬件编码后端或绘画功能，不添加依赖。原有 Windows 内存修复和其他未提交修改保留。

A0 回执：`code-quality-workflow` 路由，快照 `2026-08-18`，读取 `ST-A0`；仅采用目标、边界、验收与最小改动检查，没有采用域外候选。验收为 Android 单元测试、失败路径设备测试、APK 构建、延迟工具回归和真实连接复测。各阶段可按本文文件组回退本轮改动，不能整体重置工作区。

## 已实现

1. **视频入队前有界化**：`VideoFrameInbox` 在网络线程与 Handler 之间只持有一帧、最多一个待执行递送任务；重复入队只替换该帧并累计丢帧。正在处理的帧和 codec pending 各自也有固定上限。关闭时立即清空入口数据。
2. **参考帧恢复**：非关键帧不能替换任何待提交帧。参考链发生丢失后，释放旧解码器、进入等待 IDR 状态；超出输入 buffer 容量也通过重建归还输入槽。已在等待 IDR 时连续丢帧不会循环重建，但会重新请求可能丢失的 IDR。
3. **发送队列**：输入、控制各用一个线程，最多八个待执行任务；保留队内顺序。溢出显式触发会话断开和重连，并清空旧任务。Windows 现有 `EndSession -> input_.OnDisconnect()` 负责解除按下状态；不静默丢弃 UP/CANCEL 后继续使用原会话。
4. **资源所有权**：MediaCodec 创建后立即纳入释放路径，configure/start 异常也 release。Socket 在 connect 前登记，任意通道中途失败都关闭已登记连接。重连代次防止已取消的连接尝试继续登记；旧连接读取/写入失败不会无条件污染新会话。
5. **遥测**：解码丢帧回调同时累加会话计数与界面状态，状态更新串行化。发布延迟分析仅接受明确的 `glass_sample` / `glass_to_glass`，桌面 `video_sample` 不再被误当成实际屏幕延迟。协议字段保持兼容，文档注明编码完成与物理呈现的差别。

## 验证记录

- 修改前：Android 单元测试通过；延迟分析器 6 项通过。
- 修改后：Android 单元测试 **32 项通过**，包括 10,000 帧阻塞入队的容量/计数检查、关闭后不再持有帧、发送队列阻塞时拒绝溢出且保持事件顺序、参考帧丢失后等待 IDR。
- 延迟分析器 **7 项通过**，新增桌面编码样本不能通过玻璃到玻璃门槛的回归测试。
- `testDebugUnitTest assembleDebug assembleDebugAndroidTest` 构建成功。
- 设备测试源码已构建：连续三次中途连接失败的 socket 关闭检查；连续三次配置失败后的真实 MediaCodec 释放检查。
- 设备授权正常，但测试 APK 安装被 HyperOS 拒绝：`INSTALL_FAILED_USER_RESTRICTED: Install canceled by user`。**上述两项设备测试尚未执行，不能记为通过**；已请求用户确认平板系统安装提示。测试框架失败清理后设备暂未检出主应用；随后独立重装主 APK 也被相同系统限制拒绝，需要用户确认安装提示后恢复安装。
- `git diff --check` 通过。完整工作区范围检查触发默认 5 文件/200 行警告，包含前轮 Windows 修改及原有未跟踪文件；不能宣称整个工作区通过默认预算。本轮按视频、传输、遥测与测试分组审阅，未更改桌面源码或协议生成文件。

## 继续设备验证

为避免测试框架安装失败后的清理影响主程序，继续使用独立安装与 instrumentation 命令，安装时由用户确认平板提示：

```powershell
& D:/Android/Sdk/platform-tools/adb.exe install -r android/app/build/outputs/apk/debug/app-debug.apk
& D:/Android/Sdk/platform-tools/adb.exe install -r -t android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
& D:/Android/Sdk/platform-tools/adb.exe shell am instrument -w io.paddrawboard.client.test/androidx.test.runner.AndroidJUnitRunner
```

之后启动 `artifacts/memory-fix/windows/PadDrawBoard.exe`，由桌面端自动建立经过认证的三通道。检查视频继续输出、横竖屏恢复、客户端丢帧遥测与内存；不能把短时复测写成一小时稳定性或真正玻璃到玻璃验收。

三键探测、Blender 全流程、一小时稳定性和摄像机延迟测量仍是独立验收任务，本轮不宣称这些门槛完成。

交付 APK：`artifacts/android-reliability/PadDrawBoard-android-reliability-debug.apk`；SHA256：`9068ab7a6134332b017f9292dd2cbc716a6a1c882a396b4d2360d71c11c81778`。
