# v0.1：HarmonyOS 网页伴随屏 MVP

## Outcome

Windows 11 与 HarmonyOS 平板处于同一可信 Wi-Fi 时，用户首次由平板扫描 Edge 扩展二维码授权；
以后只需在目标标签页点击“发送当前标签页”。声音继续从 PC 输出。

## Scope

- Edge MV3 `tabCapture` + offscreen + WebCodecs H.264 Annex-B；
- Edge 直连 HarmonyOS Receiver HWC4 WebSocket；
- HarmonyOS 原生 ArkTS + C++ AVCodec + XComponent；
- 60 秒一次性 QR，短码和手动私网 IPv4 回退；
- deviceId/credential 持久信任，IP 与身份分离；
- 单个活动标签页、编码器和媒体流；
- 可信局域网，不传音频。

普通用户只安装 Receiver 与 Edge 扩展，不安装 Windows Relay、Native Host、服务或驱动。

## Acceptance

- [x] 扩展权限移除 `nativeMessaging`，不增加 `<all_urls>`、正文读取或注入。
- [x] Annex-B binary message 到假 Receiver 逐字节自动化验证。
- [x] Receiver 绑定和入站策略拒绝通配、回环、VPN、蜂窝和公网。
- [x] 60 秒一次性授权、身份校验、持久凭据与两端忘记设备实现。
- [x] 手动私网 IP、短码和 source epoch 回退/竞态测试实现。
- [x] Relay 不属于正常构建、安装或运行路径。
- [x] 仓库签名配置移除口令和本机路径。
- [x] 扩展测试、Receiver 协议测试、静态检查与 targetSdk 24 构建通过。
- [ ] 真实 Edge + HarmonyOS：扫码、直连、首帧、IP 变化重连、手动 IP。
- [ ] #3：四阶段、30 分钟、P50/P95、音画偏移、丢包恢复和隐私验收。

## Risks

- DNS-SD 服务注册不能证明裸 `harmony-web-companion.local` 一定可解析；
- Edge Local Network Access 对 WebSocket 的版本行为需真机验证；
- WebSocket/TCP 在丢包时可能出现队头阻塞；
- `ws://` 只适用于可信 LAN，不支持公网或对抗同网段窃听。

验收步骤见 [REAL_DEVICE_TEST.md](REAL_DEVICE_TEST.md)，协议见 [PROTOCOL.md](PROTOCOL.md)。
