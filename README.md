# Harmony Secondary Screen

把 HarmonyOS 6.1+ 平板作为 Windows 11 的原生 Wi-Fi 扩展屏。

## 实现边界

| 端 | v0.1 实现 |
| --- | --- |
| Windows 虚拟显示 | UMDF 2 + IddCx，枚举独立的 `1920×1200 @ 60 Hz` 显示器 |
| Windows 帧管线 | IddCx D3D11 Surface → 深度为 2 的低延迟队列 → D3D11 VideoProcessor NV12 |
| Windows 编码 | Media Foundation H.264，硬件 MFT 优先，软件 MFT 回退，无 B 帧、1 秒 GOP |
| 局域网 | 手动 IPv4；TCP 47100 控制、UDP 47101 视频；只绑定 Windows“专用”网络 |
| HarmonyOS | ArkTS 控制界面 + C++ NDK AVCodec + 原生 XComponent Surface |
| 输入 | 单指点击/拖动；显式“滚动模式”回传滚轮事件 |
| 会话安全 | 六位一次性配对码；随机 128-bit 会话；断连后仅保留 5 秒恢复窗口 |

没有 WebView、HTML5 接收页、Android APK 兼容层、镜像或远程桌面替代路径。

## 当前验证状态

| 检查 | 本仓库当前结果 |
| --- | --- |
| Windows Host Service / Input Agent / D3D11 / MF 源码 | 本机 MSVC `/W4 /WX` 构建通过 |
| 协议单元测试与 Host/Receiver 线格式兼容测试 | 通过 |
| HarmonyOS Receiver | DevEco CLI 原生 CMake/Ninja + ArkTS + unsigned HAP 构建通过 |
| IddCx 驱动构建/安装 | 本机缺 Windows Driver Kit，**未运行、未声称通过** |
| Windows 显示设置出现独立屏幕 | 需要 WDK、测试签名与 Windows 11 测试机，尚未验证 |
| 30 分钟、P95 延迟、丢帧与触控精度 | 需要真实 XYAO-W00 / 5 GHz Wi-Fi，尚无测量结果 |

源码可审查和可重复构建不等于 Issue #1 的实机验收已经完成。实机门禁见
[真实设备测试](docs/REAL_DEVICE_TEST.md)。

## 干净环境构建

### Windows Host

要求：Windows 11 x64、Visual Studio 2022 C++ Build Tools、CMake、Windows 11 SDK，以及与
Visual Studio 集成的 Windows Driver Kit（需包含 UMDF 2 / IddCx）。

```powershell
.\scripts\build-host.ps1 -Configuration Release
```

该命令先构建用户态 Host、D3D11/MF 编码库并运行 CTest，然后构建 IddCx 驱动。缺少 WDK 时会
明确失败，不能使用 `-SkipDriver` 的结果冒充完整 Host 构建。

只验证本机可用的用户态组件：

```powershell
.\scripts\build-host.ps1 -Configuration Release -SkipDriver
```

用户态产物：`out\host\Release\hss_host.exe`、`out\host\Release\hss_input_agent.exe`。驱动项目：
`host\driver\HssIddDriver.vcxproj`。

测试驱动必须在隔离的 Windows 11 测试机完成测试签名和安装；正式分发必须使用正式驱动签名。
启动 Host 前把家庭 Wi-Fi 的 Windows 网络配置文件设为“专用”。如果只有公共网络，Host 会拒绝监听，
不会回退到 `0.0.0.0`。

### HarmonyOS Receiver

要求：DevEco Studio 6.1.1 Release SDK 或兼容版本，HarmonyOS/OpenHarmony Native SDK。

```powershell
.\scripts\build-receiver.ps1
```

unsigned HAP 位于 `receiver\entry\build\default\outputs\default\`。真机安装前需要在 DevEco 中配置
开发者签名；仓库不提交证书、私钥或签名产物。

### 一键运行本机可执行检查

```powershell
.\scripts\test.ps1 -Configuration Release
```

该命令执行原生架构/禁用依赖静态检查、Host 编译与 CTest、Receiver 全量构建。IddCx 安装和实机
测试是独立门禁，不包含在这个命令里。

## 运行流程

```text
安装并启动 IddCx 驱动
        ↓ Windows 出现 Harmony Secondary Screen
管理员安装并启动 HarmonySecondaryScreenHost 服务
        ↓ 仅监听“专用”网络
当前登录用户启动 hss_input_agent.exe
        ↓ 接收服务转发的已鉴权触控
运行 hss_host.exe --pairing-code
        ↓ 输出六位一次性配对码
平板输入 Host 的专用网络 IPv4 与配对码
        ↓ TCP 鉴权与 1920×1200@60 协商
窗口拖入扩展屏 → H.264/UDP → AVCodec/XComponent
        ↑ 单指指针或滚动事件经 TCP 回传
```

Host 在 `%ProgramData%\HarmonySecondaryScreen\` 写入 `hss-host-frames.csv` 与
`hss-receiver-telemetry.csv`。后者使用四时间戳时钟
偏移估算，仅用于工程诊断；验收 P95 以真实设备测试文档中的外部测量方法为准。

服务安装和本机查询命令：

```powershell
# 管理员 PowerShell；默认只安装为“手动启动”，不会偷偷修改网络配置文件
.\scripts\install-host-service.ps1 -Configuration Release -Start

# 普通用户会话
.\out\host\Release\hss_input_agent.exe
.\out\host\Release\hss_host.exe --pairing-code

# 卸载（管理员 PowerShell）
.\scripts\install-host-service.ps1 -Uninstall
```

开发调试时可以用 `hss_host.exe --console` 代替 SCM 服务；这不是部署验收路径。

## 文档

- [系统架构](docs/ARCHITECTURE.md)
- [传输协议](docs/PROTOCOL.md)
- [真实设备测试](docs/REAL_DEVICE_TEST.md)
- [兼容性策略](docs/COMPATIBILITY.md)
- [v0.1 里程碑](docs/V0_1_MILESTONE.md)

## 许可证

Apache-2.0
