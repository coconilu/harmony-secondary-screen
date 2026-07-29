# Edge 扩展

## 安装

1. 打开 `edge://extensions/`。
2. 开启“开发人员模式”。
3. 选择“加载解压缩的扩展”，指向本目录。

无需安装 Windows Relay、Native Host、服务或驱动，也不需要管理员权限。

## 首次配对

1. 平板打开 HarmonyOS Receiver，确认当前私网 Wi-Fi IPv4并开始接收。
2. 点击扩展，显示 60 秒一次性二维码。
3. 平板点击“扫码配对”，扫描二维码。
4. 扩展点击“已扫码，连接平板”；默认解析固定 `.local`，确认实际落到允许的私网地址后才发送授权。
5. 配对成功后，进入普通 HTTP/HTTPS 标签页，点击“发送当前标签页”。

摄像头不可用时，在平板输入扩展显示的六位短码；`.local` 失败时，在扩展输入 Receiver 显示的
私网 IPv4。更新 IP 不重新配对。

Edge 首次请求手动私网 IP 权限时可能关闭 popup。扩展会先把当前二维码授权、短码、到期时间和
输入地址保存在 `chrome.storage.session`；60 秒内重新打开 popup 会恢复同一二维码和地址，并在
权限已允许时提示继续连接，无需重新扫码或输入 IP。该 pending state 只存在于当前浏览器内存
会话；配对成功、主动刷新、授权过期或浏览器会话结束后会清除或替换，不写入 `storage.local`。

## 权限说明

| 权限 | 用途 |
| --- | --- |
| `activeTab` | 用户点击时确认当前标签页 |
| `tabCapture` | 捕获当前标签页视频 |
| `offscreen` | 持有视频轨和编码器 |
| `storage` | 持久保存可信设备与 source epoch |
| `webRequest` | 只读观察 Receiver WebSocket 握手的目标地址类别，鉴权前拒绝非私网结果 |
| 固定 `.local` origin | 尝试单一 Receiver 地址 |
| optional HTTP origin | 用户手动输入私网 IP 后，只请求该精确 origin |

手动 IP 配对或保存失败时会回滚本次新增的 origin；更新地址或忘记设备时会枚举并撤销所有未使用的
手动私网 origin，撤销失败会明确报错。

`webRequest` 不使用 blocking 能力，不修改请求，不读取页面流量。扩展通过
`runtime.getURL()` 规范化并只接受 `setup.html` 或 `offscreen.html`；发送者和请求的可选
`id`、`origin`、`documentId`、`initiator` 可验证时必须匹配。Chrome/Edge 只暴露扩展同时拥有
目标和 initiator host permission 的请求；当 `initiator` / `documentId` 同时缺失或 initiator
为 opaque `null` 时，还必须满足 `tabId=-1`、`frameId=0`、`parentFrameId=-1`、
`type=websocket` 的扩展文档请求形状、目标 URL 精确匹配且只有一个待处理 attempt。普通网页标签、
网页 worker 和其他扩展不能使用该回退。
随机 attempt id 只返回给发起文档且不记录、不持久化；无 `documentId` 的 FINISH 仍须持有该 id、
来自同一允许页面且没有歧义。随后按同一 `requestId` 关联响应开始、完成与失败事件。只有观察到
`onCompleted` / `onErrorOccurred`
终态后才合并本次握手所有非空实际目标 `ip`；终态缺失、文档上下文缺失/歧义或地址类别冲突均
失败关闭。运行时消息/观测 API 缺失时不会创建自动 `.local` WebSocket，更不会发送 token 或
credential。没有 `ip` 的终态不会覆盖先前已确认的地址类别；观察结果只保留
`private_ipv4` / `non_private` / `unresolved` 类别，原始 IP 不写入存储或日志。
自动地址失败时，用户可见错误末尾会显示一次当前连接尝试的 `AD1` 诊断码；成功路径不显示。
观测状态只存在于本次 attempt 的内存对象中，FINISH 后立即删除；offscreen 将基准错误和经过固定
枚举校验的诊断码分字段发送，service worker 不从错误字符串反向解析 AD1。只有扩展自身精确
`offscreen.html` 文档可提交捕获遥测、结束或失败消息；浏览器未提供的可选 sender 字段允许缺失，
一旦提供则必须与扩展身份一致。当前错误只通过 service worker 的一次性内存通道交给已打开或下一次
打开的监控页，读取、重置或下一轮开始后即清除。持久 `state.error` / events 会将含任意 AD1 字面量
的非结构化错误整体替换为通用错误，重复、嵌套、内嵌或伪造字符串都不会被识别为诊断码；setup
配对页同样只在当前 popup 显示合法诊断。
两条路径都不把 AD1 写入 console、storage 或测试导出。
固定字段如下：

| 字段 | 含义 |
| --- | --- |
| `B` | BEGIN 是否成功创建 attempt |
| `Q` | `onBeforeRequest` 是否绑定；`not_seen`、`bound`、`ambiguous`，或具体的 request id、请求形状、initiator/document 拒绝枚举 |
| `R` | 是否见到同一尝试的 `onResponseStarted` |
| `T` | 终态：`none`、`completed`、`error` 或异常的 `multiple` |
| `I` | 是否有相关事件携带非空 `ip`；不包含 IP 原值或地址类别 |
| `C` | 已绑定上下文是否失配，或并发/终态是否产生歧义 |
| `S` | WebSocket 结果：`not_started`、`open`、`error`、`timeout` 或 `other` |

例如 `AD1|B=1|Q=shape_parent|R=1|T=completed|I=1|C=0|S=open`
只说明浏览器事件缺少 `parentFrameId`，不会暴露 URL、requestId、documentId、origin/initiator、
token、短码、credential、网页信息、原始 IP 或精确时间。
不申请 `nativeMessaging`、`<all_urls>`、`webRequestBlocking`、Cookie、history、页面正文或
站点脚本注入。

## 编码与隐私

- H.264 Annex-B `1280×720 @ 60 fps`，目标 8 Mbps；不复制源帧，实际帧率以监控页为准；
- 关键帧最长约 2 秒，并响应 Receiver 请求；断线或 WebSocket 背压丢 AU 后，下一帧强制为关键帧；
- `audio: false`，声音留在 PC；
- QR token/短码仅在当前浏览器内存会话的 `chrome.storage.session` 暂存 60 秒；成功、主动刷新或
  过期时清除或替换，不写入 `storage.local`、日志或测试导出；
- 不记录 URL、标题、正文、Cookie 或视频帧；
- 单活动捕获轨、编码器和媒体流；
- source epoch 防止旧来源迟到帧。

## 自动化

```powershell
npm test
npm audit --audit-level=high
```

测试包含真实本机 WebSocket 假 Receiver、逐字节比对至少一个 Annex-B Access Unit、同一
`sourceEpoch` 的异常断线恢复与关键帧请求、虚拟时钟超过 184 秒后继续恢复、断线期间不缓存
视频 payload、认证 ready 前禁止发送、AU 丢弃后的关键帧恢复、STOP/换源取消旧恢复，以及模拟
权限弹窗中断和 popup 重建的模块测试；地址诊断测试分别覆盖解析失败、非私网、连接超时和
WebSocket 不可达，逐项覆盖事件阶段码、请求形状/显式上下文拒绝枚举、诊断值白名单和
敏感值不回显，并验证固定 `.local` 未观察到私网地址前不会发送 token/credential。
真实 ws 恶意 Receiver 测试还会在地址观察完成前主动发送 `paired` / `ready`，验证扩展不保存身份、
不进入 authenticated、不发送 auth 或媒体。
Receiver 测试覆盖 same-epoch 重连、完整 SPS/PPS/IDR
门禁、Flush 窗口的 `NeedInput` 竞态、重复/缺失 codec data、解码队列溢出恢复和正常连续播放；
发送端测试通过 offscreen 实际使用的编排 seam 验证序号、发送计数与 key 成功送达前的 delta 门禁。
自动化不等同于 Edge + HarmonyOS 真机复验。

真实 Edge、Local Network Access、裸 `.local` 与 HarmonyOS 平板仍按
[`docs/REAL_DEVICE_TEST.md`](../docs/REAL_DEVICE_TEST.md) 验收。
