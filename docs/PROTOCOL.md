# HWC3 直连协议

协议版本：`3`，magic：`0x48574333`（`HWC3`）。

状态：自动化协议测试与 targetSdk 24 构建通过；真实 Edge + HarmonyOS 平板直连尚未验证。

## 连接

```text
ws://harmony-web-companion.local:44000/direct
```

`.local` 失败时，用户可把 host 改为 Receiver 显示的私网 IPv4。Receiver 只在用户确认的具体
`wlan*` 私网/link-local IPv4 上监听 TCP 44000；拒绝 `0.0.0.0`、回环、VPN、蜂窝、公网地址和
公网来源。扩展不扫描网络。

所有控制消息是 UTF-8 JSON text message；H.264 是 binary message。客户端 frame 必须 mask，
Receiver 支持合法 continuation frame，单消息上限为 8 MiB + 32 字节头。

## 一次性配对

扩展二维码是以下 JSON 的 QR 编码：

```json
{
  "v": 3,
  "sid": "32-lowercase-hex",
  "token": "64-lowercase-hex",
  "exp": 1784952000000
}
```

- `sid` 和 `token` 来自 Web Crypto CSPRNG；
- `exp` 最多为生成后 60 秒；
- 二维码不包含 IP、长期凭据、URL、标题、Cookie、正文或视频；
- 扩展不把 sid、token 或短码写入 `chrome.storage`；
- Receiver 只在用户点击扫码后调用 ScanKit；
- 摄像头不可用时，平板可输入由 token 派生的六位短码，仍只有效 60 秒。

扫码或短码授权后，扩展发送：

```json
{
  "type": "pair",
  "protocol": 3,
  "sessionId": "32-lowercase-hex",
  "token": "64-lowercase-hex",
  "senderId": "stable-extension-uuid"
}
```

Receiver 校验授权未过期、未使用且内容匹配，随后销毁一次性授权，保存 senderId，并返回：

```json
{
  "type": "paired",
  "protocol": 3,
  "deviceId": "32-lowercase-hex",
  "credential": "64-lowercase-hex"
}
```

两端只在应用沙箱中保存 `deviceId`、senderId 和 credential。IP 变化不改变身份。过期、重放、
未授权或身份不匹配分别返回固定错误码，不回显秘密。

v0.1 运行于用户确认的可信局域网，使用明文 `ws://`；它不支持公网或对抗同网段主动抓包攻击。

## 已配对鉴权

每次媒体连接首条消息：

```json
{
  "type": "auth",
  "protocol": 3,
  "senderId": "stable-extension-uuid",
  "deviceId": "32-lowercase-hex",
  "credential": "64-lowercase-hex",
  "sourceEpoch": 12,
  "codec": "video/avc",
  "avcFormat": "annexb",
  "width": 1280,
  "height": 720,
  "fps": 30
}
```

Receiver 只接受已保存身份、固定编码参数以及不小于历史最新值的 `sourceEpoch`：

```json
{"type":"ready","protocol":3,"sourceEpoch":12}
```

用户在任一端忘记设备后，本地长期凭据立即删除；Receiver 同时关闭当前连接。

## H.264 binary message

每个 `EncodedVideoChunk` 对应一个 WebSocket binary message：

| 偏移 | 字段 | 类型 | 说明 |
| ---: | --- | --- | --- |
| 0 | magic | u32 | `0x48574333` |
| 4 | version | u8 | `3` |
| 5 | flags | u8 | bit0 keyframe；其他位必须为 0 |
| 6 | headerSize | u16 | 固定 `32` |
| 8 | sourceEpoch | u32 | 当前唯一来源的 epoch |
| 12 | sequence | u32 | 编码块序号，允许回绕 |
| 16 | payloadLength | u32 | `1..8 MiB` |
| 20 | reserved | u32 | 必须为 0 |
| 24 | timestampUs | u64 | WebCodecs 时间戳，微秒 |
| 32 | payload | bytes | 一个 H.264 Annex-B Access Unit |

所有整数使用网络字节序。Receiver 校验 binary message 总长完全一致，只接受当前
`sourceEpoch`；旧来源迟到帧丢弃并计入 dropped。恢复或 epoch 变化时请求包含 SPS/PPS + IDR 的关键帧。

## 控制、心跳与遥测

扩展每 5 秒发送：

```json
{"type":"ping","protocol":3,"at":1784952000000}
```

Receiver 回应：

```json
{"type":"pong","protocol":3,"at":1784952000000}
```

Receiver 在会话开始、解码 Flush 或丢失恢复时发送：

```json
{"type":"keyframe","protocol":3,"reason":"loss_flush_or_session_start","requireCodecConfig":true}
```

Receiver 遥测：

```json
{
  "type": "telemetry",
  "protocol": 3,
  "captureUs": 8101000,
  "displayUs": 8161200,
  "receivedFrames": 3600,
  "receivedBytes": 60000000,
  "receiverDecodedFrames": 3598,
  "receiverDroppedFrames": 2
}
```

正常停止使用 `{"type":"close","protocol":3}`。遥测、日志和导出不得包含秘密、网页 URL/标题、
Cookie、正文或视频 payload。

## 错误码

| code | 含义 |
| --- | --- |
| `protocol_mismatch` | 不是 HWC3 |
| `authorization_expired` | 一次性授权过期 |
| `authorization_replayed` | sid 已成功使用 |
| `pairing_failed` | QR/短码内容不匹配 |
| `not_paired` | 未配对连接 |
| `identity_mismatch` | deviceId/senderId/credential 不匹配 |
| `codec_unsupported` | 编码参数不支持 |
| `epoch_stale` | sourceEpoch 早于已接受来源 |

## DNS-SD 说明

Receiver 通过 HarmonyOS 公共 `mdns.addLocalService` 注册 `_hwc._tcp` /
`harmony-web-companion` 服务实例。该 API 不承诺注册裸主机名；因此
`harmony-web-companion.local` 在 Windows/Edge 的实际可解析性必须真机记录，不能由编译或服务
注册结果推断。

## 不存在的能力

HWC3 不传输 audio、pointer、keyboard、scroll、URL、Cookie、正文、桌面枚举，也没有 UDP、公网、
云中继、设备浏览或多路标签页媒体流。
