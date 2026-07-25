# Security Policy

v0.1 仅面向用户确认的可信家庭局域网，不应部署在公共 Wi-Fi、访客网络或互联网可达环境。

HarmonyOS Receiver 只允许绑定用户确认的 `wlan*` 私网或 IPv4 link-local 地址，拒绝通配、回环、
VPN、蜂窝和公网地址。Edge 只直连已配对 Receiver，不进行设备列表或子网扫描。

日志不得包含网页 URL/标题、Cookie、页面内容、一次性 token/短码、设备 credential、视频帧、证书
私钥或签名口令。仓库不得提交本机签名材料或带秘密的构建配置。

HWC3 的 `ws://` 只适用于用户确认的可信局域网，不提供公网或同网段主动抓包攻击防护。

请不要在公开 Issue 中披露可直接利用的安全漏洞。发现安全问题时，请通过 GitHub Security Advisory
私下报告。
