# 兼容性策略

## 原则

产品不按设备型号或网站名称判断功能，而是按运行时能力协商：

- Microsoft Edge 扩展 API 与版本；
- `tabCapture`、offscreen document、MediaStreamTrackProcessor 和 WebCodecs 可用性；
- H.264 Annex-B `VideoEncoder.isConfigSupported()` 结果；
- HarmonyOS API、`video/avc` 硬件解码和 Surface 输出能力；
- 屏幕尺寸、Wi-Fi 吞吐、抖动和丢包率。

型号、GPU 和网站只用于记录可重复的测试证据，不得成为业务分支。

## 首个验证基线

| 项目 | 值 |
| --- | --- |
| PC | Windows 11 x64 |
| 浏览器 | Microsoft Edge 当前稳定版 |
| 捕获 | 用户主动选择的单个标签页，仅视频 |
| 基线编码 | H.264 Annex-B `1280×720 @ 30 fps` |
| 平板 | HUAWEI MatePad Pro；型号作为首个测试样本 |
| 系统 | HarmonyOS 6.1.0 / API 23 |
| 网络 | 同一可信家庭 5 GHz Wi-Fi |
| 音频 | Windows 原输出设备，不传输到平板 |

## 初始支持范围

- Windows 11 x64 + Microsoft Edge Manifest V3；
- `tabCapture` 和 offscreen API 可用；
- H.264 Annex-B 720p30 编码探测通过；
- HarmonyOS 6.1.0 / API 23 及以上；
- 平板支持 H.264 Surface 硬件解码；
- PC 与平板位于同一用户确认的可信局域网。

增强能力 `1920×1080 @ 30 fps` 必须双方探测通过后协商，不能仅因某个 GPU 或平板宣称支持就默认
启用。

## 页面兼容性

| 页面类型 | v0.1 立场 |
| --- | --- |
| 普通 HTML5 视频 | 目标支持 |
| Canvas/WebGL 动画 | 记录实测，不做首版承诺 |
| 受保护 DRM/EME 视频 | 明确不支持，不绕过 |
| 浏览器内部页、扩展页、商店页 | 受 Edge 安全限制，不支持 |
| 整个桌面或其他应用 | 不支持 |

B 站只是首个真实样本。代码不得根据 `bilibili.com` 或播放器 DOM 写专用捕获路径。

## 网络与 VPN

Receiver 只绑定用户确认的 Wi-Fi IPv4。Relay 主动连接平板，不监听局域网地址，不修改 Windows 的
公用/专用分类、VPN、系统代理或路由。

VPN 的“阻止局域网”或 Kill Switch 仍可能拦截平板流量。此时应在 VPN 客户端中允许本地网络访问，
不能通过放宽 Receiver 到通配地址、关闭防火墙或修改系统路由来绕过。

## 不作推断

- Edge 文档列出 API 不等于目标页面在最小化状态一定持续出帧；
- WebCodecs 标准登记 H.264 不等于每台 PC 都实现 H.264 编码；
- Receiver 编译成功不等于对应平板完成硬件解码；
- Deskreen 可用不等于本项目的原生 Receiver、协议 v2 或音画偏移已通过；
- 扩大到更低 HarmonyOS、其他浏览器或其他系统必须有独立构建和实机证据。
