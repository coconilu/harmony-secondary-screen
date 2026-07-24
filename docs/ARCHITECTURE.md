# 系统架构

## 产品目标

让 Windows 11 用户把自己明确选择的 Microsoft Edge 标签页画面发送到 HarmonyOS 平板，作为独立的
网页伴随屏观看；声音继续由 Windows 输出到现有耳机。

产品不向 Windows 枚举显示器，不支持窗口跨屏拖动，不捕获整个桌面，也不把镜像称为扩展屏。
HarmonyOS 接收端必须是原生应用。

## 组件边界

| 组件 | 职责 | v0.1 技术方向 |
| --- | --- | --- |
| Edge Extension | 接收用户手势、选择当前标签页、展示连接状态 | Manifest V3 + `activeTab` + `tabCapture` |
| Capture Worker | 持有媒体流、缩放并编码视频 | offscreen document + WebCodecs `VideoEncoder` |
| Local Bridge | 在扩展与 Relay 间传输控制消息和二进制编码帧 | Native Messaging 启动/授权 + 随机令牌保护的回环 WebSocket |
| Windows Relay | 配对、关键帧转发、UDP 分片与工程日志 | 普通用户态进程；不注册系统服务、不要求管理员 |
| LAN Transport | 可信局域网内的控制和视频 | Relay 主动连接平板 TCP 44000；向平板 UDP 47101 发送视频 |
| Harmony Receiver | 展示配对信息、收包、解码、渲染和状态展示 | ArkTS + NDK AVCodec + XComponent |

## 数据流

```text
用户点击 Edge 扩展按钮
   ↓ chrome.tabCapture.getMediaStreamId（只请求 video）
MV3 offscreen document
   ↓ MediaStreamTrackProcessor / VideoFrame
WebCodecs VideoEncoder
   ↓ H.264 Annex-B EncodedVideoChunk
回环 Local Bridge（127.0.0.1 + 临时令牌）
   ↓
Windows Relay（普通用户进程）
   ↓ UDP 受限分片
HarmonyOS Receiver
   ↓ H.264 Annex-B
OH_VideoDecoder
   ↓ Surface
XComponent
```

控制流反向返回关键帧请求、停止原因和遥测：

```text
Harmony Receiver → TCP control → Windows Relay
  → Local Bridge → offscreen document → VideoEncoder key frame
```

## 架构决策

### 捕获标签页，而不是窗口像素

Deskreen 实验验证了通用投屏链路，但最小化 Edge 后窗口停止绘制。v0.1 使用 Edge 官方支持的
`tabCapture` 获取用户主动选择的标签页媒体流，目标是让捕获生命周期不依赖窗口遮挡状态。

“Edge 窗口最小化后仍持续出帧”目前仍是 PoC 门禁，不因为 API 存在就视为已通过。

### 不采集音频

扩展只请求视频轨，不获取音频轨。B 站等网页的声音继续走 Edge 原有本地输出设备，项目不编码、
发送或在平板播放音频。这样满足“耳机只连接 PC”的核心需求，也避免双端回声。

PC 本地音频与平板视频之间可能存在可感知偏移，必须实测并记录；不能用单独的视频延迟指标替代。

### H.264 能力必须探测

WebCodecs 允许通过 `VideoEncoder.isConfigSupported()` 探测具体编码配置，AVC 注册允许
`avc.format = "annexb"`。但标准不要求浏览器一定实现 H.264 编码。

v0.1 在开始会话前探测至少 `1280×720 @ 30 fps` 的 H.264 Annex-B 配置。若不支持，扩展必须明确
展示“不支持当前编码能力”，不得静默改成 VP8/VP9 后让 HarmonyOS 端黑屏。`1920×1080 @ 30 fps`
作为增强能力协商，不是首个垂直切片的硬门禁。

### 保留轻量 Relay，不把 libwebrtc 引入平板

浏览器扩展不能直接发送任意 UDP。直接采用 WebRTC 会要求 HarmonyOS 原生端集成和维护 libwebrtc，
其体积、ABI、构建和升级成本与当前轻量目标不符。

v0.1 使用普通用户态 Relay：

- Native Messaging 只负责启动 Relay、交换临时端口和随机令牌；
- 视频块通过只绑定 `127.0.0.1` 的二进制 WebSocket 传输，避免 Base64/JSON 成为主数据面；
- Relay 主动连接平板，不开放 Windows 局域网入站端口；
- Relay 不安装为服务，不需要驱动签名或管理员权限。

若回环 WebSocket PoC 不满足性能要求，只替换本地桥接层，不改变 Edge 捕获、局域网协议和 HarmonyOS
接收端边界。

### 原生 HarmonyOS 接收端

接收端不使用 WebView、HTML5 Viewer 或 Android 兼容应用。ArkTS 负责生命周期、可信 Wi-Fi 确认、
配对信息和状态；TCP/UDP、抖动缓冲和 AVCodec 解码由 C++ 完成。

接收端只在用户明确确认的 Wi-Fi IPv4 上监听，不绑定蜂窝、VPN 或通配地址。运行时依据 API、编解码
能力、Surface 与网络条件协商，不依据设备型号分支。

### 迁移旧原型

旧 IddCx 原型中的 H.264 UDP 分片、协议边界检查、AVCodec 解码器状态机和测试思路可以复用。以下
组件不再属于产品：

- `host/driver` IddCx 驱动；
- D3D11 / Media Foundation 桌面帧编码管线；
- Session 0 Host Service 与服务安装器；
- Input Agent、`SendInput` 和触控坐标映射；
- Windows 入站防火墙规则。

具体处置见 [ADR-001](ADR-001-WEB-COMPANION-PIVOT.md)。

## 非目标

- Windows 真扩展屏、虚拟显示器或窗口跨屏拖动；
- 捕获整个桌面、任意应用窗口或远程控制 PC；
- 音频传输、平板扬声器播放或多音频设备同步；
- DRM/受保护视频捕获或绕过站点保护；
- 公网、访客 Wi-Fi、端口转发或云中继；
- USB 视频传输、多平板、HDR、4K、触控/键鼠回传；
- Windows 10、macOS、Android 或 iPadOS。
