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
WebSocket 不可达，并验证固定 `.local` 未观察到私网地址前不会发送 token/credential。
真实 ws 恶意 Receiver 测试还会在地址观察完成前主动发送 `paired` / `ready`，验证扩展不保存身份、
不进入 authenticated、不发送 auth 或媒体。
Receiver 测试覆盖 same-epoch 重连、完整 SPS/PPS/IDR
门禁、Flush 窗口的 `NeedInput` 竞态、重复/缺失 codec data、解码队列溢出恢复和正常连续播放；
发送端测试通过 offscreen 实际使用的编排 seam 验证序号、发送计数与 key 成功送达前的 delta 门禁。
自动化不等同于 Edge + HarmonyOS 真机复验。

真实 Edge、Local Network Access、裸 `.local` 与 HarmonyOS 平板仍按
[`docs/REAL_DEVICE_TEST.md`](../docs/REAL_DEVICE_TEST.md) 验收。
