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
4. 扩展点击“已扫码，连接平板”。
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
| 固定 `.local` origin | 尝试单一 Receiver 地址 |
| optional HTTP origin | 用户手动输入私网 IP 后，只请求该精确 origin |

手动 IP 配对或保存失败时会回滚本次新增的 origin；更新地址或忘记设备时会枚举并撤销所有未使用的
手动私网 origin，撤销失败会明确报错。

不申请 `nativeMessaging`、`<all_urls>`、Cookie、history、页面正文或站点脚本注入。

## 编码与隐私

- H.264 Annex-B `1280×720 @ 60 fps`，目标 8 Mbps；不复制源帧，实际帧率以监控页为准；
- 关键帧最长约 2 秒，并响应 Receiver 请求；
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
`sourceEpoch` 的异常断线恢复与关键帧请求，以及模拟权限弹窗中断和 popup 重建的模块测试。
自动化不等同于 Edge + HarmonyOS 真机复验。

真实 Edge、Local Network Access、裸 `.local` 与 HarmonyOS 平板仍按
[`docs/REAL_DEVICE_TEST.md`](../docs/REAL_DEVICE_TEST.md) 验收。
