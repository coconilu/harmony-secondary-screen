# 系统架构

## 产品目标

让非华为 Windows 11 电脑把 HarmonyOS 平板识别为真正的第二块显示器，并通过家庭 Wi-Fi 传输低延迟画面。平板端必须是原生 HarmonyOS 应用。

## 组件边界

| 组件 | 职责 | v0.1 技术方向 |
| --- | --- | --- |
| Windows Virtual Display | 向 Windows 枚举真实的扩展显示器 | IddCx / UMDF |
| Frame Pipeline | 从虚拟显示 SwapChain 取得 D3D11 帧 | IddCx SwapChainProcessor |
| Encoder | 低延迟压缩桌面帧 | Media Foundation H.264 硬件编码 |
| Host Service | 可信物理 Wi-Fi 监听、一次性配对、UDP 视频与帧日志 | Win32 SCM 服务（Session 0） |
| Input Agent | 把已鉴权触控注入当前桌面 | Win32 用户会话进程 + SendInput |
| Transport | 局域网配对、视频和控制消息 | 手动 IPv4 + TCP 控制 + UDP 视频；mDNS 为后续目标 |
| Harmony Receiver | 连接、解码、渲染、状态展示 | ArkTS + NDK AVCodec + XComponent |
| Input Return | 将触摸映射回扩展屏坐标 | TCP 控制通道 + Windows SendInput |

## 数据流

```text
Windows DWM
   ↓
IddCx 虚拟显示器（1920×1200 / 60 Hz）
   ↓ D3D11 texture
H.264 编码器（低延迟，无 B 帧）
   ↓ 仅本机命名管道
Windows Host Service（可信物理 Wi-Fi 门禁）
   ↓ UDP 分片
HarmonyOS 接收端抖动缓冲
   ↓ H.264 Annex-B
OH_VideoDecoder
   ↓ Surface
XComponent
```

触控走反向通道：

```text
ArkUI 触摸坐标 → 归一化坐标 → TCP 控制消息 → Host Service
  → 仅本机输入管道 → 用户会话 Input Agent → Windows 屏幕坐标 → SendInput
```

IddCx 驱动和 Host Service 运行在 Session 0；`SendInput` 只在用户会话 Input Agent 中执行，避免
Session 0 输入隔离。驱动只把编码后的 H.264 帧写入拒绝远程客户端的本机命名管道。Host Service
提供另一个只限本机的只读状态管道，用于查询当前一次性配对码。

## 架构决策

### 原生接收端

接收端不使用 WebView、HTML5 Viewer 或 Android 兼容应用。ArkTS 只负责生命周期、连接与设置；视频收包、抖动缓冲和 AVCodec 解码由 C++ 完成。

AVCodec 生命周期与回调队列使用两把独立锁。`Stop`、`Destroy`、`Flush` 等可能等待回调结束的 API 调用期间不持有回调队列锁；
`Flush` 成功后必须再次 `Start`，失败则重建解码器，并等待携带 SPS/PPS 的 IDR 后恢复解码。

### 异步硬件编码

IddCx 编码线程在入口建立 COM MTA。硬件 MFT 使用独立事件泵维护 `METransformNeedInput` 与 `METransformHaveOutput` credit，
输入和输出分别使用有界队列，不假设一次输入必然同步产生一次输出；运行错误时才切换到同步软件 MFT。

### Wi-Fi only

v0.1 只支持可信家庭 Wi-Fi。USB 可用于供电，但不属于视频传输链路。这个边界减少设备模式、驱动权限和型号差异带来的不确定性。

### 可信物理 Wi-Fi 门禁

Host 先用 `GetAdaptersAddresses` 将候选接口限制为 `IF_TYPE_IEEE80211`，再通过 Windows Network List
Manager 把 Wi-Fi 适配器 GUID、网络配置文件与 IPv4 地址关联；以太网和 VPN 虚拟网卡不会成为监听候选。
如果家庭 Wi-Fi 被 Windows 默认标记为“公用”，交互式启动或服务安装会展示网络名称与风险说明；用户
明确确认后，受限的 UAC 子进程只对该网络调用 `INetwork::SetCategory(Private)`。拒绝或取消授权时继续阻止监听。
由于 `SetCategory` 作用于整个 `INetwork`，自动修改前必须枚举其全部 connection；只要发现 VPN、以太网
或无法映射的连接与 Wi-Fi 聚合，就拒绝修改，避免连带改变非 Wi-Fi 网络。

Host 只在确认后的具体 Wi-Fi IPv4 上监听 TCP。每次 bind、accept 和已连接会话循环都会重新验证接口
类型与分类；运行中切到“公共”会先撤销会话数据面 epoch、停止后续 UDP 分片，再关闭 listener 和现有
控制连接，不会退化为全接口绑定。服务安装器只为 Host 程序创建 `Private + Wireless + LocalSubnet +
TCP 47100` 入站防火墙规则，并在卸载时清理；视频 UDP 由 Host 向已配对平板发起，不创建 UDP 入站规则。

### 型号不是业务分支

运行时根据 API 版本、编解码能力、屏幕参数和网络条件协商能力。`XYAO-W00` 只是首个实机验证样本。

## 非目标

- 公网远程桌面；
- DRM/受保护视频捕获；
- USB 视频传输；
- 音频、手写笔压感和多平板；
- Windows 10、macOS、Android 或 iPadOS。
