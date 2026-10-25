# PadDrawBoard 

把**小米平板** 变成 Windows 的第二块屏幕（数位屏），**支持原厂触控笔压感**。
基于上游开源项目 **PadDrawBoard**（GPL-3.0）修改而来。
别管这个那个的把项目网址复制给大肥鱼就是了。

>
> 本分支中**所有代码改动均由蓝色大肥鱼(deepseek)编写**
> 额你自己用的时候记得要开一下平板的USB调试。
> 上游项目作者与这些改动**没有任何关系**，请不要向上游报告本分支的问题。

---

## 这个分支解决了什么

上游项目原生的目标机型是小米平板 7 / 焦点触控笔。本分支把它跑通在
**小米平板 5 Pro（`M2105K81AC` / 代号 `elish`，HyperOS）** 上，并做了若干
易用性改造。

### 已验证可用

| 能力 | 状态 |
|---|---|
| USB 连接（`adb reverse`，不监听外网） | ✅ |
| 画面串流（H.264，2560×1600） | ✅ 实测 60fps，延迟 5–6 ms |
| **触控笔压感** | ✅ 实测压力 0–0.926，1103 种取值，注入 Windows Ink 0–948 |
| 笔倾斜（tilt） | ✅ 双向有效 |
| 笔悬停 / 距离 | ✅ |
| 平板作为副屏显示电脑画面 | ✅ |
| 关闭 APP / 结束程序后还原平板设置 | ✅ 含强制结束的场景 |

### 本分支的主要改动

1. **adb 版本锁定为 36.0.2**
   上游 `AdbLocator` 用**精确字符串比对**判断版本，装 37.x 会静默失败
   （桌面端判定"找不到 ADB"，从不建立连接）。

2. **必须横屏**
   `ChooseVideoSize` 要求客户端上报宽度 ≥ 1920，而 Pad 竖屏只有 1600 宽，
   会被直接拒绝。本分支改为**由 APP 自己请求横屏**（`requestedOrientation`），
   不改系统设置，退出即还原。

3. **断连自愈 + 锁竖屏**
   电脑端消失（崩溃 / 强制结束 / 拔线）后，APP 会检测到并：
   先锁定竖屏 → 关闭自身 → 结束进程，不留冻结画面。

4. **开机自启 / 插上即用**
   桌面端注册登录自启任务；插上 USB 后自动在平板拉起客户端并建立会话，
   无需操作电脑。关闭平板 APP 后自动停止，不反复重启。

5. **托盘与退出修复**
   - 补 `TaskbarCreated` 处理：开机时 Explorer 未就绪也能出现图标，Explorer 重启后恢复
   - 退出不再阻塞消息循环（原来点击"退出"会假死几十秒）
   - 安装反向映射前先 `--remove-all`，避免残留导致首次连接失败

6. **不修改系统旋转设置**
   早期版本用 `settings put system user_rotation` 强制横屏，但写回时**画面不会立即旋转**
   （自动旋转依赖重力感应），且强杀时无法还原。现改为应用级横屏。

---

## 已知限制

- **仅支持 USB**，依赖 `adb reverse`；Wi-Fi 不可用，意思是完全不需要联网！！
- **需要开发者选项中的「USB 调试」常开**（这是 ADB 传输的硬性前提，
  没有绕过的办法）。模拟输入还需要「USB 调试（安全设置）」。
- **合上笔记本盖子会中断**：合盖时系统禁用内屏，采集目标消失，平板停在最后一帧。
  如需真正支持，需要虚拟显示器驱动。
- **平板安全锁屏无法自动解锁**（`wm dismiss-keyguard` 对 PIN/图案无效），
  但会自动保持亮屏（`svc power stayon usb`）。
- **笔身按键**：硬件上确实有两个按键（内核 `BTN_STYLUS` / `BTN_STYLUS2`），
  但笔的数字化器设备没有按键布局，Android 收不到；实测按键从蓝牙通道
  以 `KEY_PAGEUP` / `KEY_PAGEDOWN` 上报。相关映射曾实现后移除
  （注入 `Ctrl+Alt+Z` 会误触 NVIDIA 覆盖层的 `Alt+Z`）。
- 笔按键识别依赖 `penButtons` 探针，只影响 `release_ready` 标志，不影响压感与画面。
  哪天给肥鱼的大白饭到了会接着更更新。
---

## 环境

| 组件 | 版本 |
|---|---|
| Windows | 11 25H2 |
| 平板 | 小米平板 5 Pro |
| 触控笔 | Xiaomi stupid Pen |
| MSVC | 14.44.35207（VS 2022 Build Tools） |
| Windows SDK | 10.0.26100.0 |
| CMake | 3.31.0 |
| JDK | 21 |
| Gradle | 8.9 |
| Android SDK | platform-36 / build-tools 35.0.0 |
| adb | **36.0.2（必须精确匹配）** |

---

## 构建

### 桌面端

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target PadDrawBoard
```

> 用 Visual Studio 生成器时**不需要** `vcvars`，MSBuild 会自己找到工具链。

### 单元测试

```powershell
ctest --test-dir build -C Release --output-on-failure
```

### 安卓端

上游用 AGP 构建。本分支的实际构建环境出现过
`AAPT ... stableIds.txt: failed to open` 的问题，最终是用
`aapt2 compile/link + kotlinc + d8 + zipalign + apksigner` 手工流水线打包的。

需要注意：`d8` 的输入**必须包含 `kotlin-stdlib`**，否则运行时会
`NoClassDefFoundError: kotlin/enums/EnumEntriesKt`。

---

## 目录结构

- `desktop/` —— C++20 桌面端（捕获、编码、输入注入、ADB、托盘）
- `android/` —— Kotlin 平板端（硬解、输入采集、诊断）
- `protocol/` —— 版本化传输协议
- `docs/` —— 架构、测试与已知问题
- `packaging/` —— Platform Tools 获取与打包

---

## 许可证与致谢

本项目沿用上游的 **GPL-3.0-only** 许可，见 [LICENSE](LICENSE)。
第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

原始项目：**PadDrawBoard**（上游作者与贡献者）。上游 README 保留为
[README.upstream.md](README.upstream.md)。

本分支的硬件排查记录见 [docs/pad5pro-notes.md](docs/pad5pro-notes.md)。
