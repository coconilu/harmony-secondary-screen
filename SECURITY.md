# Security Policy

v0.1 仅面向用户确认的可信家庭局域网，不应部署在公共 Wi-Fi、访客网络或互联网可达环境。

HarmonyOS Receiver 只允许绑定用户确认的 Wi-Fi IPv4；Windows Relay 只主动连接平板，不开放局域网
入站端口。Local Bridge 必须只绑定回环地址，并同时受扩展 ID 和临时随机令牌限制。

日志不得包含网页 URL/标题、Cookie、页面内容、配对码、session ID、本地桥接令牌、证书私钥或签名
口令。仓库不得提交本机签名材料或带秘密的构建配置。

请不要在公开 Issue 中披露可直接利用的安全漏洞。发现安全问题时，请通过 GitHub Security Advisory
私下报告。
