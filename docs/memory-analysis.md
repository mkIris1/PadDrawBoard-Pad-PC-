# 内存增长排查与修复（2026-09-06）

## 已确认的根因

问题是本机 MinGW WRL 实现的引用计数差异，不能归因于 NVIDIA 驱动自身泄漏。

`D:/mingw-w64/x86_64-w64-mingw32/include/wrl/client.h` 中的 `ComPtr::Attach()` 调用了 `InternalAddRef()`。项目原先把它当作“接管 API 已交付的一次引用”，结果多加一次引用，离开作用域后仍剩一次，对象无法销毁。

最小复现的实测输出：`Attach references=2; fixed adoption references=1`。新增回归测试覆盖初次接管、接管同一对象的另一份引用、替换对象和最终释放；不依赖真实显卡，也不只是检查一段条件逻辑。

三个泄漏入口都位于 `desktop/src/video/h264_encoder.cpp`：

- 输出 sample：每帧多留一个引用，因此私有内存随编码帧数持续增长。
- 输出事件集合：同样必须接管已有引用，不能再 AddRef。
- `ICodecAPI`：每次配置/请求关键帧都可能把整个编码器留住，导致重建后阶梯式增长。

所有这些入口统一使用 `AdoptComReference()`，通过 `ReleaseAndGetAddressOf()` 接管已有引用，兼容本机 MinGW 和微软 WRL。异步 MFT 退出及未选中的旧候选还补充了显式 `MFShutdownObject()`。

微软资料：[ComPtr 接口](https://learn.microsoft.com/en-us/cpp/cppcx/wrl/comptr-class)、[异步 MFT 显式关闭要求](https://learn.microsoft.com/en-us/windows/win32/api/mfidl/nn-mfidl-imfshutdown)。工具链的额外 AddRef 以本机头文件和最小复现为直接证据，不外推到所有 MinGW 版本。

## 最终实现

继续使用 NVIDIA 硬件 H.264、原 GPU 捕获/转换路径。排查时的软件编码回退、CPU 回读和实验性逐帧 Flush 均已撤回，没有新增编码依赖，也没有升级驱动。

同时落实实际 60Hz 发送节奏，使用进程内高精度等待定时器。编码格式声明为 60fps 本身不会限制捕获速度。等待发生在完成的帧之间，独立输入线程不参与等待。

## 验证证据

当前机器：RTX 4060 Laptop GPU，驱动 32.0.16.1088，原生编码画面 2560×1600。

| 项目 | 结果 |
| --- | --- |
| 修复前原管线 | 第 5 秒约 123 MiB，第 85 秒约 359 MiB，约 2.94 MiB/s 增长 |
| 仅捕获＋转换对照 | 第 5 秒约 76 MiB，第 85 秒约 78 MiB |
| 修复后 NVIDIA 硬件压力测试 | 120 秒通过，期间 23 次以上关键帧/重建请求；第 20 秒 120.59 MiB，第 115 秒 126.65 MiB，未超过预热后增长 16 MiB 的门槛 |
| 真实平板便携版 | USB 三通道建立，视频序号持续增长；多次横竖屏切换后内存约 109–112 MiB，自动旋转设置恢复 |
| 最终实机遥测窗口 | 120 秒约 55.47fps；捕获至编码中位 12.70ms、p95 24.15ms，发送队列深度为 0；这是编码管线时延，不是玻璃到玻璃延迟 |
| 编译与单测 | Release 编译通过；CTest 7/7；协议一致性 6/6；延迟工具测试 6/6 |

Android 源码未修改，使用平板已有安装版本联调。内存有分配器缓存及 GC 波动，分钟级稳定性不等于已完成一小时测试。复杂持续动画、其他显卡、Blender 笔压/三键整体验收仍待完成。

当前 Android 解码丢帧回调未接到 `AdbSession.recordDroppedVideoFrame()`，所以客户端遥测里的零不能证明实际零丢帧。接收遥测也不能替代真正的玻璃到玻璃延迟。

本机最终证据位于忽略目录 `out/memory-fix/`：`hardware-fixed-stress.csv`、`hardware-package-soak.jsonl`。`baseline.csv` 是修复前对照。其他 CPU/软件/Flush/早期 soak 文件均为排查过程数据，不是最终交付结果。

## 重复验证与运行

构建时启用 `PDB_BUILD_TESTS=ON`，在已登录、具有真实显示器的 Windows 会话手动运行：

```powershell
.\out\memory-fix\desktop\pdb_video_memory_probe.exe 120
```

默认 120 秒，可传入大于等于 60 的秒数；每 5 秒请求一次 IDR。使用固定大小统计、不保存画面，20 秒预热后净增长超过 16 MiB、管线失败、IDR 未恢复或不能持续产帧会返回非零退出码。它不加入无显示器的 CTest 环境；仍需检查分辨率、帧率及测试负载。

便携程序：`artifacts/memory-fix/windows/PadDrawBoard.exe`。依赖 DLL、ADB 36.0.2 和许可证已随目录附带。既有 APK 可继续使用，无需为本次 Windows 修复重装。

## 值得进一步优化的地方（本次仅审查）

1. **Android 入口队列有界化（P1）**：`AvcDecoder.queue()` 先把完整 ByteArray 放入 Handler 消息，之后才检查单帧 pending，因此解码线程堵塞时消息仍可无限积压。输入/控制执行器也没有队列上限。优化时须处理 AVC 参考帧恢复，并保留 UP/CANCEL，不能粗暴丢弃笔抬起事件。
2. **失败路径释放（P1）**：解码器 configure/start 失败时可能未 release 已创建实例；三个 socket 只在全部连接成功后加入关闭集合，中途失败可能遗漏已打开的 socket。适合单独做 Android 生命周期补丁并用失败注入验证。
3. **遥测准确性（P2）**：接通真实解码丢帧统计；`AppState.update()` 的并发读改写也应串行化或原子更新。补充真正的显示呈现时间后再评估后续延迟优化。
4. **分辨率策略与工具链回归（P2）**：复杂动态画面下检验升降级抖动；把 COM 生命周期测试保留在 MSVC/MinGW 双工具链 CI 中，避免再次被智能指针实现差异误导。

## 修改边界

工程路由 `code-quality-workflow`，A0/ST-A0，快照 2026-08-18。保留开工前已有未提交修改，不改协议或驱动，不提交本机证书材料。按引用生命周期、帧节奏、验证工具分阶段修改，没有项目重写。

全工作区范围检查包含此前的大量未提交修改和未跟踪本机证书，超出默认 5 文件/200 行门槛，不能声称全工作区预算检查通过。本次新增修改另行逐项审查，`git diff --check` 通过。
