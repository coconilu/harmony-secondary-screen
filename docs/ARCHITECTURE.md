# 系统架构

## 产品边界

用户明确点击后，把一个 Edge 标签页的视频画面发送到 HarmonyOS 原生 Receiver；PC 音频保持原输出。
不枚举 Windows 显示器、不捕获桌面、不远程控制电脑。

## 两个用户组件

| 组件 | 职责 |
| --- | --- |
| Edge 扩展 | 用户手势、一次性二维码、可信设备存储、当前标签页捕获、WebCodecs 编码、直连发送 |
| HarmonyOS Receiver | Wi-Fi 确认、固定 mDNS A 发布、DNS-SD 注册、扫码/短码授权、可信电脑存储、WebSocket、AVCodec、XComponent |

Windows Relay/Native Host 不再属于正常路径，也不由 `scripts/test.ps1` 构建。

## 数据流

```text
用户点击“发送当前标签页”
  ↓ activeTab + tabCapture
唯一 video track
  ↓ MediaStreamTrackProcessor
自动媒体合同（等比偶数尺寸、不放大、≤1920×1080、≤2073600 像素、≤60 fps）
  ↓ VideoEncoder 能力探测（AVC Level 4.2；必要时 ≤720p Level 4.0）
唯一 VideoEncoder（H.264 Annex-B）
  ↓ HWC6 binary WebSocket message
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
| QR session/token | 60 秒、成功一次后销毁；仅暂存 `storage.session`，不写 `storage.local` |
| 六位短码 | token 的人工校验回退，60 秒；只允许配合用户抄写的数字私网 IPv4 |
| `deviceId` + credential | 两端应用沙箱持久化，直到用户忘记设备 |
| IP / `.local` | 连接地址，可变化，不代表设备身份 |
| WebSocket | Receiver 打开时按需建立，断开不清除信任 |

Receiver 只绑定 `wlan*` 上由用户确认的 RFC1918 或 IPv4 link-local 地址，拒绝通配、回环、VPN、
蜂窝和公网地址；入站对端也必须来自私网/link-local。

地址发现、服务注册和身份鉴权是三条独立边界：

| 层 | 当前实现 | 不代表 |
| --- | --- | --- |
| 固定主机名地址 | Receiver 在已确认 `wlan*` 接口上只响应 `harmony-web-companion.local` 的 mDNS A 查询 | 设备身份或长期信任 |
| DNS-SD 服务实例 | HarmonyOS `mdns.addLocalService` 注册 `_hwc._tcp` | 裸 `.local` 一定可解析 |
| 身份鉴权 | 一次性 QR 后保存 `deviceId`、senderId、credential | IP 或主机名永久不变 |

mDNS 响应器不浏览服务、不枚举邻居、不扫描子网，不回答其他名称或记录类型。Receiver 在用户确认
地址时同时保存 `wlan*` 的 interface index；UDP socket 只加入该 index 的
`224.0.0.251:5353` 组，并通过 `recvmsg` 元数据要求入站目的组、interface index、源端口 5353
和可信私网/link-local source 全部匹配。完成通用门禁后先分类消息：query/probe hop limit 只
接受目标 Windows DNS 客户端实测的 1 或完整 mDNS querier 的 255；response 只接受 255，TTL 1
response 不得进入所有权冲突流程；其他值拒绝。出站响应 IP TTL 始终为 255。OpenHarmony NDK
提供 `IP_PKTINFO`、`IP_RECVTTL` 和 `IP_MULTICAST_ALL`；生产 adapter 关闭跨接口 multicast
接收，任一 socket option 不可用即发布失败并保留数字 IPv4 回退。

发布采用随机 0–250 ms 延迟与三次间隔 250 ms 的 probe。同时启动者按 A 记录字节序确定性仲裁；
已发布所有者收到后来者 probe 时发送权威 A 防御，不把名称让给后启动者。正式同名不同 A
response 才进入冲突状态。停止接收、Wi-Fi 地址或 interface index 失效、重新选择地址时，旧地址
恰好发送一次 TTL 0 goodbye 后关闭 socket；TCP 控制与媒体仍只绑定具体私网地址。

## 竞态边界

每次开始捕获和稳定尺寸变化都通过 service worker 串行分配单调递增的 `sourceEpoch`。Receiver
记录最新 epoch，只接受当前 epoch 的视频头；同 epoch 必须保持同一动态媒体合同。旧 encoder
输出携不可变 generation/epoch/合同，迟到帧直接丢弃。新 epoch 先重建 encoder、重新鉴权并让
Receiver 动态重建 AVCodec；首个可交付 AU 必须包含 SPS/PPS + IDR。

Receiver 将协商宽高暴露给 ArkUI，两种播放 Surface 都按真实宽高比 contain；横屏和竖屏来源
不会被固定 16:9 拉伸。

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

Edge 不会稳定向扩展公开 WebSocket 的真实远端 IP。默认 `.local` 配对使用高熵 QR token 的
HWC6 challenge/proof，扩展验证 proof 和全部绑定字段后才发送 token；长期 credential 鉴权还
绑定 `sourceEpoch`、`width`、`height` 和 `maxFps`。
六位短码不能用作 HMAC key，否则可从一次 proof 离线枚举，因此自动 `.local` 明确拒绝短码授权。
短码只允许用户在扩展填写 Receiver 显示的数字私网 IPv4并授予精确 origin 后走
`pair_manual_ipv4`；该手动地址确认是 out-of-band endpoint authentication，首包携一次性 token。
自动路径的乱序、错 nonce、错 mode、错身份、伪造或重放 proof 均在秘密发送前失败。
nonce、proof 与 challenge 状态只存在于当前 WebSocket 内存中，不写入 storage、日志、监控或导出。
扩展不申请 `<all_urls>`、`webRequest`、`webRequestBlocking`、`nativeMessaging`、Cookie、
history、正文读取或脚本注入。

## 非目标

- 多设备浏览、子网扫描、云 rendezvous、WebRTC、蓝牙或 USB；
- 多标签页并发、#4 的候选页/完整切换 UI；
- 音频、DRM 绕过、网页正文采集；
- 公网、端口映射、远程控制；
- 设备型号或网站业务分支。
