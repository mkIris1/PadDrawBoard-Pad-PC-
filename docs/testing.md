# PadDrawBoard 延迟与长时间稳定性测试

延迟工具是无外部依赖的 Python 3 工具。它读取控制、视频和输入遥测通道导出的 JSON Lines，生成稳定且适合机器读取的 JSON 报告。

## 运行分析器

在仓库根目录执行：

```text
python tools/latency/latency_tool.py tools/latency/fixtures/latency.jsonl --output report.json
python tools/latency/latency_tool.py tools/latency/fixtures/soak_failure.jsonl --mode soak --min-duration-s 0
python tools/latency/latency_tool.py exported-control.jsonl --mode latency
python -m unittest discover -s tools/latency -p "test_*.py" -v
```

分析器接受合并后的导出文件。当导出文件有意只包含一种遥测类别时，可使用 `--mode clock`、`--mode latency` 或 `--mode soak`；默认的 `--mode all` 要求三种类别全部存在。空行以及以 `#` 开头的行会被忽略。报告会输出到 stdout；指定 `--output` 后，还会以 UTF-8 JSON 格式写入文件。输出包含 `schema_version: 1`，以及 `clock`、`latency`、`soak`、`overall_pass` 和 `failures` 字段。

退出码适用于 CI：

| 代码 | 含义 |
| ---: | --- |
| 0 | 所请求的延迟和稳定性门槛全部通过 |
| 2 | 输入格式错误、输入不可读，或 CLI 参数值无效 |
| 3 | 输入有效，但一个或多个门槛未通过 |
| 4 | 未提供任何遥测记录 |

35 ms 的玻璃到玻璃中位延迟是优化目标，不是发布门槛。工具会通过 `latency.optimizationTargetMet` 明确报告该目标是否达成。发布硬门槛为：中位数不超过 50 ms、p95 不超过 70 ms、输入传输 p95 不超过 8 ms；三项合并结果记录在 `latency.releasePass` 中。因此，中位数为 40 ms 的运行结果会得到 `optimizationTargetMet: false`，但只要发布硬门槛和所有请求的稳定性门槛均通过，进程仍会以 0 退出。

## JSONL 遥测格式

每一行都是一个 JSON 对象。合并后的导出文件可以包含以下任意记录类型。时间戳后缀可以是 `_ns`、`_us`、`_ms` 或 `_s`；不带后缀的 `timestamp` 字段按纳秒解释。

### 时钟同步

每次 NTP 风格的四时间戳交换导出一条记录：

```json
{"kind":"clock_sync","device_send_ns":1000000000,"host_receive_ns":1003000000,"host_send_ns":1004000000,"device_receive_ns":1010000000}
```

分析器按 `((host_receive - device_send) + (host_send - device_receive)) / 2` 计算主机时间减设备时间的时钟偏移，并按 `(device_receive - device_send) - (host_send - host_receive)` 计算 RTT。偏移为正表示主机时钟领先。协议遥测也接受 `client_send_timestamp_ns`、`host_receive_timestamp_ns`、`host_send_timestamp_ns` 和 `client_receive_timestamp_ns` 作为别名。

### 玻璃到玻璃与输入样本

玻璃到玻璃样本可以直接提供：

```json
{"kind":"glass_sample","glass_to_glass_ms":32.4}
```

也可以根据实际测量的捕获时间戳和屏幕呈现时间戳计算，必须明确标记为 `glass_sample`：

```json
{"kind":"glass_sample","capture_timestamp_ns":1000000000,"presentation_timestamp_ns":1032400000}
```

桌面 `video_sample.presentation_timestamp_ns` 实际表示编码完成，不能改名后充当屏幕测量；分析器不再将 `video_sample` 纳入玻璃到玻璃发布门槛。Android 的视频遥测只覆盖编码完成到接收，也不包含解码及屏幕显示。

输入传输样本可以使用毫秒、微秒或发送/接收时间戳：

```json
{"kind":"input_transport","latency_us":5200}
{"kind":"input_transport","input_send_ns":5000000000,"input_receive_ns":5006500000}
```

### 长时间稳定性健康点

在一小时运行期间持续输出累计健康点。`crashes` 是累计进程崩溃次数。优先使用 `active_pointer_ids`，因为它能帮助诊断指针卡住问题；也接受 `active_pointers`。

```json
{"kind":"soak","timestamp_s":0,"queue_depth":0,"active_pointer_ids":[],"crashes":0,"latency_ms":31.0}
{"kind":"soak","timestamp_s":3600,"queue_depth":0,"active_pointer_ids":[],"crashes":0,"latency_ms":31.3}
```

稳定性门槛默认要求运行时间至少为 3600 秒；队列深度从第一条到最后一条不得增加；结束时活动指针数必须为 0；崩溃次数必须为 0；延迟范围不得超过一帧 60 Hz 视频的时长（16.6667 ms）。只有短时诊断或单元测试才使用 `--min-duration-s 0`；它不是参考验收运行。

## 高速摄像机 / LED 视觉延迟测试流程

使用以下流程进行实体玻璃到玻璃测量。它测量完整链路：桌面捕获、编码、ADB 传输、Android 解码/显示，以及实体显示屏响应。

1. 使用至少支持 1000 fps 的高速摄像机，固定快门、固定焦距，并将曝光时间设短，使 LED 转换边缘清晰。刚性固定摄像机，确保同一帧中同时包含参考显示器和 Pad 7 屏幕。关闭摄像机防抖和自动曝光。
2. 将明亮且响应快速的红色 LED 连接到 GPIO 或微控制器输出，并确保摄像机能拍到 LED。相同输出还应使被捕获桌面全屏切换黑/白（例如使用由 GPIO 控制器驱动的小型本地测试应用）。LED 是视觉起始标记；不要使用 USB 数据包时间戳作为玻璃到玻璃结果。
3. 将桌面和 Pad 7 设置为选定的原生适配 60 Hz 模式。关闭 HDR、夜间灯光、自适应亮度、摄像机自动处理和所有屏幕叠加层。记录确切分辨率、码率、编码器、Android 构建版本、线缆和主机 GPU。
4. 预热会话五分钟。以低于 2 Hz 的频率生成至少 100 次随机开/关转换，避免摄像机对转换产生混叠。保持笔静止，并在同一次运行期间单独采集一段输入传输导出数据。
5. 对每段摄像机序列，从 LED 边缘开始计算 1000 fps 帧数，直到第一帧中 Pad 7 画面出现可见变化。如果参考显示器不是源边缘，则扣除摄像机测得的 LED 到参考显示器延迟。使用摄像机测得的帧周期将帧数换算为毫秒；模糊或被遮挡的转换应舍弃。
6. 将合格测量结果录入 `glass_sample` JSONL 记录。使用工具报告的中位数和插值 p95。在诊断门槛失败时，分别使用 Pad 7 原生适配、长边 2560 和长边 1920 模式重复测试；除非原生适配未达到发布限制，否则验收模式为原生适配。
7. 将摄像机元数据和原始视频片段与 JSON 报告放在一起保存。有效的发布结果要求中位数不超过 50 ms、p95 不超过 70 ms；项目目标为中位数不超过 35 ms。

为了获得可重复的电气触发，LED 和显示切换应由测试应用中的同一个单调事件驱动。不要仅根据桌面遥测的 `presentation_timestamp_ns` 推断玻璃到玻璃结果；该时间戳结束于 Android 面板发出光子之前。

## 参考硬件验收清单

将清单记录在测试产物中，并附上 JSON 报告。

- [ ] Windows 11 23H2 或更高版本，x64；已记录主机时区和构建版本。
- [ ] Xiaomi Pad 7 使用 HyperOS 2，存在一台已授权设备，屏幕处于唤醒状态且未发生热降频。
- [ ] Xiaomi Focus Pen 已配对且电量充足；通过普通 Android 事件或无需 Root 的 ADB 探针观察到压力和全部三枚按键。若不支持悬停、倾斜或距离，必须报告为不可用，绝不得伪造。
- [ ] USB 3.2 数据线直接连接主机；未使用集线器或仅充电线；`adb reverse` 已为端口 48100、48101 和 48102 启用。
- [ ] 主机 GPU 属于 RTX 4060 或 AMD 780M 参考级别，已选择硬件 H.264 路径，使用 8-bit SDR 且未使用 B 帧。
- [ ] 已选择原生适配 60 fps 模式；视频队列容量为一个待处理帧，输入使用独立通道。
- [ ] 已采集至少 100 次摄像机转换；报告包含玻璃到玻璃中位数、p95 和输入传输 p95。
- [ ] 一小时稳定性测试至少包含 3600 秒时间戳，崩溃次数为 0，结束时活动指针数为 0，队列无增长，延迟漂移不超过 16.6667 ms。
- [ ] 已完成 Blender 4.5.1 雕刻、纹理绘制和 Grease Pencil 冒烟检查；压力保持连续，配置的笔按键执行预期操作。
- [ ] JSON 报告、命令行、git 修订版本、摄像机片段/元数据以及所有门槛失败信息已一并归档。
