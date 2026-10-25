# 2026-09-12 画面冻结复查

## 两条独立证据

1. Windows 事件日志 16:26:22 记录 `D:\PadDrawBoard\PadDrawBoard.exe` 崩溃，异常 `0xc0000005`，故障模块 `nvwgf2umx.dll`。该 EXE 是 8 月 9 日文件，SHA256 `5e80956de63bceb055baa4acff8574428d5627456ebc4b57fb59fe444ae6b30b`，没有更新为 9 月内存修复版。随后桌面进程消失，Android 连续 ECONNREFUSED。崩溃位置不能单独证明厂商驱动本身的根因。
2. 启动已交付的 Windows 修复版后，Android 新补丁仍反复重建 MediaCodec：几十秒内实例编号达到 78，客户端累计丢帧超过 700。原因是单帧入口丢失 USB 突发到包，加上普通参考帧丢失也重建 codec，造成恢复期间继续积压、继续重建的循环。这是上一轮 Android 修复引入的回归。

## 本轮修正

- Android 入口改为最多四帧的 FIFO，始终只有一个待递送任务，仍然有严格容量上限；超过容量时报告全部丢失帧并重新等待 IDR。
- 等待可用解码输入槽时保留当前 pending 帧，递送任务每 2ms 延后检查，不用新帧覆盖它。正常短突发保留 IDR 与随后参考帧顺序。
- 普通参考链丢失只清理 pending、等待并请求 IDR，保留当前 MediaCodec 和输入槽；只有真正的 codec 错误、超大帧输入槽恢复、配置/Surface 变化才重建。
- `D:\PadDrawBoard` 的 EXE 与两个运行库已更新；原文件在 `D:\PadDrawBoard\backup-before-20260912`。更新后的 EXE SHA256 为 `0ecd81a29ac688de78e6fa1c8cc06390bd47175ba85097ff445f3cd8bbbad314`。
- 最终 Android APK 已成功覆盖安装；交付路径仍为 `artifacts/android-reliability/PadDrawBoard-android-reliability-debug.apk`，SHA256 为 `3f0e386f3f2f1b0d827f0561b70d7692673950060c53fb1b548872eac19142c1`。

## 验证

- Android 单元测试 33 项通过，新增四帧正常 USB 突发保序测试，更新 10,000 帧过载下的固定容量与丢帧计数测试；APK 构建通过，`git diff --check` 通过。
- 最终从 `D:\PadDrawBoard\PadDrawBoard.exe` 运行。16:37:38–16:38:38 七次采样：视频序号 1389→3976；客户端丢帧计数保持 0；stream_resets 保持 2，不再连续增加；Windows 私有内存 111.89–113.82 MiB。
- 平板日志中同一 MediaCodec 实例持续 `onReleaseOutputBuffer ... render: 1`，未见此前反复重建。仍有偶发约 100ms 的呈现间隔，不能把此次冻结修复写成已达到稳定 60fps 或端到端延迟门槛。
- 数据保存在忽略目录 `out/freeze-diagnosis/`：崩溃前遥测、重建循环日志、最终采样和解码日志。没有使用累计历史 crash_count 作为本轮新增崩溃数。

之前因安装限制未执行的资源失败 instrumentation 测试仍不计为通过；本轮仅针对画面冻结回归验证。三键、Blender 完整流程和一小时验收仍待单独完成。
