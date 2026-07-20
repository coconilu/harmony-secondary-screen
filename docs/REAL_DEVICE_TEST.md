# v0.1 真实设备验收

本文定义测试方法，不包含伪造或推算的通过结果。只有真实 Windows 11 + HarmonyOS 平板记录完成后，
才能勾选 Issue #1 中的稳定性、延迟、丢帧和触控精度验收项。

## 记录模板

| 项目 | 实测值 |
| --- | --- |
| 日期/操作者 | 待填写 |
| Windows 版本 / GPU / 编码 MFT | 待填写 |
| 驱动 commit / 签名类型 | 待填写 |
| 平板型号 | 待填写（首个基线为 XYAO-W00） |
| HarmonyOS / API | 待填写（最低 6.1.0 / API 23） |
| 会话分辨率 / 帧率 | 待填写 |
| AP 型号 / 频段 / 信道 / 距离 | 待填写 |
| 平均码率 | 待填写 |
| P50 / P95 端到端延迟 | 待填写 |
| 30 分钟编码帧 / 解码帧 / 丢弃帧 | 待填写 |
| 断连恢复耗时 | 待填写 |
| 点击/拖动最大 X/Y 误差 | 待填写 |
| 崩溃、黑屏、花屏、异常日志 | 待填写 |

## 前置门禁

1. `scripts/test.ps1 -Configuration Release` 通过。
2. `scripts/build-host.ps1 -Configuration Release` 在安装 WDK 的干净环境通过。
3. 测试签名驱动在隔离 Windows 11 测试机安装成功，“显示设置”出现独立显示器并选择“扩展这些显示器”。
4. 主屏和平板显示不同内容，窗口能跨屏拖动；不得使用复制、投屏镜像或远程桌面。
5. 保持可信家庭 Wi-Fi 为 Windows 默认分类，运行 Host 网络授权入口，必须显示网络名称与风险说明；
   确认后记录应用自己的 Wi-Fi profile ID，不得调用 `INetwork::SetCategory`。拒绝时 Host 必须保持关闭。
6. VPN 保持连接，确认 Host 只监听已确认的物理 Wi-Fi IPv4，不监听 VPN 或以太网地址，且 VPN 与
   Windows 网络分类均未被修改。
7. 检查 Windows 防火墙规则 `HarmonySecondaryScreenHost-Control`：必须同时限定 `Wireless`、
   `LocalSubnet`、Host 程序路径与 TCP 47100；允许跨 Windows profile 生效，但不得存在全接口或 UDP
   入站放行，实际 listener 必须仅绑定应用已确认的 Wi-Fi IPv4。
8. 连接建立后切换到另一个未确认的 Wi-Fi，Host 必须关闭 listener、立即撤销现有 session，并停止
   UDP 视频；从 `wifi_not_allowed` 错误出现起抓包确认新增 UDP 数据包数为 0。服务模式重启后也只能
   使用安装时确认的 profile ID，不得在 Session 0 弹出交互窗口。

## 30 分钟稳定性与丢帧

1. 删除旧的 `hss-host-frames.csv` 和 `hss-receiver-telemetry.csv`。
2. 启动 Host 与 Receiver，确认 `1920×1200 @ 60 Hz` 会话。
3. 连续循环执行窗口拖动、网页滚动、文本输入、静态桌面和 60 fps 动画场景 30 分钟。
4. 保留两份 CSV、Host 控制台、`hilog`、Windows 事件日志和录屏/相机视频。
5. 以 Receiver 的 `framesDecoded`/`framesDropped` 计算
   `drop_rate = dropped / (decoded + dropped)`；必须 `<1%`。
6. 期间不得崩溃、永久黑屏或人工重连。瞬时丢包后必须由关键帧自动恢复。

## 端到端延迟

软件 CSV 的 `end_to_end_us` 只用于定位管线，不作为最终验收。最终结果使用 240 fps 或更高帧率相机，
同画面拍摄 Windows 上由鼠标/键盘触发的高对比状态变化与平板显示变化。每个交互场景至少 100 次，
逐次记录相机帧差，换算毫秒后报告 P50/P95；P95 必须 `<=100 ms`。原始逐次数据与相机帧率、
计算脚本/公式一起提交到 `docs/test-results/<date>-<commit>/`。

## 断连恢复

会话稳定后短暂关闭 AP 无线或屏蔽网络 1 秒，再恢复；从链路恢复到平板重新显示移动画面的时间必须
`<=5 s`，且不重新输入配对码。重复至少 10 次，记录每次耗时和是否重新请求关键帧。超过 Host 的
5 秒会话窗口后要求新配对码属于安全设计，不计作自动恢复通过。

## 触控精度与滚动

在扩展屏显示包含四角、中心与网格交点的校准页。对每个点点击和拖动至少 10 次，记录 Windows 实际
指针坐标与目标坐标：`abs(dx)/1920` 和 `abs(dy)/1200` 均须 `<=1%`。切到滚动模式分别上滑、下滑，
确认 Windows 内容方向符合自然手势设计，并记录视频证据。

## 结果落库规则

- 原始 CSV、逐次延迟/触控数据、日志摘要和设备环境必须一起提交；
- 不得把模板、模拟数据、单元测试或软件估算标为实机结果；
- 任一必填项缺失时，对应 Issue 验收框保持未勾选。
