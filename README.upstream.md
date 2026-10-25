# PadDrawBoard

PadDrawBoard 是一个仅限 GPL-3.0 的 Windows/Android 项目，可将小米 Pad 7
和小米焦点触控笔变成面向 Windows Ink 应用（例如 Blender）的低延迟镜像数位屏。

v1 版本的目标环境是 Windows 11 23H2 及更高版本、HyperOS 2，以及一台通过 USB
连接的平板；支持最高 3200x2136、60 fps 的 H.264 SDR 视频、具备压力感应的笔输入
和多点触控。USB 传输使用经过明确授权的 Android Debug Bridge 连接；桌面端应用
不会对外暴露网络监听器。

## 仓库结构

- `desktop/` —— 原生 C++20 捕获、编码、输入、ADB 和托盘应用。
- `android/` —— Kotlin 客户端、硬件解码、输入采集和诊断功能。
- `protocol/` —— 版本化传输协议的唯一事实来源。
- `packaging/` —— 固定版本 Platform Tools 的获取、Windows 打包、APK 收集和校验和生成。
- `cmake/` —— Windows x64 ZIP 和 NSIS 目标的安装及 CPack 规则。
- `tools/` —— 开发工具和硬件诊断工具。
- `docs/architecture.md` —— 已冻结的 v1 集成边界。
- `docs/testing.md` —— 延迟、稳定性和参考硬件验收方法。
- `docs/handoff.md` —— 当前实机状态、已知限制、排障步骤和维护交接清单。

## 开发状态

项目仍在积极开发中。所有硬件能力声明都必须先通过 Pad 7 能力探针和参考系统
延迟测试后才能确认。特别是倾斜和悬停功能，只有在 HyperOS 提供真实设备数据时
才会报告为支持。

## 原生端构建与测试

前置条件：

- Visual Studio 2022 Build Tools，包含 MSVC v143 和 Windows 11 SDK。
- CMake 3.22 或更高版本。

```powershell
cmake -S . -B build -A x64 -DPDB_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

打包与普通开发构建分开执行，因为官方 Platform Tools 压缩包会在构建时下载，
且不会提交到仓库：

```powershell
.\packaging\build-windows-packages.ps1 `
  -BuildDirectory .build-release-package `
  -OutputDirectory artifacts\windows `
  -Configuration Release
```

该命令会生成便携版 ZIP、NSIS 安装程序，以及 `SHA256SUMS.txt` 和
`SHA256SUMS.json`。生成安装程序需要 `makensis.exe`；没有 NSIS 的本机可以使用
`-SkipNsis`，只执行 ZIP 检查。CMake 构建也提供 `package-zip` 和 `package-nsis`
目标。

随程序提供的 ADB 目录为 `tools/adb/36.0.2/`。获取脚本会固定使用并校验 Google
Platform Tools 36.0.2 压缩包，验证 `adb version`，并复制 Google 提供的完整
`NOTICE.txt` 及其他附带文件。脚本不会跟随可变的 `latest` URL。详见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## Android 端构建与测试

前置条件：JDK 21 和 Android SDK platform 36。CI 会明确安装 platform 36、
build-tools 35.0.0 以及 platform-tools。

在 POSIX 系统上，检出代码后请确保仓库内的包装脚本具有可执行权限；某些本地
检出方式不会保留 Git 文件权限：

```bash
chmod +x android/gradlew
android/gradlew --no-daemon -p android testDebugUnitTest assembleDebug assembleRelease
```

如果没有设置签名环境变量，Release APK 会按预期收集为
`PadDrawBoard-0.1.0-android-release-unsigned.apk`；Debug APK 使用开发调试密钥。
二者都不是受信任的公开发布签名。本地当前生成的产物未签名。

## CI 覆盖范围

GitHub Actions 会运行原生 CTest、协议 Python 一致性测试、协议 Kotlin/JVM 测试、
延迟单元测试和两个延迟夹具门槛测试、Android 测试，以及 Debug/Release APK 构建。
随后，Windows 任务会获取固定版本的 ADB 包，构建 x64 ZIP 和 NSIS 安装程序，生成
校验和清单并上传两个产物。只有在所有受保护的 Android 签名密钥都存在时，Android
任务才会上传明确标记为 `signed` 的产物；否则只上传明确标记为未签名的产物。因此，
来自 Fork 和 Pull Request 的运行无需受信任的签名凭据也能正常工作。

## 签名

仓库和默认 CI 不会假装产物已经完成代码签名：Windows 软件包未签名，Debug APK
使用 Gradle 调试密钥，Release APK 未签名。要进行受信任的公开发布，需要受保护的
Windows Authenticode 证书和受保护的 Android Release 密钥库。

Android Release 签名是可选的，并且独立于本仓库。维护者可以在受信任的仓库中为
GitHub Actions 配置以下受保护的密钥（不要在 Fork 中配置）：
`PDB_ANDROID_KEYSTORE_BASE64`、`PDB_ANDROID_KEYSTORE_PASSWORD`、
`PDB_ANDROID_KEY_ALIAS` 和 `PDB_ANDROID_KEY_PASSWORD`。工作流只会将 Base64 密钥库
解码到运行器临时文件，为 Gradle 设置 `PDB_ANDROID_KEYSTORE_FILE`，构建 Release
APK，使用 Android SDK 的 `apksigner` 验证，并检查包名 `io.paddrawboard.client`，
最后在 `always()` 清理步骤中删除密钥库。四个密钥必须同时存在；如果全部缺失，
Release 将保持未签名状态。部分配置会直接失败。密钥绝不能放入本仓库或上传的
未签名产物中。

## 干净检出

使用全新克隆验证：项目不依赖任何已生成的构建目录、已下载的压缩包、APK、安装程序
或本地签名密钥：

```powershell
git clone <repository-url> PadDrawBoard-clean
Set-Location PadDrawBoard-clean
git status --short
cmake -S . -B build -A x64 -DPDB_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

构建前预期的干净状态是没有任何输出。`build/`、`.build-*`、`artifacts/` 和打包
下载工作区等生成目录均会被忽略。下载的 Platform Tools 压缩包和二进制文件只能
作为构建输入使用，不得加入源代码管理。

## 许可证

版权所有 (c) 2026 PadDrawBoard 贡献者。本项目采用 GPL-3.0-only 许可证。详见
`LICENSE`。
