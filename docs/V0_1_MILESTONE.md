# v0.1：HarmonyOS 网页伴随屏 MVP

## Problem

耳机同时连接 Windows PC 与平板时需要频繁切换。用户实际需要的不是完整 Windows 扩展屏，而是：
工作时继续使用 PC 和 PC 耳机，把当前 Edge 视频标签页的画面放到 HarmonyOS 平板。

## Desired outcome

Windows 11 与 HarmonyOS 平板处于同一可信 Wi-Fi 时，用户点击 Edge 扩展按钮并输入平板显示的一次性
配对码，即可把当前标签页视频持续显示在平板原生应用中。Edge 窗口可以最小化或被其他应用覆盖，
声音继续从 PC 原有输出设备播放。

## Scope

- Edge Manifest V3 扩展：用户手势触发 `tabCapture`，offscreen document 持有视频流。
- WebCodecs H.264 Annex-B 能力探测与 `1280×720 @ 30 fps` 基线编码。
- 普通用户态 Windows Relay：本地受限桥接、TCP 控制、UDP 视频和工程日志。
- HarmonyOS 原生 Receiver：ArkTS 状态界面、C++ 收包、AVCodec 解码、XComponent 渲染。
- 手动平板 IPv4 与六位一次性配对码；只支持用户确认的可信局域网。
- 单个 Edge 标签页、单台平板、单个会话。

## Acceptance criteria

- [ ] 扩展只在用户点击后捕获当前活动标签页；停止后释放所有媒体轨和连接。
- [ ] 扩展权限不包含无必要的 `<all_urls>`；不读取 Cookie、历史记录或页面正文。
- [ ] 捕获只包含视频轨；会话开始、运行和停止期间，Edge 音频始终从原 PC 输出设备播放。
- [ ] Windows 不安装 IddCx/内核驱动，不启用测试模式，不注册系统服务，不需要管理员权限。
- [ ] HarmonyOS 接收端不包含 WebView/HTML5，不依赖 Android APK。
- [ ] `VideoEncoder.isConfigSupported()` 通过后可建立 `1280×720 @ 30 fps` H.264 Annex-B 会话；
  不支持时展示可理解的错误，不发送错误编码格式。
- [ ] Edge 窗口依次处于前台、被完全覆盖、最小化、标签页后台四种状态各 10 分钟，平板视频持续
  前进，无永久定格或人工重连。
- [ ] 使用代表性普通 HTML5 视频页连续播放 30 分钟，无崩溃、永久黑屏或人工重连；DRM 内容单独
  标记为不支持。
- [ ] 可信 5 GHz Wi-Fi 下，捕获到平板显示的端到端延迟 P95 暂定不高于 250 ms；报告 PC 音频与
  平板视频的实测偏移，并由用户确认是否影响观看。
- [ ] 30 分钟测试期间接收端丢帧率低于 1%；1 秒网络中断恢复后 5 秒内重新显示移动画面。
- [ ] Receiver 只监听用户确认的 Wi-Fi IPv4；Relay 只主动连接该地址，不开放 Windows 局域网入站
  端口，不修改 Windows 网络分类或 VPN。
- [ ] 实机记录包含 Edge 版本、GPU、WebCodecs 配置、Relay 版本、设备型号、HarmonyOS/API、Wi-Fi、
  码率、帧率、P50/P95 延迟、音画偏移和异常日志。
- [ ] 扩展、Relay 与 Receiver 均有可重复构建和安装说明，且不提交签名证书、私钥或口令。

## Non-goals

- Windows 扩展屏、虚拟显示器、桌面镜像或窗口跨屏拖动；
- 任意桌面窗口捕获、远程桌面或触控/键鼠回传；
- 音频传输、平板扬声器播放或自动音画同步；
- DRM/受保护视频绕过；
- 公网、访客 Wi-Fi、云中继、端口转发；
- USB、多平板、HDR、4K、60 fps；
- Chrome/Firefox、Windows 10、macOS、Android 或 iPadOS；
- HarmonyOS 6.1.0 以下版本的兼容承诺。

## Dependencies and risks

- Edge `tabCapture` 与 offscreen API 的后台/最小化行为已通过 13 分 24 秒真机短时基线；四种状态
  各 10 分钟的正式验收仍未完成。
- WebCodecs 标准允许 H.264，浏览器实现并非强制；必须保留能力失败路径。
- 回环 WebSocket 与 Native Messaging 的吞吐、CPU 和生命周期需要测量。
- 本地音频领先于平板视频会影响口型体验，产品是否可接受必须由真实观看验证。
- Edge 扩展生产分发需要商店审核或受控侧载；它不是 Windows 驱动签名问题。
- 受保护内容可能拒绝捕获或输出黑帧，不属于 v0.1 缺陷。

## Evidence

- `docs/ADR-001-WEB-COMPANION-PIVOT.md`
- `docs/ARCHITECTURE.md`
- `docs/PROTOCOL.md`
- `docs/COMPATIBILITY.md`
- `docs/test-results/2026-07-24-continuous-playback.md`
- [Issue #3：正式稳定性与体验验收](https://github.com/coconilu/harmony-secondary-screen/issues/3)
- [Microsoft Edge 扩展 API 支持表](https://learn.microsoft.com/microsoft-edge/extensions/developer-guide/api-support)
- [Chrome `tabCapture` API](https://developer.chrome.com/docs/extensions/reference/api/tabCapture)
- [Chrome offscreen document API](https://developer.chrome.com/docs/extensions/reference/api/offscreen)
- [WebCodecs](https://www.w3.org/TR/webcodecs/)
- [WebCodecs AVC/H.264 注册](https://www.w3.org/TR/webcodecs-avc-codec-registration/)
