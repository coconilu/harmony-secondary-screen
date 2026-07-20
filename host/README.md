# Windows Host

| 目录 | 职责 |
| --- | --- |
| `driver/` | IddCx / UMDF 2 独立显示器、D3D11 swap chain 消费与帧队列 |
| `graphics/` | D3D11 BGRA→NV12、Media Foundation H.264 与本机命名管道 |
| `app/` | SCM Host 服务、用户会话输入代理、配对/恢复、UDP 分片与测量日志 |
| `common/` | 跨端线协议与有界解析 |
| `tests/` | 协议/跨端线格式、ACL、网络门禁、显示映射、连接取消策略与编码烟测 |

驱动与 Host Service 运行在 Session 0，通过仅限本机的
`\\.\pipe\HarmonySecondaryScreen.Frames` 传递编码帧。已鉴权触控经
`\\.\pipe\HarmonySecondaryScreen.Input` 转给当前登录用户运行的 `hss_input_agent.exe`，只有后者
调用 `SendInput`。`\\.\pipe\HarmonySecondaryScreen.Status` 只向本机客户端返回配对状态。

Frames 管道使用受保护 DACL：UMDF 的 LocalService 主体只有写权限，LocalSystem Host 和管理员有
完全权限，同时启用 `PIPE_REJECT_REMOTE_CLIENTS`。硬件 H.264 MFT 由独立事件泵分别消费
`METransformNeedInput` / `METransformHaveOutput`，并使用有界输入/输出队列适配预热和多入多出；
运行时错误会重建同步软件 MFT。编码线程入口负责建立 COM MTA。
输入代理使用 `QueryDisplayConfig` 的 adapter device path、驱动 Hardware ID 与固定 target 0 映射
虚拟屏，不依赖本地化或驱动版本可能改变的 `DeviceString`。

构建、签名和运行命令统一维护在仓库根目录 README。
