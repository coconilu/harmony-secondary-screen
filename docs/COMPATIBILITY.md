# 兼容性策略

## 按能力，不按型号或网站

| 层 | 运行时门禁 |
| --- | --- |
| Edge | MV3、`tabCapture`、offscreen、MediaStreamTrackProcessor、WebCodecs |
| 编码 | H.264 `avc1.42001f`、Annex-B、1280×720 @ 30 fps |
| 局域网 | Edge Local Network Access、WebSocket、私网 Wi-Fi |
| HarmonyOS | API 23+、ScanKit、NetworkKit mDNS、Preferences、AVCodec、XComponent |

型号、GPU、网站和 AP 只进入测试记录，不得成为业务分支。

## 地址与 Local Network Access

扩展持久 host permission 仅包含 `http://harmony-web-companion.local/*`。手动 IP 回退使用
`http://*/*` 的 optional host 声明，但只在用户输入并确认具体私网 IPv4 后请求该单一 origin。

Edge 143+ 的 Local Network Access 行为仍可能变化，必须在目标 Edge 版本验证 WebSocket。不得用
`<all_urls>` 或网络扫描规避。

HarmonyOS DNS-SD 服务实例注册不等于裸 `.local` 主机名解析。兼容性记录必须分别报告：

1. DNS-SD 注册是否成功；
2. Windows 是否能解析 `harmony-web-companion.local`；
3. Edge 是否允许建立对应 WebSocket；
4. 手动私网 IPv4 是否成功。

## 页面范围

| 页面类型 | v0.1 |
| --- | --- |
| 普通 HTTP/HTTPS 页面和非 DRM 视频 | 目标支持 |
| Canvas/WebGL | 记录实测 |
| DRM/EME | 不支持，不绕过 |
| 浏览器内部页、商店页、扩展页 | 不支持 |
| 桌面或其他应用 | 不支持 |

音频始终留在 PC。

## 网络限制

Receiver 仅允许用户确认的 `wlan*` 私网或 IPv4 link-local 地址；拒绝通配、回环、VPN、蜂窝和公网。
VPN 的“阻止局域网”可能阻止连接，产品不修改 VPN、代理、路由、防火墙或 Windows 网络分类。

## 不能由自动化推断的结论

- Receiver 构建成功不等于目标平板扫码或 AVCodec 显示成功；
- DNS-SD API 成功不等于裸 `.local` 可解析；
- 假 Receiver 收到 Annex-B 不等于真实 Edge/平板端到端通过；
- 旧 Relay 的 13 分 24 秒结果不等于 HWC3 直连链路通过；
- 短时画面不等于 #3 的 30 分钟、延迟、丢包和恢复验收。
