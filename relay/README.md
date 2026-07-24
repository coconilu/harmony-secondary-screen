# Windows Relay

该组件是“网页伴随屏”的普通用户态 Relay，不是 Windows 服务，也不包含驱动。

当前实现：

```text
Edge Native Messaging
  → 127.0.0.1 随机端口 + 临时令牌
  → 二进制 WebSocket
  → 校验 H.264 Annex-B Access Unit
  → HSS2 UDP 分片
  → HarmonyOS 原生 Receiver
```

Relay 不监听任何 Windows 局域网地址，只主动连接用户输入并通过一次性码配对的平板。连续链路已
通过进程级假 Receiver 测试；真机连续播放仍是独立门禁。仓库保留单关键帧冒烟发送器，用于隔离
验证平板协议和原生解码路径。

## 构建与自动测试

```powershell
.\scripts\build-relay.ps1 -Configuration Release
```

脚本执行：

1. MSVC x64 Release 构建；
2. C++ 协议边界单元测试；
3. 单关键帧发送器 HSS2 分片单元测试；
4. Native Messaging + 回环 WebSocket + 假 Receiver 配对/UDP 重组进程级联调。

输出：

```text
out\relay\Release\harmony_web_companion_relay.exe
```

## 平板首帧冒烟测试

平板安装并启动 Receiver 后，输入并确认平板当前的 Wi-Fi IPv4。把界面显示的地址和六位一次性码
传给脚本：

```powershell
.\scripts\send-receiver-smoke.ps1 `
  -ReceiverAddress 192.168.1.30 `
  -PairingCode 123456
```

脚本用 `ffmpeg` 生成一个 `1280×720` H.264 Annex-B 关键帧，主动连接平板的 TCP 44000 完成协议
v2 配对，再向 UDP 47101 发送 HSS2 分片。只有平板 AVCodec 返回至少一帧显示遥测时脚本才输出
`{"ok":true,...}`。这不是正式连续投屏路径。

## 当前用户安装

先在 `edge://extensions/` 复制该解压缩扩展的 32 位 ID，然后执行：

```powershell
.\scripts\install-relay-native-host.ps1 -ExtensionId <扩展ID>
```

安装只执行以下当前用户操作：

| 项目 | 位置 |
| --- | --- |
| Relay 与 Native Host manifest | `%LOCALAPPDATA%\HarmonyWebCompanion\NativeHost` |
| Edge Native Messaging 注册 | `HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.coconilu.harmony_web_companion` |

不需要管理员权限，不创建服务、不写 HKLM、不修改防火墙、网络分类、VPN、代理或路由。

卸载：

```powershell
.\scripts\install-relay-native-host.ps1 -Uninstall
```

## 安全边界

- listener 使用系统分配的随机端口，只绑定 `127.0.0.1`；
- Native Host manifest 的 `allowed_origins` 只包含安装时传入的扩展 ID；
- WebSocket 再校验浏览器传给 Native Host 的 Origin 和 256-bit 临时令牌；
- 每次进程启动生成新令牌，令牌不落盘、不进入日志；
- 只允许一个已鉴权 WebSocket 客户端；
- 支持 Edge 对大消息生成的 RFC 6455 continuation frames，并限制重组总长；
- 视频消息上限 8 MiB，长度和协议字段严格验证；
- 鉴权后的协议拒绝向扩展返回脱敏错误码，不再静默断开。
