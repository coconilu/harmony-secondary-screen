# Windows IddCx 驱动

本目录是 Windows 11 x64 的 UMDF 2 间接显示驱动。它只枚举一块
`1920×1200 @ 60 Hz` 显示器，从 IddCx swap chain 获取 D3D11 Surface，复制到深度为 2
的低延迟队列，再经 D3D11 VideoProcessor 与 Media Foundation H.264 编码后写入本机命名管道。

驱动基于 Microsoft 官方 IddCx 模型和 `IddSample` 的对象/回调生命周期，不包含镜像、
远程桌面或屏幕抓取替代路径。

构建与测试签名步骤见仓库根目录 README。生产分发必须使用正式驱动签名；测试签名只允许在
隔离测试机上使用。

`IDDCX_MONITOR_INFO` 初始化与 EDID-less monitor 模式以微软
[IddSample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay/IddSampleDriver)
及 [IDDCX_MONITOR_INFO 文档](https://learn.microsoft.com/windows-hardware/drivers/ddi/iddcx/ns-iddcx-iddcx_monitor_info)
为核对基线。当前机器缺少 WDK，因此静态核对不能替代真实 WDK 编译和安装。
