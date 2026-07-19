# v0.1 传输协议

协议版本：`1`。所有多字节整数使用网络字节序。v0.1 只支持同一可信局域网中的手动 IPv4；
mDNS `_hss._tcp.local` 保留为后续发现能力，不影响手动 IP 保底路径。

## 端口与绑定

| 通道 | 传输 | 默认端口 | 绑定与用途 |
| --- | --- | --- | --- |
| Control | TCP | 47100 | Host 只绑定 Windows 分类为“专用”的接口 IPv4；配对、心跳、恢复、触控、遥测 |
| Video | UDP | 47101 | Receiver 本地收包；Host 只向已配对 TCP 对端 IP 发送 H.264 Annex-B |

公共网络接口、`0.0.0.0` 控制监听和公网端口映射都不属于 v0.1。控制端同时只接受一个 Receiver。

## 控制帧

控制通道使用长度前缀 UTF-8 JSON：

```text
uint32_be payload_length  // 1..65536
uint8[payload_length] utf8_json
```

解析器支持拆包/粘包，长度为 0 或超过 64 KiB 时立即终止连接。

### 首次配对

Host 用系统 CSPRNG 生成六位一次性码。Receiver 发送：

```json
{
  "type": "pair",
  "protocol": 1,
  "pairingCode": "123456",
  "receiverNonce": "receiver-random-value",
  "videoPort": 47101,
  "codec": "video/avc",
  "width": 1920,
  "height": 1200,
  "fps": 60
}
```

Host 校验一次性码、协议与固定视频端口后生成随机 128-bit `sessionId` 和非零 32-bit
`sessionShort`：

```json
{
  "type": "session",
  "protocol": 1,
  "sessionId": "8a7730fcd4b64f70a7db7cd0e8fedc80",
  "sessionShort": 2712847316,
  "codec": "video/avc",
  "width": 1920,
  "height": 1200,
  "fps": 60,
  "videoPort": 47101
}
```

配对码消费后不可用于另一个会话。

### 五秒恢复窗口

控制连接意外断开后，Host 仅为原对端 IP 保留会话 5 秒。Receiver 重连时发送：

```json
{
  "type": "resume",
  "protocol": 1,
  "sessionId": "8a7730fcd4b64f70a7db7cd0e8fedc80",
  "videoPort": 47101
}
```

命中 IP、随机会话和期限后，Host 返回原 `session` 并请求新关键帧。超时后会话销毁并生成新的
一次性码；旧码和旧会话都不能恢复。

### 心跳与时钟估算

Receiver 每秒发送 `ping`。Host 返回四时间戳所需的服务端时间：

```json
{"type":"ping","clientSendUs":1200000}
{"type":"pong","clientSendUs":1200000,"serverReceiveUs":8100000,"serverSendUs":8100030}
```

Receiver 用 NTP 形式估算 Host 与 Receiver 单调时钟偏移。这个估算只用于诊断遥测，不替代外部
端到端延迟测量。

### 关键帧与遥测

```json
{"type":"keyframe","reason":"loss_or_session_start"}
{"type":"telemetry","captureUs":8101000,"endToEndUs":60200,"framesDecoded":3600,"framesDropped":3}
```

Host 收到关键帧请求后触发 Media Foundation 强制 IDR。遥测写入 CSV；`endToEndUs` 是时钟同步后的
软件埋点估算，实际验收见 `docs/REAL_DEVICE_TEST.md`。

## 视频包

UDP 数据报不超过 1232 字节：32 字节固定头 + 最多 1200 字节 H.264 负载。

| 偏移 | 字段 | 类型 | 说明 |
| ---: | --- | --- | --- |
| 0 | magic | u32 | `0x48535331`（`HSS1`） |
| 4 | version | u8 | `1` |
| 5 | headerSize | u8 | `32` |
| 6 | flags | u16 | bit0 keyframe、bit1 codec-config、bit2 end-of-frame |
| 8 | session | u32 | `sessionShort` |
| 12 | frame | u32 | 帧序号，回绕允许 |
| 16 | fragment | u16 | 从 0 开始的分片序号 |
| 18 | fragments | u16 | 本帧分片总数 |
| 20 | payloadLength | u16 | `0..1200`，必须等于数据报剩余长度 |
| 22 | reserved | u16 | v0.1 为 0 |
| 24 | timestampUs | u64 | IddCx 帧进入驱动队列时的 Host 单调时钟 |

编码负载是 H.264 Annex-B Access Unit。关键帧前附带 SPS/PPS，编码设置无 B 帧、关键帧间隔 1 秒。
Receiver 最多同时保留 4 帧重组状态；单帧超过 16 MiB、100 ms 未收齐、字段不一致或任一分片
丢失时丢弃整帧并请求关键帧。v0.1 不重传视频。

## 输入消息

XComponent 坐标归一化到 `[0,1]`，只接受 `pointerId=0`。Host 映射到 IddCx 显示器在 Windows
虚拟桌面中的实际矩形，并用 `MOUSEEVENTF_VIRTUALDESK` 注入。

```json
{
  "type": "pointer",
  "action": "move",
  "pointerId": 0,
  "x": 0.42,
  "y": 0.68,
  "buttons": 1,
  "timestampUs": 123456789
}
```

`action` 为 `down`、`move`、`up` 或 `scroll`。滚动模式额外携带归一化 `deltaY`；Receiver UI
显式切换指针/滚动模式，避免把拖动手势错误解释为滚动。

## 安全边界

- 六位码只用于首次可信局域网配对，不是互联网级认证；
- 会话 ID 使用系统 CSPRNG，断连恢复同时校验原 IP 与 5 秒期限；
- 控制监听只绑定 Windows 专用网络适配器；公共网络默认拒绝；
- v0.1 明确不提供加密、持久设备身份、公网访问或端口转发支持；
- Host 与 Receiver 对长度、坐标、分片数和会话标识做边界检查。
