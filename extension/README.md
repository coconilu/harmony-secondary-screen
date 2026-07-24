# Edge 标签页 → HarmonyOS 网页伴随屏

该扩展验证第三个风险切片：Edge 标签页持续产生视频帧，由 WebCodecs 编码为 H.264 Annex-B，并经
随机令牌保护的回环 WebSocket 送到普通用户态 Windows Relay，再经可信局域网发送到 HarmonyOS
原生 Receiver。

编码基线固定为：

| 项目 | 配置 |
| --- | --- |
| 编码 | H.264 Baseline，`avc1.42001f` |
| 封装 | Annex-B |
| 分辨率 | `1280×720`，保持比例并补黑边 |
| 帧率 | 30 fps |
| 目标码率 | 4 Mbps |
| 关键帧间隔 | 最长约 2 秒 |
| 加速偏好 | `prefer-hardware`，不等于已证明使用硬件编码 |

Relay 校验编码块并使用 HSS2 UDP 分片发送到已配对平板；不采集音频，编码块不写入磁盘。当前连续
链路已通过自动化假 Receiver 测试和 13 分 24 秒真实 Edge + 平板短时基线；四阶段独立遥测和
30 分钟验收仍待完成。

## 安装

1. 在 Edge 打开 `edge://extensions/`。
2. 打开“开发人员模式”。
3. 点击“加载解压缩的扩展”，选择本目录：

   ```text
   C:\Users\admin\Documents\doing\harmony-secondary-screen\extension
   ```

4. 复制扩展卡片上显示的 32 位扩展 ID。
5. 在项目根目录构建并为当前用户注册 Relay：

   ```powershell
   .\scripts\build-relay.ps1
   .\scripts\install-relay-native-host.ps1 -ExtensionId <扩展ID>
   ```

6. 回到 `edge://extensions/` 点击扩展的“重新加载”，并固定到工具栏。

## 测试

1. 打开一个持续播放且画面有运动的普通 HTTP/HTTPS 视频标签页。
2. 平板点击“开始接收”，取得当前 Wi-Fi IPv4 和六位一次性码。
3. 保持视频标签页为活动页，点击扩展图标，输入平板 IPv4 和配对码，再点击“发送当前标签页”。
4. 配对成功后图标显示 `REC`；再次点击图标打开状态监视器。
5. 在监视器中依次选择 A/B/C/D 阶段，然后执行对应操作：

   - A：目标标签页在前台可见；
   - B：用其他应用完全覆盖 Edge；
   - C：最小化整个 Edge 窗口；
   - D：恢复 Edge 并切到其他标签页。

6. 每个阶段至少保持 2 分钟。捕获帧、编码帧、局域网发送帧和平板已显示帧都应持续增加；连续 2 秒
   没有新捕获帧会记为一次
   “定格事件”。
7. 测试结束后点击“停止捕获”，再点击“导出测试结果”保存 JSON。
8. 点击“清除并准备下一次”后，平板重新开始接收并生成新码，再开始新测试。

## 本地测试

```powershell
cd extension
npm test
```

## 通过条件

| 指标 | PoC 通过条件 |
| --- | --- |
| `encoderConfig` | `avc1.42001f`、1280×720、30 fps、`annexb` |
| `encodeErrors` | 必须为 0 |
| `encodedFrames` | 四个阶段均持续增长 |
| `droppedFrames / totalFrames` | 建议低于 1%；超过时先定位编码背压 |
| `relayErrors` / `relayInvalidMessages` | 必须为 0 |
| `relayDroppedFrames` | 必须为 0 |
| `relaySentFrames` | 停止后必须等于 `relayReceivedFrames` |
| `relaySentBytes` | 停止后必须等于 `relayReceivedBytes` |
| `lanSentFrames` | 必须持续增长，并最终接近平板 `receiverDecodedFrames` |
| `lanSendErrors` | 必须为 0 |
| `stageRuns` | A/B/C/D 每段至少 120 秒，并包含阶段起止遥测 |

`bitrateKbps` 是最近约 1 秒的滚动值，`averageBitrateKbps` 是全程平均值。H.264 目标码率不是每秒
严格恒定，短时波动属于正常现象。

## 界面证据

- [视觉概念](../docs/assets/capture-monitor-concept.png)
- [1440×900 实现截图](../docs/assets/capture-monitor-implementation.png)

## 隐私边界

- `manifest.json` 不申请 `<all_urls>`、Cookie、history 或页面内容权限；
- `getUserMedia` 明确使用 `audio: false`；
- 状态和导出结果不记录网页 URL、标题、账号或页面内容；
- Relay 本地入口只绑定 `127.0.0.1` 随机端口，不开放 Windows 局域网入站端口；
- Relay 只主动连接用户输入的私网/链路本地 IPv4；配对码不写入扩展存储或测试导出；
- 不提供公网、端口映射或云中继。
