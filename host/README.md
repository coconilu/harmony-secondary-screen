# Windows Host

| 目录 | 职责 |
| --- | --- |
| `driver/` | IddCx / UMDF 2 独立显示器、D3D11 swap chain 消费与帧队列 |
| `graphics/` | D3D11 BGRA→NV12、Media Foundation H.264 与本机命名管道 |
| `app/` | SCM Host 服务、用户会话输入代理、配对/恢复、UDP 分片与测量日志 |
| `common/` | 跨端线协议与有界解析 |
| `tests/` | 协议单元测试和 Host/Receiver 线格式兼容测试 |

驱动与 Host Service 运行在 Session 0，通过仅限本机的
`\\.\pipe\HarmonySecondaryScreen.Frames` 传递编码帧。已鉴权触控经
`\\.\pipe\HarmonySecondaryScreen.Input` 转给当前登录用户运行的 `hss_input_agent.exe`，只有后者
调用 `SendInput`。`\\.\pipe\HarmonySecondaryScreen.Status` 只向本机客户端返回配对状态。

构建、签名和运行命令统一维护在仓库根目录 README。
