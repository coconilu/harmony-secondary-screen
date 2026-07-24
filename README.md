# Harmony Web Companion

把用户明确选择的 Microsoft Edge 标签页画面发送到 HarmonyOS 6.1+ 平板，作为“网页伴随屏”使用；
音频继续由 Windows 输出到用户原有耳机。

本项目不再把平板伪装成 Windows 显示器，也不再追求窗口跨屏拖动。它不是扩展屏、远程桌面或通用
投屏工具。

## v0.1 产品边界

| 端 | 目标实现 |
| --- | --- |
| Edge | Manifest V3 扩展；用户点击扩展按钮后，仅捕获当前标签页的视频轨 |
| 后台媒体处理 | `tabCapture` + offscreen document；WebCodecs H.264 能力运行时探测 |
| Windows Relay | 普通用户态进程；接收扩展编码帧并发送到平板，不安装驱动或系统服务 |
| 局域网 | 手动填写平板 IPv4；TCP 44000 控制、UDP 47101 视频；只面向用户确认的可信 Wi-Fi |
| HarmonyOS | ArkTS 控制界面 + C++ NDK AVCodec + 原生 XComponent Surface |
| 音频 | 不采集、不传输；继续从 PC 耳机或扬声器输出 |
| 安全 | 平板显示六位一次性配对码；随机 128-bit 会话；断连后仅保留 5 秒恢复窗口 |

平板端没有 WebView、HTML5 接收页或 Android APK 兼容层。Windows 端没有 IddCx、内核驱动、测试
模式、驱动签名、管理员安装或输入注入。

## 为什么转向

Deskreen 实机体验已经验证了两个产品假设：Windows 页面可以经局域网显示在平板上，声音可以留在
PC。它同时暴露了“捕获窗口像素”的限制：Edge 窗口最小化后可能停止绘制，平板只能看到最后一帧。

新方向改为捕获 Edge 标签页媒体流，不再依赖 Windows 窗口是否可见。**这只是已选技术假设，尚未
完成本项目的端到端验证。**

## 当前状态

| 项目 | 状态 |
| --- | --- |
| 网页伴随屏的用户价值与通用局域网投屏链路 | 已通过 Deskreen 外部实验验证 |
| Edge `tabCapture` / MV3 offscreen API 可用性 | 真实 Edge 连续会话已通过 13 分 24 秒短时基线；四阶段为人工观察，尚无独立阶段遥测 |
| Edge 窗口最小化后持续出帧 | 短时真机观察通过，完整四阶段各 10 分钟仍待验收 |
| WebCodecs H.264 Annex-B 编码 | 真机以 `avc1.42001f`、1280×720 编码 21,778 帧，编码错误为 0 |
| 普通用户态 Relay | 真机连续发送 21,778 帧，Relay 与局域网发送错误为 0 |
| HarmonyOS 原生解码/显示 | 真机解码 21,763 帧，短时接收端丢帧率 0.0643%，四种窗口状态人工观察正常 |
| 30 分钟稳定性、延迟和音画偏移 | 尚无实测结果 |

仓库现有 `host/driver`、Media Foundation 编码器、输入代理和协议 v1 实现属于旧的 IddCx 扩展屏
原型。它们暂时保留用于迁移取证，但不再代表 v0.1 产品，也不能用其构建通过声称新链路可用。迁移
边界见 [ADR-001](docs/ADR-001-WEB-COMPANION-PIVOT.md)。

## 目标运行流程

```text
平板启动 HarmonyOS 原生 Receiver
        ↓ 只在用户确认的可信 Wi-Fi IPv4 上监听
平板显示自身 IPv4 与六位一次性配对码
        ↓
用户打开 Edge 视频页并点击扩展按钮
        ↓ 选择“发送当前标签页”，输入平板 IPv4 与配对码
Edge tabCapture（仅视频）→ offscreen document → WebCodecs H.264
        ↓ 本机受限 Relay 通道
普通用户态 Relay → TCP 配对/控制 + UDP H.264
        ↓
HarmonyOS AVCodec → XComponent

PC 上的 Edge 音频轨未被捕获，继续输出到原有耳机。
```

## v0.1 首个垂直切片

1. 扩展通过一次用户点击捕获当前标签页，仅请求 `activeTab`、`tabCapture`、`offscreen` 和
   `nativeMessaging` 等必要权限。
2. offscreen document 探测并使用 H.264 Annex-B 编码；能力不满足时明确报错，不静默换成平板
   无法解码的格式。
3. Relay 以普通用户身份运行，经只限回环地址的本地通道接收编码帧，再主动连接平板。
4. 平板原生 Receiver 完成配对、UDP 重组、AVCodec 解码和 XComponent 显示。
5. Edge 窗口最小化、被其他应用覆盖或标签页转入后台后，平板视频仍持续前进，PC 音频不中断。

验收方法见 [真实设备测试](docs/REAL_DEVICE_TEST.md)，协议草案见
[传输协议](docs/PROTOCOL.md)。

### 当前可运行的 Edge → Relay → HarmonyOS 垂直切片

`extension/` 与 `relay/` 已实现连续链路候选：捕获当前标签页视频轨，通过
`MediaStreamTrackProcessor` 统计帧连续性，并用 WebCodecs 探测和执行
`1280×720 @ 30 fps` H.264 Annex-B 编码，再经 Native Messaging 授权的随机回环 WebSocket 把
二进制编码块交给普通用户态 Relay。Relay 使用平板一次性码完成协议 v2 配对，把 Access Unit 拆成
HSS2 UDP 数据报，并把平板关键帧请求反馈给编码器。自动化假 Receiver 已逐字节验证重组结果；真实
Edge 到平板连续画面已完成 13 分 24 秒短时基线，详细证据与未完成项见
[2026-07-24 真机测试报告](docs/test-results/2026-07-24-continuous-playback.md)。

构建/安装 Relay 见 [relay/README.md](relay/README.md)，扩展四阶段测试见
[extension/README.md](extension/README.md)。

```powershell
.\scripts\build-relay.ps1
```

### 当前可运行的 Windows → HarmonyOS 首帧冒烟测试

该测试发送一个由 `ffmpeg` 生成的 H.264 Annex-B 关键帧，只验证 HSS2 配对、UDP 分片、原生 AVCodec
解码与 XComponent 显示；不验证连续视频、Edge 最小化或音画同步。

```powershell
.\scripts\send-receiver-smoke.ps1 `
  -ReceiverAddress <平板界面确认的Wi-Fi-IPv4> `
  -PairingCode <平板显示的六位码>
```

步骤与判定见 [真实设备测试](docs/REAL_DEVICE_TEST.md)。

## 旧原型的可重复检查

在迁移完成前，下列命令只验证旧原型仍可构建，不能作为网页伴随屏验收证据：

```powershell
.\scripts\test.ps1 -Configuration Release
```

旧 Windows Host、IddCx 驱动和安装脚本不再属于目标安装路径。迁移工作不得为了让旧静态检查继续
通过而保留已经失去产品职责的组件。

## HarmonyOS Receiver 构建

要求：DevEco Studio 6.1.1 Release SDK 或兼容版本，以及 HarmonyOS/OpenHarmony Native SDK。

```powershell
.\scripts\build-receiver.ps1
```

开发者签名只用于把 HAP 安装到真实平板。仓库不得提交证书、私钥、口令或带本机绝对路径的签名配置。

## 文档

- [系统架构](docs/ARCHITECTURE.md)
- [ADR-001：转向网页伴随屏](docs/ADR-001-WEB-COMPANION-PIVOT.md)
- [传输协议](docs/PROTOCOL.md)
- [真实设备测试](docs/REAL_DEVICE_TEST.md)
- [兼容性策略](docs/COMPATIBILITY.md)
- [v0.1 里程碑](docs/V0_1_MILESTONE.md)

## 参考

- [Microsoft Edge 扩展 API 支持表](https://learn.microsoft.com/microsoft-edge/extensions/developer-guide/api-support)
- [Chrome `tabCapture` API](https://developer.chrome.com/docs/extensions/reference/api/tabCapture)
- [Chrome offscreen document API](https://developer.chrome.com/docs/extensions/reference/api/offscreen)
- [WebCodecs](https://www.w3.org/TR/webcodecs/)
- [WebCodecs AVC/H.264 注册](https://www.w3.org/TR/webcodecs-avc-codec-registration/)

## 许可证

Apache-2.0
