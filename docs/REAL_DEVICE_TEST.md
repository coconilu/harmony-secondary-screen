# HWC4 真实设备验收

本文同时包含待执行步骤与按 exact-head 记录的历史结果；只有明确写为 PASS 的单项才代表对应真机门禁通过。

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

## 自动地址发现验收

以下各项必须使用同一个 exact-head 的真实 Edge 与本分支原生 Receiver；“Receiver 已发布”、
DNS-SD 注册成功、单元测试或 TCP 端口可达均不能替代 Windows 解析结果。

| 场景 | 操作与记录 | 通过条件 |
| --- | --- | --- |
| 默认扫码 | 开始接收，记录 Receiver 显示地址；刷新并扫描一轮 QR | Windows 将固定 `.local` 解析为该地址，Edge 无需输入 IP 即完成配对 |
| 鉴权前地址门禁 | 记录扩展错误分类与用户动作，并在隔离环境主动先发 `paired` / `ready` | 非私网、无法确认或请求前响应均在发送 token/credential 前被拒绝；不保存身份、不进入 authenticated |
| Receiver 重启 | 停止后重新开始接收 | 停止期间旧记录不继续回答；重启后同一地址恢复且 deviceId 不变 |
| Wi-Fi 地址变化 | 切换到另一个可信 Wi-Fi 后重新确认新地址 | 旧 A 被 TTL 0 撤销；新地址只有重新确认并开始接收后发布 |
| 名称冲突 | 在隔离测试网引入同名不同 A | Receiver 明确显示冲突；扩展不随机连接，提示输入本机数字 IPv4 |
| 不可解析 | 阻断/隔离 multicast 后尝试默认连接 | 显示“自动地址解析失败/超时”，唯一下一步为数字 IPv4 回退 |
| 手动回退 | 输入 Receiver 显示的数字 IPv4 | 仅请求精确 origin；扫码与配对成功；不清除既有设备身份 |

每次记录：Receiver 与扩展 exact commit、Windows/Edge/HarmonyOS 版本、DNS-SD 注册状态、固定 A
发布/冲突状态、Windows 解析到的地址类别、WebSocket 连接耗时和用户可见结果。不得保存私网地址
原值、token、短码、credential、网页 URL/标题或视频帧到仓库。

### 2026-07-28 Issue #19 当前分支验证边界

| 证据层 | 结果 |
| --- | --- |
| 开发基线 | `main@2e530dd9b7ba460a9db0de5956810399fa5b7c4d` |
| 自动化 | 扩展真实 ws 测试覆盖主动 `paired` / `ready` 不能绕过地址门禁；固定 A 协议与生产 responder seam 覆盖错误接口/端口/TTL/目的组、随机三 probe、已发布者防御、同时启动仲裁、5353 共享、频率/放大预算、地址失效一次 TTL 0、并发 Stop 与析构 |
| 权限 | 仅新增只读 `webRequest`；固定与可选 host 范围未扩大；无 `<all_urls>` 或 `webRequestBlocking` |
| HarmonyOS 构建 | unsigned Release 已编译 `recvmsg`、`IP_PKTINFO`、`IP_RECVTTL`、`IP_MULTICAST_ALL=0` 生产 adapter；不代表目标设备运行时 multicast socket 与 Edge 解析成功 |
| 真实 Edge + Receiver | **本分支尚未安装或执行；默认 `.local`、重启、地址变化、冲突和手动回退均为未验证** |

在上述真机行补齐 exact-head 证据前，Issue #19 的首两项端到端验收不得标记为 PASS。

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
| 3 | 播放期间按电源键主动息屏至少 30 秒后解锁 | 可信配对、用户选择的标签页 capture 和 `sourceEpoch` 保持；若系统中断 TCP/WebSocket，扩展自动重建连接；原生解码器重建并主动请求 SPS/PPS + IDR |
| 4 | 观察解锁后的首帧和计数 | 无需用户重新配对、重新发送、手工重连或重启 Receiver；解锁后 5 秒内移动画面恢复，Receiver `received`/`decoded` 计数重新增长 |
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
| 当前修复 | 保留单 capture、单 encoder、可信凭据和同一 `sourceEpoch`，持续重建 WebSocket；断线期间不缓存视频 payload，重连后请求 SPS/PPS + IDR |
| 修复后自动化 | 完整 Release 门禁通过：扩展 31/31、Receiver 2/2、依赖审计 0 漏洞、unsigned HAP 构建成功 |
| 修复后真机 | **待使用修复后的 exact-head 重新签名安装，并从第 1/5 轮重新验收** |

失败导出的原始文件、网页内容、私网地址、配对凭据和签名信息不进入仓库。本表只记录脱敏聚合值。

### 2026-07-28 `6b1594f` 长息屏失败与第二轮修复边界

| 项目 | 结果 |
| --- | --- |
| exact-head 安装 | 已用仓库外测试签名覆盖安装 `6b1594f2c620051d087b8d8ca05fcf4c7d079e8a`，安装身份与测试基线一致；签名材料未进入仓库 |
| 正式监控 | 16:34:27.328 息屏，连续 SLEEP 184.180 秒，16:37:31.874 唤醒 |
| 连接中断 | 息屏约 5.096 秒后媒体 TCP 从已连接变为 0；Receiver 进程、前台 Ability 和监听端口仍存活 |
| 唤醒后现场 | 唤醒后至少 15.244 秒内没有恢复媒体 TCP；平板停在最后一帧，扩展最终显示“捕获失败 / 平板连接恢复超时” |
| 脱敏聚合遥测 | capture 51965、encode 51921、后台丢弃 44、stall 0、encode error 0；只能证明本地捕获/编码健康，不能证明 Receiver 收帧或握手阶段 |
| 根因 | 旧实现的全局 120 秒恢复预算在平板仍处于 SLEEP 时已经耗尽，约比实际唤醒早 59.45 秒终止捕获；唤醒时已没有重连客户端 |
| 第二轮修复 | 无 STOP、换源或永久协议错误时持续恢复；退避最长 2 秒；保留同一 capture、VideoEncoder 和 `sourceEpoch`；断线 payload 直接丢弃；认证 ready 后强制关键帧 |
| 取消与错误边界 | STOP/换源主动取消旧连接尝试，代际守卫阻止旧 Promise 覆盖新来源；协议、身份、认证、epoch、codec 永久错误立即失败 |
| 自动化边界 | 虚拟时钟覆盖超过 184 秒且跨过旧 120 秒边界的恢复、同 epoch、无 payload 回放、关键帧、取消与永久错误；完整 Release 门禁通过：扩展 32/32、Receiver 2/2、依赖审计 0 漏洞、unsigned HAP 构建成功 |
| 下一次真机 | **本轮不执行；待代码独立审查通过、扩展重新加载并安装 exact-head 后，从第 1/5 轮重新验收** |

本次只保存状态、计数和持续时间等脱敏证据，不保存截图原件、原始探针、网页内容、私网地址、
配对凭据或签名信息。hilog 缓冲区没有足够的握手阶段日志，因此不把连接失败细分到未被证实的
accept、upgrade 或 auth 子阶段。

### 2026-07-28 `26a8e37` 长息屏、已知限制与花屏修复边界

| 项目 | 结果 |
| --- | --- |
| 测试基线 | 用户在 PR head `26a8e375d471efdad17ad8b8b213f08762f66aea` 重载扩展并重新发送，息屏前人工确认动态画面与 Receiver `received`/`decoded` 计数增长 |
| 监控武装 | 连续 5/5 个样本满足 AWAKE、单监听、单媒体连接和活动 SCREEN lock |
| 正式周期 | 17:44:14.639 观察到息屏，连续 SLEEP 712.428 秒，17:56:07.251 观察到唤醒 |
| 机器侧恢复 | 唤醒后 15.521 秒、24 个样本内媒体连接和 SCREEN lock 均未恢复；原 5 秒自动恢复门禁严格 FAIL，未开始 Cycle 2 |
| 视觉层 | 用户确认解锁后 10 秒仍停在息屏前最后一帧；激活原 Edge 播放源标签页后继续播放。用户接受“需要重新激活原标签页”的已知限制，但该豁免不得写成后台标签页自动恢复 PASS |
| 花屏现场 | 用户随后报告播放经常花屏；没有把连接完整性等同于 H.264 依赖链完整性 |
| 代码根因 | 相同 `sourceEpoch` 重连跳过 Decoder Flush；发送端未连接/背压丢 AU 后不强制下一关键帧；Receiver 解码队列溢出只弹出旧帧后继续提交 P 帧，三者都可能在缺少可靠参考帧时继续解码 |
| 当前同步修复 | 每次可信认证进入 `NeedsCodecData`，完整 SPS + PPS + IDR 前丢弃 P 帧；发送 AU 丢弃后强制下一关键帧；队列溢出清空依赖链并请求完整同步；新增聚合 resync/keyframe request 计数 |
| 审查修正 | Decoder Flush 在清队列前先关闭 `NeedInput` 回调入口，避免旧 input slot 在 Flush 窗口复活；发送与 Receiver 测试改为覆盖生产实际使用的编排 seam，而非只验证孤立辅助函数 |
| 当前自动化门禁 | Release 全量门禁通过：扩展 37/37、Receiver 协议与生命周期 2/2、`npm audit` 高危漏洞 0、静态/隐私检查通过、unsigned HAP 构建成功 |
| 真机边界 | 本行是 `26a8e37` 阶段的历史边界；后续 exact-head 安装与花屏复测结果见下一节 |

原始周期证据仅保存在仓库外，仓库不保存私网地址、配对秘密、网页内容、视频 payload 或签名材料。

### 2026-07-28 `f7fd224` exact-head 安装与花屏复测

本节验收对象是代码提交 `f7fd22492c5df9a68163cca70bfba63eb2adac5e`。后续若仅追加本节文档，
不改变该代码验收对象。

| 证据层 | 结果 |
| --- | --- |
| 签名与覆盖安装 | 仓库外完成签名构建，官方签名工具 `verify-app` 验证通过；与设备既有应用身份一致后仅执行 `install -r` 覆盖安装，未卸载、未清除应用数据 |
| 信任保留 | 覆盖安装后首次安装时间保持不变，Receiver 原生界面确认电脑配对仍在，无需重新扫码或输入连接码 |
| 初始动态播放 | 用户在真实 Edge 动态来源页发送后确认画面正常、无花屏 |
| 停止与重发 | 用户停止发送，等待 2–3 秒后重新发送同一动态来源页；用户确认首帧及后续动态画面均无花屏 |
| 连接状态机证据 | 仓库外持续监控捕获 `STOP` 后媒体连接断开、随后 `RECONNECT`，重连后连续 10 个样本保持 Receiver 进程、单监听和单媒体连接稳定 |
| Receiver 计数边界 | 当前 Release/UI 没有可供本轮可靠只读采集的 Receiver `received`/`decoded` 数值接口，因此不虚构计数增长；连接状态机是机器侧证据，用户可见动态画面是独立视觉证据 |
| 花屏复测结论 | exact `f7fd224` 的“初始发送”和“停止后重发”两个现场均未复现花屏；该结论只覆盖已执行场景，不扩展为任意网络或息屏条件下的普遍保证 |
| 长息屏限制 | 先前长息屏周期仍需重新激活原 Edge 来源页后才恢复播放；原“解锁后自动 5 秒恢复”严格 FAIL，连续 5 次循环未执行。用户明确接受该限制并决定本 Issue 不再继续优化，绝不得把它标记为 PASS |

签名材料与工具输出、设备标识、网络地址、配对秘密、短码、token、网页内容和视频 payload
均未写入仓库。GitHub 没有为本分支报告 CI status check；本节记录的是本地门禁、独立审查、
官方验签和真实设备分层证据，不把它们表述为 GitHub CI。

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
