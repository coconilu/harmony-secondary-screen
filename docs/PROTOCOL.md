# HWC4 直连协议

协议版本：`4`，magic：`0x48574334`（`HWC4`）。

HWC4 将固定媒体合同从 1280×720 @ 30 fps / 4 Mbps 升级为 1280×720 @ 60 fps / 8 Mbps，
属于不兼容升级。Edge 扩展和平板应用必须同时更新；新旧版本混用时返回
`protocol_mismatch`，不会误报为平板编解码能力不足。
已有 `deviceId`、senderId 和 credential 不因版本升级而删除，双端更新后无需重新配对；
未完成的一次性二维码或短码需要刷新。

状态：自动化协议测试与 targetSdk 24 构建通过；真实 Edge + HarmonyOS 平板直连尚未验证。

## 连接

```text
ws://harmony-web-companion.local:44000/direct
```

`.local` 失败时，用户可把 host 改为 Receiver 显示的私网 IPv4。Receiver 只在用户确认的具体
`wlan*` 私网/link-local IPv4 上监听 TCP 44000；拒绝 `0.0.0.0`、回环、VPN、蜂窝、公网地址和
公网来源。扩展不扫描网络。固定 `.local` 自动路径在发送 `pair` 或 `auth` 前，只读观察
WebSocket 握手的实际目标地址类别；只有 RFC1918 或 IPv4 link-local 才允许发送凭据。

所有控制消息是 UTF-8 JSON text message；H.264 是 binary message。客户端 frame 必须 mask，
Receiver 支持合法 continuation frame，单消息上限为 8 MiB + 32 字节头。
TCP 连接建立后，客户端必须在 5 秒内完成 WebSocket upgrade；静默或不完整的连接会被关闭，
Receiver 随后继续接受下一条连接，避免单个客户端长期占用唯一接收循环。

## 一次性配对

扩展二维码是以下 JSON 的 QR 编码：

```json
{
  "v": 4,
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
  "protocol": 4,
  "sessionId": "32-lowercase-hex",
  "token": "64-lowercase-hex",
  "senderId": "stable-extension-uuid"
}
```

Receiver 校验授权未过期、未使用且内容匹配，随后销毁一次性授权，保存 senderId，并返回：

```json
{
  "type": "paired",
  "protocol": 4,
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
  "protocol": 4,
  "senderId": "stable-extension-uuid",
  "deviceId": "32-lowercase-hex",
  "credential": "64-lowercase-hex",
  "sourceEpoch": 12,
  "codec": "video/avc",
  "avcFormat": "annexb",
  "width": 1280,
  "height": 720,
  "fps": 60
}
```

Receiver 只接受已保存身份、固定编码参数以及不小于历史最新值的 `sourceEpoch`：
发送端请求并配置 60 fps，但不复制帧；若源视频、Edge 合成或设备刷新率不足 60 Hz，监控页显示的
实际捕获/编码帧率会低于 60。

```json
{"type":"ready","protocol":4,"sourceEpoch":12}
```

用户在任一端忘记设备后，本地长期凭据立即删除；Receiver 同时关闭当前连接。

## H.264 binary message

每个 `EncodedVideoChunk` 对应一个 WebSocket binary message：

| 偏移 | 字段 | 类型 | 说明 |
| ---: | --- | --- | --- |
| 0 | magic | u32 | `0x48574334` |
| 4 | version | u8 | `4` |
| 5 | flags | u8 | bit0 keyframe；其他位必须为 0 |
| 6 | headerSize | u16 | 固定 `32` |
| 8 | sourceEpoch | u32 | 当前唯一来源的 epoch |
| 12 | sequence | u32 | 当前 capture 中成功交给 WebSocket 的 AU 序号，从 0 开始并允许 u32 回绕 |
| 16 | payloadLength | u32 | `1..8 MiB` |
| 20 | reserved | u32 | 必须为 0 |
| 24 | timestampUs | u64 | WebCodecs 时间戳，微秒 |
| 32 | payload | bytes | 一个 H.264 Annex-B Access Unit |

所有整数使用网络字节序。Receiver 校验 binary message 总长完全一致，只接受当前
`sourceEpoch`；旧来源迟到帧丢弃并计入 dropped。`sequence` 不为本地未连接/背压丢弃的 AU
分配序号，当前版本也不使用它检测网络缺口。恢复或 epoch 变化时请求包含 SPS/PPS + IDR 的关键帧。

## 控制、心跳与遥测

扩展每 5 秒发送：

```json
{"type":"ping","protocol":4,"at":1784952000000}
```

Receiver 回应：

```json
{"type":"pong","protocol":4,"at":1784952000000}
```

Receiver 在会话开始、解码 Flush 或丢失恢复时发送：

```json
{"type":"keyframe","protocol":4,"reason":"loss_flush_or_session_start","requireCodecConfig":true}
```

Receiver 从后台返回前台或取得新的有效 Surface 时，仍使用上述 HWC4 `keyframe` 消息请求
SPS/PPS + IDR；本地 Ability、Surface 和 AVCodec 生命周期修复不新增控制消息，也不改变端口、
字段、二进制帧格式或兼容性边界。

若 HarmonyOS 息屏期间由系统中断底层 TCP，Edge 扩展不得把一次 WebSocket `close` 立即升级为
捕获失败。只要用户没有停止或切换来源，且未收到明确的协议、身份、认证、epoch 或 codec
永久错误，扩展就保留同一条用户授权的标签页 capture、同一个 VideoEncoder、可信凭据和
`sourceEpoch`，持续以最长 2 秒退避重建 WebSocket，并重新发送既有 `auth`；每次连接尝试仍有
独立超时，避免单次握手无限挂起。断线期间编码输出直接丢弃，不缓存视频 payload；重连成功后
强制生成关键帧。任一 AU 因未连接或 WebSocket 背压被丢弃时，发送端也把下一次可编码帧强制为
关键帧。Receiver 允许与最新值相等的 `sourceEpoch` 重新认证；每次可信认证（包括相同 epoch
重连）都 Flush 或重建解码器并进入 `NeedsCodecData`，在收到同一个 AU 内完整的 SPS + PPS + IDR
前丢弃普通 P 帧。解码输入队列溢出、输入 buffer 不足或提交失败时同样清空待解码依赖链、进入
`NeedsCodecData` 并按既有 `keyframe` 合同请求 SPS/PPS + IDR。STOP 或新来源会立即取消旧恢复
任务，旧任务不得覆盖新来源状态。该行为不增加消息类型，不改变 HWC4 版本或二进制帧格式。

Receiver 遥测：

```json
{
  "type": "telemetry",
  "protocol": 4,
  "captureUs": 8101000,
  "displayUs": 8161200,
  "receivedFrames": 3600,
  "receivedBytes": 60000000,
  "receiverDecodedFrames": 3598,
  "receiverDroppedFrames": 2,
  "receiverResyncEvents": 3,
  "receiverKeyframeRequests": 3
}
```

`receiverResyncEvents` 统计可信重连、队列溢出或解码输入失败触发的解码同步事件；
`receiverKeyframeRequests` 统计成功发出的 SPS/PPS + IDR 请求。扩展另记录本地 AU 丢弃触发的
`directResyncEvents`。这些字段只包含聚合计数。

正常停止使用 `{"type":"close","protocol":4}`。遥测、日志和导出不得包含秘密、网页 URL/标题、
Cookie、正文或视频 payload。

## 错误码

| code | 含义 |
| --- | --- |
| `protocol_mismatch` | 不是 HWC4；扩展与平板应用需要同时更新 |
| `authorization_expired` | 一次性授权过期 |
| `authorization_replayed` | sid 已成功使用 |
| `pairing_failed` | QR/短码内容不匹配 |
| `not_paired` | 未配对连接 |
| `identity_mismatch` | deviceId/senderId/credential 不匹配 |
| `codec_unsupported` | 编码参数不支持 |
| `epoch_stale` | sourceEpoch 早于已接受来源 |

## 自动地址发布

Receiver 同时维护两个互不等价的发布层：

| 层 | 名称 / 记录 | 用途 |
| --- | --- | --- |
| DNS-SD | `_hwc._tcp` / `harmony-web-companion` | 公共 API 服务实例；当前扩展不浏览服务列表 |
| 固定 mDNS A | `harmony-web-companion.local` → 当前确认的 Wi-Fi IPv4 | 供 Edge 默认 WebSocket 地址解析 |

最小 mDNS A 响应器使用 UDP multicast `224.0.0.251:5353`，只加入用户确认的当前 `wlan*`
RFC1918 或 IPv4 link-local 接口。确认地址时同时保存 interface index；生产 socket 开启
`IP_PKTINFO` 与 `IP_RECVTTL`、关闭 `IP_MULTICAST_ALL`，并只接受以下元数据全部满足的报文：

| 入站门禁 | 必须值 |
| --- | --- |
| interface index | 用户确认地址所属的同一个 `wlan*` index |
| destination | `224.0.0.251` |
| source port | `5353` |
| query / probe IPv4 TTL / hop limit | `1`（目标 Windows DNS 客户端实测）或 `255`（完整 mDNS querier） |
| response IPv4 TTL / hop limit | 仅 `255` |
| source address | RFC1918 或 IPv4 link-local |

RFC 6762 §11 要求 mDNS **响应**使用 IP TTL 255，并要求查询方只接受本链路响应；它没有把
TTL 255 定义为 responder 接收查询的必要条件。目标 Windows 11 的 DNS 客户端在真实 WLAN 上以
TTL 1 发送其余字段合规的 `.local` query，因此 Receiver 只对 query/probe、且仅在上述其他门禁
全部成立时额外接受 TTL 1。正式 response 仍只接受 TTL 255；TTL 1 response 不能触发冲突、
goodbye 或停止。TTL 2、64、128 等其他值始终拒绝，避免把兼容范围扩成任意 hop limit。

报文层只接受 DNS ID 0、单 question、IN class、固定名称的 A/ANY 查询；拒绝其他名称/类型、多
question、response-as-query、truncated、越界、循环压缩指针、尾随数据和超过 512 字节的输入。
响应 IP TTL 固定为 255；DNS A 记录 TTL 为 120 秒。响应最多 64 字节、只含一个 cache-flush A
记录；单次响应不超过查询的 1.5 倍，普通查询响应限制为每秒最多 10 条。known-answer 的剩余
TTL 大于或等于原 TTL 一半时抑制响应。

开始发布前随机等待 0–250 ms，再发送三次间隔 250 ms、携带待声明 A 的 probe。探测期间若收到已
发布的不同 A response，则后来者进入冲突；若两个候选同时 probe，则按 A RDATA 字节序确定性仲裁，
不会以到达先后决定。已发布所有者收到不同 A 的新 probe 时立即重发权威 A 防御并继续发布；收到
正式同名不同 A response 才发送一次 TTL 0 goodbye 并进入冲突。正常停止、Wi-Fi 地址或 interface
index 失效、重新选择地址时，同样只对旧地址发送一次 TTL 0 goodbye 后关闭 socket。

OpenHarmony target NDK 的 sysroot 声明 `recvmsg`、`IP_PKTINFO`、`IP_RECVTTL`、
`IP_MULTICAST_ALL`、`in_pktinfo` 与 `if_nametoindex`，且 unsigned Release target 已编译该生产
adapter。生产状态机测试通过注入 socket/clock/interface seam 覆盖两个 responder 的已发布/后
启动/同时启动、query/probe 的 Windows TTL 1 与完整 querier TTL 255、TTL 1 response 不得改变
所有权、TTL 255 response 仍触发冲突、其他 hop limit 和错误接口过滤、5353 共享、停止与析构
并发、频率预算和一次 goodbye。以上仍不能证明修复后的目标设备运行时 multicast 或
Windows/Edge 解析成功，必须重新真机验证。

公开 DNS-SD 注册、固定 A 可解析和 HWC4 身份鉴权必须分别记录；前两者都不授予身份信任。

## 不存在的能力

HWC4 不传输 audio、pointer、keyboard、scroll、URL、Cookie、正文、桌面枚举，也没有 UDP 媒体、
公网、云中继、设备浏览或多路标签页媒体流；唯一 UDP 输入面是上述固定名称的受限 mDNS A 响应器。
