# HWC6 直连协议

协议版本：`6`，magic：`0x48574336`（`HWC6`）。

HWC6 把媒体合同升级为鉴权绑定的动态 `width`、`height`、`maxFps`。Edge 扩展和平板应用必须
同时更新；HWC5 控制消息、二维码和 binary frame 都返回或触发 `protocol_mismatch`，不得混用。
升级不会主动删除两端已保存的 `deviceId`、senderId 和 credential；因此保留同一应用沙箱时，
HWC5 时代的配对记录可直接用于 HWC6 challenge/proof，无需重新扫码。

状态：自动化协议测试与 targetSdk 24 构建通过；真实 Edge + HarmonyOS 平板直连尚未验证。

## 连接

```text
ws://harmony-web-companion.local:44000/direct
```

`.local` 失败时，用户可把 host 改为 Receiver 显示的私网 IPv4。Receiver 只在用户确认的具体
`wlan*` 私网/link-local IPv4 上监听 TCP 44000；拒绝 `0.0.0.0`、回环、VPN、蜂窝、公网地址和
公网来源。扩展不扫描网络。Edge 在真实目标平台不会向扩展可靠暴露 WebSocket 的 remote IP，
因此 HWC6 不依赖 `webRequest` 或地址观测判定身份。默认 `.local` 必须通过下面的高熵
challenge-response；用户手抄数字 IPv4 的短码例外在后文单独定义。Receiver 的具体 Wi-Fi 私网
绑定仍是网络边界，QR/长期 credential proof 是自动路径的秘密释放边界。

所有控制消息是 UTF-8 JSON text message；H.264 是 binary message。客户端 frame 必须 mask，
Receiver 支持合法 continuation frame，单消息上限为 8 MiB + 32 字节头。
TCP 连接建立后，客户端必须在 5 秒内完成 WebSocket upgrade；静默或不完整的连接会被关闭，
Receiver 随后继续接受下一条连接，避免单个客户端长期占用唯一接收循环。

## 一次性配对

扩展二维码是以下 JSON 的 QR 编码：

```json
{
  "v": 6,
  "sid": "32-lowercase-hex",
  "token": "64-lowercase-hex",
  "exp": 1784952000000
}
```

- `sid` 和 `token` 来自 Web Crypto CSPRNG；
- `exp` 最多为生成后 60 秒；
- 二维码不包含 IP、长期凭据、URL、标题、Cookie、正文或视频；
- 扩展只在 `chrome.storage.session` 暂存 sid、token 与短码 60 秒，不写 `storage.local`；
- Receiver 只在用户点击扫码后调用 ScanKit；
- 摄像头不可用时，平板可输入由 token 派生的六位短码，仍只有效 60 秒；该模式必须在扩展填写
  Receiver 显示的数字私网 IPv4，不能使用默认 `.local`。

二维码扫码授权后，扩展先生成 32 字节 Web Crypto CSPRNG nonce，只发送非秘密 challenge：

```json
{
  "type": "pair_challenge",
  "protocol": 6,
  "mode": "pair",
  "sessionId": "32-lowercase-hex",
  "senderId": "stable-extension-uuid",
  "nonce": "64-lowercase-hex"
}
```

Receiver 只在扫码 token 仍有效、未使用且 challenge 格式正确时返回：

```json
{
  "type": "pair_proof",
  "protocol": 6,
  "mode": "pair",
  "proofMode": "qr",
  "sessionId": "32-lowercase-hex",
  "senderId": "stable-extension-uuid",
  "nonce": "64-lowercase-hex",
  "deviceId": "32-lowercase-hex",
  "proof": "64-lowercase-hex"
}
```

`proofMode` 在 HWC6 自动路径固定为 `qr`，HMAC key 是 token 的 32 个原始字节。proof 为
HMAC-SHA256，canonical message 是以下 UTF-8 字节（字段间为单个 LF，末尾无 LF）：

```text
HWC6-PAIR-PROOF
<proofMode>
<sessionId>
<senderId>
<nonce>
<deviceId>
```

扩展用本地 token 计算期望 proof，并用固定 64 字符循环进行常数时间
比较。只有 proof、mode、sessionId、senderId、nonce、deviceId 全部匹配后才发送：

```json
{
  "type": "pair",
  "protocol": 6,
  "mode": "pair",
  "sessionId": "32-lowercase-hex",
  "token": "64-lowercase-hex",
  "senderId": "stable-extension-uuid",
  "nonce": "64-lowercase-hex"
}
```

Receiver 要求 final message 与本连接 challenge 完全一致，再执行 60 秒、一次性 QR token 匹配，
销毁授权并生成 credential：

```json
{
  "type": "paired",
  "protocol": 6,
  "deviceId": "32-lowercase-hex",
  "credential": "64-lowercase-hex"
}
```

两端只在应用沙箱中保存 `deviceId`、senderId 和 credential。nonce、proof 和 challenge 状态只在
当前 WebSocket 内存中存在，不进入 console、storage、事件、监控或导出。IP 变化不改变身份。

### 六位短码的数字 IPv4 分流

六位短码不能作为 HMAC key：任何对外返回的短码 proof 都会成为可在离线枚举 100 万种取值的
oracle。因此 Receiver 只有短码授权时，对默认 `.local` 的 `pair_challenge` 返回固定
`short_code_requires_manual_ipv4`，不生成 proof，也不接收 token。扩展提示用户把“平板地址”
改为 Receiver 屏幕显示的数字私网 IPv4。

扩展只接受 RFC1918 / IPv4 link-local 字面量，并在用户确认后请求该精确 origin 权限；随后首个
控制消息是：

```json
{
  "type": "pair_manual_ipv4",
  "protocol": 6,
  "mode": "pair_manual_ipv4",
  "sessionId": "32-lowercase-hex",
  "token": "64-lowercase-hex",
  "senderId": "stable-extension-uuid"
}
```

Receiver 按 60 秒、一次性短码授权校验 token 派生结果并返回同一 `paired`。这一手动路径把用户
抄写 Receiver 精确数字 IPv4 定义为 out-of-band endpoint authentication，首包会携一次性 token；
它不宣称满足自动 `.local` 的“proof 前零秘密”不变量。公开 HMAC 短码 proof 在 HWC6 中不存在。
若未来要让短码安全使用自动 `.local`，必须引入真正抗离线猜测的 PAKE，而不是限速或更换 nonce。

v0.1 运行于用户确认的可信局域网，使用明文 `ws://`；它不支持公网或对抗同网段主动抓包攻击。

## 已配对鉴权

每次媒体连接先发送不含 credential 的 challenge：

```json
{
  "type": "auth_challenge",
  "protocol": 6,
  "mode": "auth",
  "senderId": "stable-extension-uuid",
  "deviceId": "32-lowercase-hex",
  "sourceEpoch": 12,
  "nonce": "64-lowercase-hex",
  "codec": "video/avc",
  "avcFormat": "annexb",
  "width": 1280,
  "height": 720,
  "maxFps": 60
}
```

Receiver 只对已保存的 senderId/deviceId、合法动态媒体合同以及不小于历史最新值的
`sourceEpoch` 计算 proof。宽高必须为正偶数；长边不超过 1920、短边不超过 1080、像素数不超过
2073600；`maxFps` 为 1～60。HMAC key 是 credential 的 32 个原始字节，canonical UTF-8
message 为：

```text
HWC6-AUTH-PROOF
<senderId>
<deviceId>
<sourceEpoch十进制>
<nonce>
video/avc
annexb
<width十进制>
<height十进制>
<maxFps十进制>
```

返回：

```json
{
  "type": "auth_proof",
  "protocol": 6,
  "mode": "auth",
  "senderId": "stable-extension-uuid",
  "deviceId": "32-lowercase-hex",
  "sourceEpoch": 12,
  "nonce": "64-lowercase-hex",
  "proof": "64-lowercase-hex"
}
```

扩展常数时间验证 proof 和全部绑定字段后，才发送含 credential 的 final `auth`；Receiver 再次
要求字段与本连接 challenge 一致，并常数时间比较 credential：

```json
{
  "type": "auth",
  "protocol": 6,
  "mode": "auth",
  "senderId": "stable-extension-uuid",
  "deviceId": "32-lowercase-hex",
  "credential": "64-lowercase-hex",
  "sourceEpoch": 12,
  "nonce": "64-lowercase-hex",
  "codec": "video/avc",
  "avcFormat": "annexb",
  "width": 1280,
  "height": 720,
  "maxFps": 60
}
```

发送端按源轨设置推导 `maxFps`，硬上限 60；只对到达的源帧做节流，不复制、不插值。分辨率按
源方向等比缩放、偶数对齐、不放大，并同时满足长边、短边和像素上限。优先探测
`avc1.42002a`（AVC Level 4.2）；不支持或配置失败时降级为不超过 1280×720 的
`avc1.420028`（Level 4.0）。码率按像素和 `maxFps` 自动计算并限制在实现定义的安全区间。

```json
{"type":"ready","protocol":6,"sourceEpoch":12,"width":1280,"height":720,"maxFps":60}
```

`ready` 必须逐字段回显 HMAC 绑定的媒体合同；任何不一致均中止连接。

旧/错 nonce、错 mode、错 identity、重放 proof、并发第二条消息、伪造 proof，以及 proof 前提前
发送 `paired` / `ready` 都立即失败关闭；扩展在这些路径发送 token/credential 的数量必须为 0。
Receiver 每条连接最多接受一个 challenge 和一个对应 final message，控制消息上限 2048 字节，
整个 challenge + final 必须在 10 秒内完成。

用户在任一端忘记设备后，本地长期凭据立即删除；Receiver 同时关闭当前连接。

## H.264 binary message

每个 `EncodedVideoChunk` 对应一个 WebSocket binary message：

| 偏移 | 字段 | 类型 | 说明 |
| ---: | --- | --- | --- |
| 0 | magic | u32 | `0x48574336` |
| 4 | version | u8 | `6` |
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
{"type":"ping","protocol":6,"at":1784952000000}
```

Receiver 回应：

```json
{"type":"pong","protocol":6,"at":1784952000000}
```

Receiver 在会话开始、解码 Flush 或丢失恢复时发送：

```json
{"type":"keyframe","protocol":6,"reason":"loss_flush_or_session_start","requireCodecConfig":true}
```

Receiver 从后台返回前台或取得新的有效 Surface 时，仍使用上述 HWC6 `keyframe` 消息请求
SPS/PPS + IDR；本地 Ability、Surface 和 AVCodec 生命周期修复不新增控制消息，也不改变端口、
字段、二进制帧格式或兼容性边界。

若 HarmonyOS 息屏期间由系统中断底层 TCP，Edge 扩展不得把一次 WebSocket `close` 立即升级为
捕获失败。只要用户没有停止或切换来源，且未收到明确的协议、身份、认证、epoch 或 codec
永久错误，扩展就保留同一条用户授权的标签页 capture、同一个 VideoEncoder、可信凭据和
`sourceEpoch`，持续以最长 2 秒退避重建 WebSocket，并重新执行 challenge/proof 门禁后发送
既有 `auth`；每次连接尝试仍有
独立超时，避免单次握手无限挂起。断线期间编码输出直接丢弃，不缓存视频 payload；重连成功后
强制生成关键帧。任一 AU 因未连接或 WebSocket 背压被丢弃时，发送端也把下一次可编码帧强制为
关键帧。Receiver 允许与最新值相等的 `sourceEpoch` 重新认证；每次可信认证（包括相同 epoch
重连）都 Flush 或重建解码器并进入 `NeedsCodecData`，在收到同一个 AU 内完整的 SPS + PPS + IDR
前丢弃普通 P 帧。解码输入队列溢出、输入 buffer 不足或提交失败时同样清空待解码依赖链、进入
`NeedsCodecData` 并按既有 `keyframe` 合同请求 SPS/PPS + IDR。STOP 或新来源会立即取消旧恢复
任务，旧任务不得覆盖新来源状态。

捕获期间连续 3 帧出现同一新尺寸才视为稳定来源变化；瞬时抖动和 A→B→A 不触发切换。发送端
串行分配新的持久化 `sourceEpoch`，关闭旧连接和 encoder，不 flush 旧输出，再按新尺寸重新执行
能力探测、鉴权与解码器创建。每个 encoder 输出绑定不可变的 generation、epoch 和媒体合同，
任何迟到旧输出都不得借用新全局状态发送。Receiver 对同一 epoch 只接受原合同；新 epoch 可原子
激活新合同，动态 Destroy/Create AVCodec，创建失败不发送 `ready`。新会话仍需 SPS/PPS + IDR。

Receiver 遥测：

```json
{
  "type": "telemetry",
  "protocol": 6,
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

正常停止使用 `{"type":"close","protocol":6}`。遥测、日志和导出不得包含秘密、网页 URL/标题、
Cookie、正文或视频 payload。

## 错误码

| code | 含义 |
| --- | --- |
| `protocol_mismatch` | 不是 HWC6；扩展与平板应用需要同时更新 |
| `challenge_invalid` | challenge 字段、模式、编码参数或身份无效 |
| `challenge_state_invalid` | 同一连接收到重复、乱序或超量控制消息 |
| `challenge_required` | 未在时限内完成 challenge + final |
| `authorization_expired` | 一次性授权过期 |
| `authorization_replayed` | sid 已成功使用 |
| `pairing_failed` | QR/短码内容不匹配 |
| `short_code_requires_manual_ipv4` | 短码不能用于自动 `.local` proof；需填写 Receiver 数字私网 IPv4 |
| `not_paired` | 未配对连接 |
| `identity_mismatch` | deviceId/senderId/credential 不匹配 |
| `codec_unsupported` | 编码参数不支持 |
| `epoch_stale` | sourceEpoch 早于已接受来源 |
| `epoch_contract_mismatch` | 同一 sourceEpoch 携带了不同媒体合同 |
| `decoder_configuration_failed` | Receiver 无法按已鉴权的动态合同创建解码器 |

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

公开 DNS-SD 注册、固定 A 可解析和 HWC6 challenge/proof 身份鉴权必须分别记录；前两者都不授予身份信任。

## 不存在的能力

HWC6 不传输 audio、pointer、keyboard、scroll、URL、Cookie、正文、桌面枚举，也没有 UDP 媒体、
公网、云中继、设备浏览或多路标签页媒体流；唯一 UDP 输入面是上述固定名称的受限 mDNS A 响应器。
