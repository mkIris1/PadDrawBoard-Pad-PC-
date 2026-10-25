# 小米平板 5 Pro 实机排查记录

> 本文记录把 PadDrawBoard 跑通在 Pad 5 Pro 上的实测结论，供后来者少走弯路。
> 所有结论都来自真机验证。

## 1. adb 版本必须精确是 36.0.2

`desktop/src/adb/client.cpp` 的 `AdbLocator::LocateWithDiagnostics`：

```cpp
if (*parsed != options.required_bundled_version) {   // 精确字符串比对
    diagnostics << LocatorFailure(path, "reported version " + *parsed +
                                  " but required " + options.required_bundled_version);
    return false;
}
```

装 37.x 会让桌面端判定"找不到 ADB"，表现为**不建立反向通道、不拉起客户端、
没有任何报错**。把 platform-tools 36.0.2 放到 `tools/adb/36.0.2/` 即可。

## 2. 必须横屏（宽度 ≥ 1920）

`desktop/src/app/desktop_server.cpp` 的 `ChooseVideoSize`：

```cpp
for (const std::uint32_t longest_edge : video::kLongestEdgeLadder) {  // {3200, 2560, 1920}
  const video::Size candidate = video::SelectEncodeResolution(native, longest_edge);
  if (candidate.valid() && candidate.width  <= hello.maxVideoWidth &&
                            candidate.height <= hello.maxVideoHeight) {
    return std::make_pair(candidate, longest_edge);
  }
}
return std::nullopt;   // → "client video dimensions are below the 1920px v1 ladder"
```

Pad 5 Pro 竖屏是 1600×2560，宽度只有 1600，三档全部失败，服务器直接拒绝。

**推荐做法**：让客户端自己请求横屏（`requestedOrientation`），
不要改系统 `user_rotation`——写回系统设置时画面不会立即旋转，
而且电脑端被强杀时没有机会还原。

## 3. 令牌只能由桌面端生成

`AdbSession::ValidateSessionToken` 只在 `authentication_armed_` 为真的窗口内接受令牌，
而它只在 `AttemptConnect` 刚生成令牌的那一刻为真。所以手工
`adb shell am start --es <token>` 注入的令牌必然被拒。必须让桌面端自己拉起客户端。

## 4. 笔的按键：硬件有两个，但 Android 收不到

内核里能看到两个设备：

```
NVTCapacitivePen   （笔尖数字化器，/dev/input/event5）
  KEY: BTN_DIGI  BTN_TOUCH  BTN_STYLUS  BTN_STYLUS2
  ABS: PRESSURE 0..4095, TILT_X/Y -60..60, DISTANCE 0..1
  但 KeyLayoutFile 为空 → Android 不会映射 BTN_STYLUS*

Xiaomi Smart Pen   （笔身蓝牙，/dev/input/event13，Classes: KEYBOARD|EXTERNAL）
  实测按键上报为：
    上键 → KEY_PAGEUP
    下键 → KEY_PAGEDOWN
```

`penButtons=[]` 是客户端探针不认这类按键，不是硬件没有按键。

> 曾尝试把下键映射成撤销快捷键，但 `Ctrl+Alt+Z` 包含 `Alt+Z`，
> 会误触 NVIDIA GeForce Experience 的覆盖层。做这类功能必须避开注册过的全局热键，
> 或者干脆走应用自己的脚本接口。

## 5. MIUI 会把被杀掉的 APP 拉起来

`am force-stop` 之后约 14–16 秒，MIUI 会把进程重新拉起并置前台。
**给应用加电池优化白名单 / 后台运行权限会让这个行为更明显**——本想提高稳定性，
反而导致"关了又自己开"。去掉这些授权后 force-stop 能保持生效。

## 6. adb 服务会卡死

反复强杀桌面端进程会留下孤立的 adb 守护进程，导致所有 adb 命令超时数分钟。
恢复办法：结束所有 `adb.exe`，重启程序即可。

## 7. 压感链路实测数据

```
平板  MotionEvent.getPressure()  [0,1]          实测 0 ~ 0.926（1103 种取值）
传输  归一化 u16（×65535）
电脑  NormalizedPressure(/65535) → ToPressure(×1024)   实测 0 ~ 948
注入  Windows Ink POINTER_PEN_INFO.pressure
```

tilt 双向有效（tiltX -56~57，tiltY -53~53）。
