# v0.1 传输协议草案

## 端口

| 通道 | 传输 | 默认端口 | 用途 |
| --- | --- | --- | --- |
| Discovery | mDNS | 系统分配 | 发现 `_hss._tcp.local` 服务 |
| Control | TCP | 47100 | 配对、能力协商、心跳、触控 |
| Video | UDP | 47101 | H.264 Annex-B 分片 |

端口只绑定专用网络，并由 Windows 防火墙限制在本地子网。

## 控制帧

控制通道使用长度前缀 JSON：

```text
uint32_be payload_length
utf8_json payload
```

握手示例：

```json
{
  "type": "hello",
  "protocol": 1,
  "device": {
    "platform": "HarmonyOS",
    "api": 23,
    "width": 2800,
    "height": 1840,
    "refreshRates": [60]
  },
  "decoders": ["video/avc"]
}
```

服务端选择第一版参数：

```json
{
  "type": "session",
  "sessionId": "random-128-bit-id",
  "codec": "video/avc",
  "width": 1920,
  "height": 1200,
  "fps": 60,
  "videoPort": 47101
}
```

## 视频包

每个 UDP 包包含固定头和一个 H.264 NAL 分片：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| magic | u32 | `0x48535331` (`HSS1`) |
| session | u32 | 会话短标识 |
| frame | u32 | 帧序号 |
| fragment | u16 | 当前分片序号 |
| fragments | u16 | 总分片数 |
| flags | u16 | keyframe/config/end-of-frame |
| payloadLength | u16 | 负载长度 |
| timestampUs | u64 | 采集时间戳 |

v0.1 不实现重传；丢失任何分片时丢弃整帧，并在下一次关键帧恢复。

## 输入消息

触摸坐标归一化到 `[0, 1]`，避免依赖具体平板分辨率：

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

## 安全边界

v0.1 必须具备一次性配对码和会话随机数，但只声明支持可信局域网。后续版本再引入持久设备身份与加密传输。
