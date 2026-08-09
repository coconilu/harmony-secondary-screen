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
4. 扩展点击“已扫码，连接平板”；默认 `.local` 路径先验证 Receiver 的 HWC6 QR proof，再发送 token。
5. 配对成功后，进入普通 HTTP/HTTPS 标签页，点击“发送当前标签页”。

摄像头不可用时，在平板输入扩展显示的六位短码，并把扩展“平板地址”改为 Receiver 显示的数字
私网 IPv4；自动 `.local` 不接受低熵短码 proof。更新 IP 不重新配对。

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
| 固定 `.local` origin | 尝试单一 Receiver 地址 |
| optional HTTP origin | 用户手动输入私网 IP 后，只请求该精确 origin |

手动 IP 配对或保存失败时会回滚本次新增的 origin；更新地址或忘记设备时会枚举并撤销所有未使用的
手动私网 origin，撤销失败会明确报错。

真实 Edge 不会稳定公开 WebSocket 的远端 IP，扩展因此不再申请 `webRequest`。默认 `.local`
配对只允许高熵 QR token 的 HMAC-SHA256 proof；proof、nonce、模式、senderId、sessionId 和
deviceId 全部匹配后才发送 token。六位短码不能安全充当 HMAC key，否则一次 proof 就能被离线
枚举；因此短码仅在用户抄写 Receiver 数字私网 IPv4、授予该精确 origin 后使用
`pair_manual_ipv4`，首个控制消息会携带一次性 token。长期 credential 鉴权始终使用 challenge。
自动路径的主动 `paired`、错 nonce/mode/identity、伪造或重放 proof 均在秘密发送前失败。
nonce、proof 与 challenge 状态不写入 storage、日志、监控或导出。
不申请 `nativeMessaging`、`<all_urls>`、`webRequest`、`webRequestBlocking`、Cookie、history、
页面正文或站点脚本注入。

## 编码与隐私

- H.264 Annex-B 自动合同：保持源方向和比例、偶数尺寸、不放大，长边 ≤1920、短边 ≤1080、
  像素数 ≤2073600、`maxFps` ≤60；码率自动限界，不提供手动质量选择；
- 优先探测 AVC Level 4.2；不支持或配置失败时自动降级到 ≤720p 的 Level 4.0；
- 不复制、不插值源帧，实际捕获/编码帧率与目标 `maxFps` 在监控页分开显示；
- 关键帧最长约 2 秒，并响应 Receiver 请求；断线或 WebSocket 背压丢 AU 后，下一帧强制为关键帧；
- `audio: false`，声音留在 PC；
- QR token/短码仅在当前浏览器内存会话的 `chrome.storage.session` 暂存 60 秒；成功、主动刷新或
  过期时清除或替换，不写入 `storage.local`、日志或测试导出；
- 不记录 URL、标题、正文、Cookie 或视频帧；
- 单活动捕获轨、编码器和媒体流；
- source epoch 防止旧来源迟到帧；稳定尺寸变化会分配新 epoch、重新鉴权、重建两端编码/解码，
  并等待 SPS/PPS + IDR 后恢复。

## 自动化

```powershell
npm test
npm audit --audit-level=high
```

测试包含真实本机 WebSocket 假 Receiver、逐字节比对至少一个 Annex-B Access Unit、同一
`sourceEpoch` 的异常断线恢复与关键帧请求、虚拟时钟超过 184 秒后继续恢复、断线期间不缓存
视频 payload、认证 ready 前禁止发送、AU 丢弃后的关键帧恢复、STOP/换源取消旧恢复，以及模拟
权限弹窗中断和 popup 重建的模块测试；网络错误覆盖自动地址/连接与手动 IPv4 连接分类。
真实 ws 恶意 Receiver 测试覆盖自动 `.local` 的 QR proof、短码拒绝且零 token、伪造/重放/错
nonce/mode/identity、主动 `paired` / `ready`，以及手动 IPv4 短码成功路径。
Receiver 测试覆盖 same-epoch 重连、完整 SPS/PPS/IDR
门禁、Flush 窗口的 `NeedInput` 竞态、重复/缺失 codec data、解码队列溢出恢复和正常连续播放；
发送端测试通过 offscreen 实际使用的编排 seam 验证序号、发送计数与 key 成功送达前的 delta 门禁。
自动化不等同于 Edge + HarmonyOS 真机复验。

真实 Edge、Local Network Access、裸 `.local` 与 HarmonyOS 平板仍按
[`docs/REAL_DEVICE_TEST.md`](../docs/REAL_DEVICE_TEST.md) 验收。
