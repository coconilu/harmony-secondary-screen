# HWC4 真实设备验收

当前文档是待执行步骤，不代表真实设备已经通过。

## 环境记录

| 项目 | 实测 |
| --- | --- |
| 日期 / 操作者 / commit | 待填写 |
| Windows / Edge 版本 | 待填写 |
| 扩展版本 | `0.5.1` |
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
3. 抓取 HWC4 binary header，B 的 `sourceEpoch` 必须大于 A。
4. 人工注入旧 epoch AU，Receiver dropped 增加且画面不回退。
5. 确认同一时刻只有一个 video track、VideoEncoder、WebSocket 媒体流。

#4 的候选页列表与完整切换 UI 尚未实现；全屏连续播放仍由后续体验验收覆盖。常亮、主动息屏和
解锁后的 Surface/AVCodec 恢复必须由 #13 按本节完成真机验收。

## 常亮与息屏恢复回归

| 步骤 | 操作 | 预期 |
| ---: | --- | --- |
| 1 | 开始发送标签页并持续播放超过系统自动息屏时间 | 连接存续期间平板不自动息屏 |
| 2 | 停止发送并等待系统自动息屏时间 | Receiver 释放常亮，平板可按系统设置正常息屏 |
| 3 | 播放期间按电源键主动息屏至少 30 秒后解锁 | WebSocket 会话保持；原生解码器重建并主动请求 SPS/PPS + IDR |
| 4 | 观察解锁后的首帧和计数 | 无需重新连接或重启 Receiver；接收、解码计数重新增长，画面继续更新 |
| 5 | 连续执行息屏/解锁 5 次 | 每次均恢复，Receiver 无崩溃、黑屏或永久卡帧 |

自动化构建只能验证 API、生命周期桥接和关键帧请求路径；本节必须用真实 Edge + HarmonyOS
平板验证后才能标记通过。

### 2026-07-26 Issue #13 修复分支验证边界

| 项目 | 结果 |
| --- | --- |
| 开发基线 | `main@c2140f52cefc7e453779b17368f388bb4b17f66b` |
| 设备可见性 | `hdc 3.2.0d` 可识别一台 HarmonyOS 6.1.0.117 平板，且已安装旧版 Receiver |
| 自动化范围 | 生命周期策略覆盖重复回调、旧 Surface 迟到销毁、后台无 Surface 和前台重建条件；完整 Release 门禁通过（扩展 27/27、Receiver 2/2、依赖审计 0 漏洞、unsigned HAP 构建成功） |
| 本分支真机安装 | 未执行；仓库只生成 unsigned HAP，不改写本机签名配置 |
| 自动息屏回归 | **未验证** |
| 30 秒息屏/解锁与 5 次循环 | **未验证** |
| 协议 | 无变更；恢复沿用 HWC4 `keyframe` + `requireCodecConfig: true` |

设备在线、构建通过或旧版本已安装都不能替代本分支的真实播放验收。只有把本分支构建安装到平板，
同时保持 Edge 媒体会话并观察接收/解码计数重新增长后，才能填写通过结果。

### 2026-07-27 `e5f8640` 真机失败与修复复测边界

| 项目 | 结果 |
| --- | --- |
| exact-head 安装 | 已使用仓库外测试签名安装 `e5f8640acfe612d0d9b286039f77ba1862184bc0`；签名材料未进入仓库 |
| 自动息屏 | 系统超时为 600000 ms；播放到 T0 + 660.296 秒仍为 AWAKE、媒体 TCP 仍连接，机器侧通过 |
| 停止后的常亮释放 | 媒体 TCP 关闭；Receiver 回到等待；对应窗口 SCREEN lock 变为 inactive，常亮释放通过 |
| 第 1 次主动息屏 | 息屏 30.168 秒后仍为 SLEEP；用户解锁后媒体 TCP 未恢复，Edge 显示“捕获失败 / 平板连接意外断开”，严格判定失败并停止后续循环 |
| 断线前脱敏遥测 | capture/encode/send 为 14087/14070/14070，`lastFrameAgeMs=4`、`stallEvents=0`、`directErrors=1`；说明捕获与编码在 socket close 前健康 |
| 失败现场 | Receiver 进程和前台 Ability 存活、44000 仍监听、可信配对保留，但无媒体 TCP；扩展把任意 WebSocket close 立即终止为不可恢复捕获失败 |
| 当前修复 | 保留单 capture、单 encoder、可信凭据和同一 `sourceEpoch`，在恢复窗口内仅重建 WebSocket；断线期间不缓存视频 payload，重连后请求 SPS/PPS + IDR |
| 修复后自动化 | 完整 Release 门禁通过：扩展 31/31、Receiver 2/2、依赖审计 0 漏洞、unsigned HAP 构建成功 |
| 修复后真机 | **待使用修复后的 exact-head 重新签名安装，并从第 1/5 轮重新验收** |

失败导出的原始文件、网页内容、私网地址、配对凭据和签名信息不进入仓库。本表只记录脱敏聚合值。

## 界面与主题截图

浅色、深色真机界面及其证据边界见
[`test-results/2026-07-26-playback-experience.md`](test-results/2026-07-26-playback-experience.md)。

## 仍由 #3 验收

- A/B/C/D 各 10 分钟；
- 全程至少 30 分钟；
- P50/P95 捕获到显示延迟；
- PC 音频相对平板视频偏移；
- 丢包、队头阻塞和恢复；
- 隐私日志扫描。

Issue #13 合并前若没有真实 Edge + 平板媒体会话，以上真机项必须明确保持“未验证”。
