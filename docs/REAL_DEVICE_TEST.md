# HWC3 真实设备验收

当前文档是待执行步骤，不代表真实设备已经通过。

## 环境记录

| 项目 | 实测 |
| --- | --- |
| 日期 / 操作者 / commit | 待填写 |
| Windows / Edge 版本 | 待填写 |
| 扩展版本 | `0.5.0` |
| HarmonyOS / API / Receiver 构建 | 待填写 |
| 平板型号（只作证据） | 待填写 |
| Wi-Fi / AP / VPN 状态 | 待填写 |
| 实际 H.264 配置 | 待填写 |

## 前置门禁

```powershell
.\scripts\test.ps1
```

确认：

- manifest 没有 `nativeMessaging`、`<all_urls>`、正文读取或注入；
- Windows 没有安装本项目 Relay、Native Host、服务或驱动；
- Receiver 是 ArkTS + C++ AVCodec + XComponent 原生 HAP；
- 本机开发者自行配置签名，仓库配置无证书、私钥、口令和本机绝对路径。

## 首次扫码与直连

| 步骤 | 操作 | 预期 |
| ---: | --- | --- |
| 1 | PC 与平板接入同一可信 Wi-Fi，打开 Receiver | 自动列出私网 Wi-Fi IPv4，用户点击确认后只绑定该地址 |
| 2 | Edge 加载 `extension/`，打开扩展 | 显示 60 秒一次性 QR 与六位短码 |
| 3 | 平板点击“扫码配对”并扫描 | 只在该操作时出现 ScanKit 相机 UI |
| 4 | 扩展点击“已扫码，连接平板” | 显示稳定 deviceId；二维码再次使用被拒绝 |
| 5 | 在普通 HTTP/HTTPS 页面点击“发送当前标签页” | 直连 WebSocket，首个 SPS/PPS + IDR 后出现画面 |
| 6 | 打开监控页 | 发送、接收、解码计数持续增加，音频仍从 PC 输出 |

记录过期 QR、重复 QR、未扫码直接连接、错误 deviceId/credential 均被拒绝，且日志不含 token、
credential、URL、标题、Cookie、正文或视频内容。

## 两种正式回退

### `.local` → 手动 IP

1. 记录 DNS-SD 服务注册结果。
2. 记录 Windows `harmony-web-companion.local` 是否解析。
3. 若解析或 Edge WebSocket 失败，在已配对面板输入 Receiver 显示的私网 IPv4。
4. 点击“更新地址（不重新配对）”，再次发送页面。
5. 必须连接成功且 deviceId 不变；公网、回环和通配地址必须被扩展与 Receiver 拒绝。

### 摄像头 → 六位短码

1. 取消 ScanKit UI或在无相机环境测试。
2. 在平板输入扩展当前显示的六位短码并授权。
3. 60 秒内完成配对。
4. 过期短码和另一轮二维码的短码必须失败。

## 持久信任与忘记设备

| 测试 | 通过条件 |
| --- | --- |
| 重启 Edge | 仍识别同一 deviceId，不重新扫码 |
| 重启 Receiver | Preferences 恢复同一 deviceId/credential |
| DHCP 改变平板 IP | 更新地址后直连，信任不清除 |
| 扩展“忘记平板” | 本地凭据和手动 IP origin 权限删除，需重新配对 |
| Receiver“忘记电脑” | 凭据删除且当前连接关闭，旧扩展鉴权失败 |

## 单来源与竞态

1. 开始发送标签页 A。
2. 停止后从标签页 B 开始新的捕获。
3. 抓取 HWC3 binary header，B 的 `sourceEpoch` 必须大于 A。
4. 人工注入旧 epoch AU，Receiver dropped 增加且画面不回退。
5. 确认同一时刻只有一个 video track、VideoEncoder、WebSocket 媒体流。

#4 的候选页列表、完整切换 UI、全屏和息屏不在本 Issue。

## 仍由 #3 验收

- A/B/C/D 各 10 分钟；
- 全程至少 30 分钟；
- P50/P95 捕获到显示延迟；
- PC 音频相对平板视频偏移；
- 丢包、队头阻塞和恢复；
- 隐私日志扫描。

Issue #5 合并前若没有真实 Edge + 平板环境，以上真机项必须明确保持“未验证”。
