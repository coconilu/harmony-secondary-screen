# v0.1 传输协议

局域网协议版本：`2`，状态：**连续发送与关键帧反馈已实现，并通过 13 分 24 秒真机短时基线**。
本地桥接协议版本：`1`，状态：**真实 Edge 连续传输已验证**。完整四阶段和 30 分钟验收仍未完成。
除 Native Messaging 自身的长度前缀外，所有多字节整数使用网络字节序。

协议 v2 面向“Edge 标签页 → 普通用户态 Relay → HarmonyOS 原生 Receiver”。它与旧 IddCx 原型的
协议 v1 不兼容，任何一端收到非 `2` 版本都必须明确拒绝，不能猜测降级。

## 角色与连接方向

| 角色 | 职责 |
| --- | --- |
| Edge Extension | 捕获并编码用户选择的标签页；不直接访问局域网协议 |
| Windows Relay | 普通用户态发送端；主动连接 Receiver，发送 H.264 视频 |
| Harmony Receiver | 配对码所有者；只在用户确认的 Wi-Fi IPv4 上监听和解码 |

用户在 Receiver 中确认当前可信 Wi-Fi 后，Receiver 显示该 Wi-Fi IPv4 和六位一次性配对码。用户把
两者输入 Edge 扩展；Relay 随后主动连接 Receiver。

## 端口与绑定

| 通道 | 传输 | 默认端口 | 绑定与用途 |
| --- | --- | --- | --- |
| Control | TCP | 44000 | Receiver 只绑定用户确认的 Wi-Fi IPv4；配对、心跳、恢复、关键帧和遥测 |
| Video | UDP | 47101 | Receiver 绑定同一 Wi-Fi IPv4；只接受已配对 Relay IP 和当前会话 |

v0.1 不允许 Receiver 绑定 `0.0.0.0`、蜂窝、VPN 或未经确认的接口；不允许公网端口映射。Windows
Relay 不开放局域网入站端口，也不修改 Windows 公用/专用网络分类、VPN 或系统代理。

控制端同时只接受一个已鉴权 Relay。其他来源可以完成 TCP 三次握手，但在没有正确一次性码时不得
获得会话或触发视频数据面。

## 本地 Extension ↔ Relay 通道

本地通道不是局域网协议，不使用 44000/47101：

1. Edge 通过 Native Messaging 启动已登记且 `allowed_origins` 只包含本扩展 ID 的 Relay。
2. Relay 随机选择回环端口，只绑定 `127.0.0.1`，并生成至少 128-bit 临时令牌。
3. Native Messaging 返回端口和令牌；令牌不得写入日志或持久化。
4. 扩展以 `ws://127.0.0.1:<ephemeral>/capture` 建立二进制 WebSocket，并在首条消息证明令牌。
5. Relay 只允许一个已授权连接；扩展或 Relay 退出时立即关闭端口并清除令牌。

控制元数据使用 UTF-8 JSON，视频使用二进制 WebSocket message。视频数据面不得 Base64 包在 JSON
里作为正式实现。

### Native Messaging 控制

Native Messaging 使用浏览器规定的 32-bit 小端长度前缀 JSON。Relay 启动成功后只通过该受
`allowed_origins` 限制的通道返回：

```json
{"type":"ready","protocol":1,"port":49152,"token":"64-char-lowercase-hex"}
```

`port` 必须是 Relay 随机绑定的回环端口；`token` 使用系统 CSPRNG 生成 32 字节随机数并编码为
64 个小写十六进制字符。扩展不得把令牌放入 `chrome.storage`、导出 JSON 或日志。

扩展随后经同一 Native Messaging 连接传入本次会话的平板信息：

```json
{
  "type": "configure_receiver",
  "receiverAddress": "192.168.3.112",
  "pairingCode": "123456"
}
```

Relay 只接受私网或 IPv4 link-local 地址；配对码只保存在本次进程内存中，不写入日志或扩展存储。
完成下文协议 v2 配对后返回：

```json
{"type":"receiver_ready","protocol":2}
```

失败只返回固定错误码，不回显地址、配对码、Nonce 或 session：

```json
{"type":"error","code":"pairing_failed"}
```

扩展停止会话时发送：

```json
{"type":"shutdown"}
```

Native Messaging stdin 关闭、收到 `shutdown`、扩展断开或进程退出，任一情况都必须关闭 WebSocket
listener 并清除内存中的令牌。

### 回环 WebSocket 握手

- 请求目标固定为 `/capture`；
- Relay 必须校验 RFC 6455 版本 13、Upgrade/Connection 头；
- `Origin` 必须等于 Edge 传给 Native Host 的调用扩展 Origin；
- WebSocket 建立后的第一条消息必须精确证明临时令牌：

```json
{"type":"auth","token":"64-char-lowercase-hex"}
```

鉴权成功后 Relay 返回：

```json
{"type":"ready","protocol":1}
```

在鉴权完成前不得接受视频。只允许一个连接；客户端消息必须带 mask。Relay 必须按 RFC 6455 重组
浏览器产生的 continuation frames，并允许控制帧穿插在分片之间；控制帧自身不得分片。重组后的单条
消息上限为本地视频头加 8 MiB，累计长度超限、缺少起始帧、分片期间出现新的数据起始帧或连接结束
都必须立即丢弃整个消息并关闭连接。

### 本地视频消息

每个 `EncodedVideoChunk` 对应一个二进制 WebSocket message：

| 偏移 | 字段 | 类型 | 说明 |
| ---: | --- | --- | --- |
| 0 | magic | u32 | `0x48574C31`（`HWL1`） |
| 4 | version | u8 | 本地桥接版本 `1` |
| 5 | flags | u8 | bit0 keyframe；其他位必须为 0 |
| 6 | headerSize | u16 | 固定为 24 |
| 8 | sequence | u32 | 编码块序号，允许回绕 |
| 12 | payloadLength | u32 | H.264 负载长度，最大 8 MiB |
| 16 | timestampUs | u64 | `EncodedVideoChunk.timestamp`，单位微秒 |
| 24 | payload | bytes | 一个 H.264 Annex-B Access Unit |

Relay 必须验证 magic、版本、flags、头长、负载长度和 WebSocket message 实际长度完全一致。验证后
同一个 Access Unit 按本协议后文的 UDP 头分片并主动发送到已配对 Receiver。

### 本地控制与遥测

扩展可请求即时统计或正常关闭：

```json
{"type":"stats"}
{"type":"close"}
```

Relay 至少每秒一次，并在 `close` 前返回累计统计：

```json
{
  "type": "telemetry",
  "receivedFrames": 3600,
  "receivedBytes": 60000000,
  "keyFrames": 60,
  "invalidMessages": 0
}
```

未知文本消息、无效二进制消息、未 mask、非法分片序列、超限消息或第二个连接必须拒绝，不能猜测
兼容。鉴权后的协议拒绝应尽力先返回不含令牌和页面信息的固定错误码
`local_protocol_rejected`，再关闭连接。

Receiver 请求关键帧时 Relay 经本地 WebSocket 返回：

```json
{"type":"keyframe","reason":"loss_flush_or_session_start","requireCodecConfig":true}
```

offscreen encoder 必须让下一次成功提交的编码帧成为包含 SPS/PPS 的 IDR。

## TCP 控制帧

局域网控制通道使用长度前缀 UTF-8 JSON：

```text
uint32_be payload_length  // 1..65536
uint8[payload_length] utf8_json
```

解析器必须支持拆包/粘包。长度为 0、超过 64 KiB、UTF-8 非法或 JSON 类型不符合预期时，立即终止
连接并销毁未鉴权状态。

## 首次配对

Receiver 使用系统 CSPRNG 生成六位一次性码和 `receiverNonce`。一次性码显示后 5 分钟过期，成功
消费后立即失效。

Relay 发送：

```json
{
  "type": "pair",
  "protocol": 2,
  "pairingCode": "123456",
  "senderNonce": "sender-random-value",
  "receiverNonce": "receiver-random-value",
  "codec": "video/avc",
  "avcFormat": "annexb",
  "width": 1280,
  "height": 720,
  "fps": 30
}
```

`receiverNonce` 由扩展在用户输入 IPv4 后先通过未鉴权 `hello` 获取：

```json
{"type":"hello","protocol":2}
{"type":"hello","protocol":2,"receiverNonce":"receiver-random-value","pairingExpiresInSec":240}
```

`hello` 不返回设备名称、系统版本、网络信息或配对码。

Receiver 校验一次性码、Nonce、协议和编码上限后，生成随机 128-bit `sessionId` 和非零 32-bit
`sessionShort`：

```json
{
  "type": "session",
  "protocol": 2,
  "sessionId": "8a7730fcd4b64f70a7db7cd0e8fedc80",
  "sessionShort": 2712847316,
  "codec": "video/avc",
  "avcFormat": "annexb",
  "width": 1280,
  "height": 720,
  "fps": 30,
  "videoPort": 47101
}
```

Relay 必须使用已鉴权 `session` 返回的 `videoPort`，不能忽略协商结果继续硬编码目标端口；v0.1
Receiver 默认返回 47101。

v0.1 必须支持 `1280×720 @ 30 fps`。`1920×1080 @ 30 fps` 只有双方能力探测通过后才能协商。其他
尺寸、60 fps、音频或非 H.264 编码必须拒绝。

## 五秒恢复窗口

控制连接意外断开后，Receiver 仅为原 Relay IP 保留会话 5 秒。Relay 重连时发送：

```json
{
  "type": "resume",
  "protocol": 2,
  "sessionId": "8a7730fcd4b64f70a7db7cd0e8fedc80"
}
```

命中原 IP、随机会话和期限后，Receiver 返回原 `session` 并发送关键帧请求。超时后会话销毁并生成
新配对码；旧码、旧会话和旧 UDP 包都不能恢复。

## 心跳、关键帧和遥测

Relay 每秒发送 `ping`，Receiver 返回：

```json
{"type":"ping","senderSendUs":1200000}
{"type":"pong","senderSendUs":1200000,"receiverReceiveUs":8100000,"receiverSendUs":8100030}
```

时间戳来自各自单调时钟，只用于 NTP 形式的工程偏移估算。

Receiver 在会话开始、分片丢失、解码器 Flush 或恢复连接后发送：

```json
{"type":"keyframe","reason":"loss_flush_or_session_start","requireCodecConfig":true}
```

Relay 必须把请求转发给 offscreen encoder。下一关键帧必须包含 SPS/PPS 与 IDR；在此之前 Receiver
丢弃增量帧。

Receiver 每秒最多发送一条遥测：

```json
{
  "type": "telemetry",
  "captureUs": 8101000,
  "displayUs": 8161200,
  "framesDecoded": 3600,
  "framesDropped": 3
}
```

软件时钟估算不能冒充真实端到端延迟或 PC 音频/平板视频偏移，最终验收见
`docs/REAL_DEVICE_TEST.md`。

正常停止：

```json
{"type":"stop","reason":"user_stopped_capture"}
```

允许的 `reason` 由实现枚举，日志不得包含网页 URL、标题、Cookie 或配对码。

## 错误

已鉴权连接在可以安全返回错误时使用：

```json
{"type":"error","code":"wifi_not_allowed"}
```

v0.1 固定错误码：

| code | 含义 |
| --- | --- |
| `protocol_mismatch` | 对端不是协议 v2 |
| `pairing_failed` | 一次性码、Nonce 或有效期校验失败 |
| `codec_unsupported` | H.264 Annex-B、尺寸或帧率无法协商 |
| `wifi_not_allowed` | 用户撤销 Wi-Fi、网络切换或绑定地址失效 |
| `session_expired` | 会话不存在或五秒恢复窗口已过期 |

配对失败不得说明究竟是码、Nonce 还是有效期错误。Wi-Fi 失效时 Receiver 先撤销视频数据面，再尽力
发送 `wifi_not_allowed` 并关闭控制连接；错误消息发送失败不能阻止撤销。

## UDP 视频包

UDP 数据报不超过 1232 字节：32 字节固定头 + 最多 1200 字节 H.264 负载。

| 偏移 | 字段 | 类型 | 说明 |
| ---: | --- | --- | --- |
| 0 | magic | u32 | `0x48535332`（`HSS2`） |
| 4 | version | u8 | `2` |
| 5 | headerSize | u8 | `32` |
| 6 | flags | u16 | bit0 keyframe、bit1 codec-config、bit2 end-of-frame |
| 8 | session | u32 | `sessionShort` |
| 12 | frame | u32 | 帧序号，回绕允许 |
| 16 | fragment | u16 | 从 0 开始的分片序号 |
| 18 | fragments | u16 | 本帧分片总数 |
| 20 | payloadLength | u16 | `0..1200`，必须等于数据报剩余长度 |
| 22 | reserved | u16 | v0.1 为 0 |
| 24 | timestampUs | u64 | `VideoFrame.timestamp`/编码块对应的捕获时间，单位微秒 |

编码负载是一个 H.264 Annex-B Access Unit。关键帧携带 SPS/PPS，编码无 B 帧，常规关键帧间隔最长
2 秒。Receiver 最多同时保留 4 帧重组状态；单帧超过 8 MiB、150 ms 未收齐、字段不一致或任一分片
丢失时丢弃整帧并请求关键帧。v0.1 不重传视频。

`keyframe` 与 `codec-config` 在同一帧的所有分片上保持一致；`end-of-frame` 只在最后一个分片上置位。
当前 `scripts/send-receiver-smoke.ps1` 使用这一格式发送一个包含 SPS/PPS 与 IDR 的测试 Access Unit，
用于验证配对、分片重组与 AVCodec 首帧显示。它不是连续投屏发送器。

Receiver 只接受：

- 来源 IP 等于当前已鉴权 TCP Relay IP；
- 目标地址等于当前已确认 Wi-Fi IPv4；
- `magic/version/session` 与当前会话一致；
- 分片数量、长度和帧大小在边界内。

## 不存在的协议能力

协议 v2 没有以下消息；收到后按未知类型拒绝：

- pointer、keyboard、scroll 或任意输入注入；
- audio 或音量控制；
- URL 导航、Cookie、页面正文或站点脚本命令；
- 桌面/窗口枚举；
- 公网发现、云中继或端口映射。

## 安全边界

- 六位码只用于用户确认的可信局域网配对，不是互联网级认证；
- 一次性码、Nonce、会话 ID 和本地桥接令牌均使用系统 CSPRNG；
- Receiver 只绑定用户确认的具体 Wi-Fi IPv4，网络切换时先撤销 session，再停止 UDP 接收并关闭
  listener，不退化为 `0.0.0.0`；
- Relay 主动连接平板，不创建 Windows 局域网入站防火墙规则；
- Local Bridge 只绑定回环地址，并同时受扩展 ID 与临时令牌限制；
- v0.1 不提供传输加密、持久设备身份、公网访问或端口转发；
- 双端对长度、分片、会话、编码配置和消息速率做边界检查；
- 日志不得记录配对码、会话 ID、本地令牌、网页 URL、标题或页面内容。
