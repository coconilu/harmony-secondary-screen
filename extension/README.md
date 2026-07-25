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

## 权限说明

| 权限 | 用途 |
| --- | --- |
| `activeTab` | 用户点击时确认当前标签页 |
| `tabCapture` | 捕获当前标签页视频 |
| `offscreen` | 持有视频轨和编码器 |
| `storage` | 持久保存可信设备与 source epoch |
| 固定 `.local` origin | 尝试单一 Receiver 地址 |
| optional HTTP origin | 用户手动输入私网 IP 后，只请求该精确 origin |

不申请 `nativeMessaging`、`<all_urls>`、Cookie、history、页面正文或站点脚本注入。

## 编码与隐私

- H.264 Annex-B `1280×720 @ 30 fps`，目标 4 Mbps；
- 关键帧最长约 2 秒，并响应 Receiver 请求；
- `audio: false`，声音留在 PC；
- 不保存 QR token/短码，不记录 URL、标题、正文、Cookie 或视频帧；
- 单活动捕获轨、编码器和媒体流；
- source epoch 防止旧来源迟到帧。

## 自动化

```powershell
npm test
npm audit --audit-level=high
```

测试包含真实本机 WebSocket 假 Receiver，并逐字节比对至少一个 Annex-B Access Unit。

真实 Edge、Local Network Access、裸 `.local` 与 HarmonyOS 平板仍按
[`docs/REAL_DEVICE_TEST.md`](../docs/REAL_DEVICE_TEST.md) 验收。
