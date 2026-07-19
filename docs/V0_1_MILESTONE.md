# v0.1：原生 Wi-Fi 扩展屏 MVP

## Problem

非华为 Windows 11 电脑无法通过华为官方多屏协同把纯血 HarmonyOS 平板用作扩展屏。现有浏览器或 Android APK 方案不符合原生 HarmonyOS 产品目标。

## Desired outcome

用户在同一可信家庭 Wi-Fi 中启动 Windows Host 和 HarmonyOS Receiver 后，Windows 出现一块可用的虚拟显示器；窗口可以拖入平板，平板原生应用稳定显示画面并回传基础触控。

## Scope

- HarmonyOS 原生接收端：ArkTS 控制界面、C++ AVCodec H.264 解码、XComponent Surface 渲染。
- Windows 11 Host：IddCx 虚拟显示器、D3D11 帧处理、Media Foundation H.264 硬件编码。
- 局域网连接：手动 IP 作为保底，mDNS 发现作为目标；TCP 控制和 UDP 视频。
- 基础触控回传：单指点击、拖动和滚动。
- 首个实机基线：XYAO-W00 / HarmonyOS 6.1.0。

## Acceptance criteria

- [ ] Windows 11“显示设置”出现独立显示器，可选择“扩展这些显示器”。
- [ ] Windows 主屏与平板显示不同内容，窗口可跨屏拖动，不以镜像或远程桌面冒充扩展屏。
- [ ] HarmonyOS 接收端不包含 WebView/HTML5，不依赖 Android APK。
- [ ] 1920×1200、60 fps 配置可以建立会话；连续 30 分钟无崩溃、无永久黑屏、无人工重连。
- [ ] 家庭 5 GHz Wi-Fi 下，交互场景端到端延迟 P95 不高于 100 ms；测量方法和原始结果写入仓库。
- [ ] 30 分钟测试期间丢帧率低于 1%，断连后 5 秒内自动恢复。
- [ ] 单指点击与拖动位置误差不超过扩展屏宽高的 1%；滚动方向正确。
- [ ] Windows 服务仅监听专用网络；公共网络默认拒绝；首次连接需要一次性配对码。
- [ ] 实机测试记录包含设备型号、HarmonyOS/API、分辨率、Wi-Fi 频段、平均码率、P50/P95 延迟和异常日志。
- [ ] 接收端 HAP 和 Windows Host 有可重复构建命令，干净环境构建步骤写入 README。

## Non-goals

- USB 视频传输；
- 公网远程连接；
- 音频、HDR、DRM 视频、手写笔压感、多点触控；
- 多平板同时连接；
- HarmonyOS 6.1.0 以下版本的兼容承诺；
- Windows 10、macOS、Android 或 iPadOS。

## Dependencies and risks

- Windows IddCx 驱动安装与签名会影响首次运行体验。
- 不同 GPU 的 Media Foundation 编码器行为可能不同，需要软件编码降级策略。
- 桌面静态内容与快速滚动的码率、清晰度目标冲突，需要桌面场景调参。
- UDP 丢包可能导致整帧丢弃，必须控制关键帧间隔并提供快速恢复。
- 受保护内容可能无法捕获，不属于 v0.1 缺陷。

## Evidence

- `docs/ARCHITECTURE.md`
- `docs/PROTOCOL.md`
- `docs/COMPATIBILITY.md`
- Microsoft IddCx documentation
- HarmonyOS AVCodec and XComponent documentation
