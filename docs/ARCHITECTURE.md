# 系统架构

## 产品边界

用户明确点击后，把一个 Edge 标签页的视频画面发送到 HarmonyOS 原生 Receiver；PC 音频保持原输出。
不枚举 Windows 显示器、不捕获桌面、不远程控制电脑。

## 两个用户组件

| 组件 | 职责 |
| --- | --- |
| Edge 扩展 | 用户手势、一次性二维码、可信设备存储、当前标签页捕获、WebCodecs 编码、直连发送 |
| HarmonyOS Receiver | Wi-Fi 确认、扫码/短码授权、DNS-SD 注册、可信电脑存储、WebSocket、AVCodec、XComponent |

Windows Relay/Native Host 不再属于正常路径，也不由 `scripts/test.ps1` 构建。

## 数据流

```text
用户点击“发送当前标签页”
  ↓ activeTab + tabCapture
唯一 video track
  ↓ MediaStreamTrackProcessor
唯一 VideoEncoder（H.264 Annex-B）
  ↓ HWC4 binary WebSocket message
Receiver 绑定用户确认的具体私网 wlan IPv4:44000
  ↓ sourceEpoch 校验
OH_VideoDecoder
  ↓
XComponent Surface
```

音频轨显式设为 `false`。扩展不读取 URL、标题、Cookie、页面正文，也不注入站点脚本。

## 配对、身份与地址

| 概念 | 生命周期 |
| --- | --- |
| QR session/token | 60 秒、成功一次后销毁，不持久化 |
| 六位短码 | token 的人工校验回退，60 秒，不持久化 |
| `deviceId` + credential | 两端应用沙箱持久化，直到用户忘记设备 |
| IP / `.local` | 连接地址，可变化，不代表设备身份 |
| WebSocket | Receiver 打开时按需建立，断开不清除信任 |

Receiver 只绑定 `wlan*` 上由用户确认的 RFC1918 或 IPv4 link-local 地址，拒绝通配、回环、VPN、
蜂窝和公网地址；入站对端也必须来自私网/link-local。

HarmonyOS `mdns.addLocalService` 注册的是 DNS-SD 服务实例，不足以证明 Windows 一定能解析裸
`harmony-web-companion.local`。扩展先尝试该固定地址，失败时允许用户输入 Receiver 显示的私网
IPv4；不进行 mDNS 浏览或子网扫描。

## 竞态边界

每次开始捕获分配单调递增的 `sourceEpoch`。Receiver 记录最新 epoch，只接受当前 epoch 的视频头；
旧 epoch 的迟到帧直接丢弃。扩展在开始新捕获前停止旧 reader、轨道和 encoder，确保单活动来源。

## 权限

| 权限 | 原因 |
| --- | --- |
| `activeTab` | 仅在用户点击时确认当前页面 |
| `tabCapture` | 获取用户选择标签页的媒体流 |
| `offscreen` | 扩展弹窗关闭后持有媒体流和编码器 |
| `storage` | 保存设备身份、凭据、连接地址和 source epoch |
| 固定 `.local` host permission | 只访问一个预定 Receiver 地址 |
| 可选 `http://*/*` 声明 | Chrome match pattern 无法枚举所有 RFC1918；仅在用户输入并确认具体 IP 时请求该精确 origin |

扩展只保留当前可信设备实际使用的手动私网 origin；配对/保存失败回滚新授权，更新地址和忘记设备时
枚举并撤销其余手动 origin，且不得把 `permissions.remove()` 的失败当成成功。

不申请 `<all_urls>`、`nativeMessaging`、Cookie、history、正文读取或脚本注入。

## 非目标

- 多设备浏览、子网扫描、云 rendezvous、WebRTC、蓝牙或 USB；
- 多标签页并发、#4 的候选页/完整切换 UI；
- 音频、DRM 绕过、网页正文采集；
- 公网、端口映射、远程控制；
- 设备型号或网站业务分支。
