# 系统架构

## 产品目标

让非华为 Windows 11 电脑把 HarmonyOS 平板识别为真正的第二块显示器，并通过家庭 Wi-Fi 传输低延迟画面。平板端必须是原生 HarmonyOS 应用。

## 组件边界

| 组件 | 职责 | v0.1 技术方向 |
| --- | --- | --- |
| Windows Virtual Display | 向 Windows 枚举真实的扩展显示器 | IddCx / UMDF |
| Frame Pipeline | 从虚拟显示 SwapChain 取得 D3D11 帧 | IddCx SwapChainProcessor |
| Encoder | 低延迟压缩桌面帧 | Media Foundation H.264 硬件编码 |
| Transport | 局域网发现、配对、视频和控制消息 | mDNS + TCP 控制 + UDP 视频 |
| Harmony Receiver | 连接、解码、渲染、状态展示 | ArkTS + NDK AVCodec + XComponent |
| Input Return | 将触摸映射回扩展屏坐标 | TCP 控制通道 + Windows SendInput |

## 数据流

```text
Windows DWM
   ↓
IddCx 虚拟显示器（1920×1200 / 60 Hz）
   ↓ D3D11 texture
H.264 编码器（低延迟，无 B 帧）
   ↓ UDP 分片
HarmonyOS 接收端抖动缓冲
   ↓ H.264 Annex-B
OH_VideoDecoder
   ↓ Surface
XComponent
```

触控走反向通道：

```text
ArkUI 触摸坐标 → 归一化坐标 → TCP 控制消息 → Windows 屏幕坐标 → SendInput
```

## 架构决策

### 原生接收端

接收端不使用 WebView、HTML5 Viewer 或 Android 兼容应用。ArkTS 只负责生命周期、连接与设置；视频收包、抖动缓冲和 AVCodec 解码由 C++ 完成。

### Wi-Fi only

v0.1 只支持可信家庭 Wi-Fi。USB 可用于供电，但不属于视频传输链路。这个边界减少设备模式、驱动权限和型号差异带来的不确定性。

### 型号不是业务分支

运行时根据 API 版本、编解码能力、屏幕参数和网络条件协商能力。`XYAO-W00` 只是首个实机验证样本。

## 非目标

- 公网远程桌面；
- DRM/受保护视频捕获；
- USB 视频传输；
- 音频、手写笔压感和多平板；
- Windows 10、macOS、Android 或 iPadOS。
