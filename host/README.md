# Windows 旧原型与迁移说明

本目录当前主要保存旧的 IddCx 真扩展屏原型。自 ADR-001 起，它不再代表 v0.1 网页投屏 / TabReach 的目标
Windows 实现。

| 目录 | 当前状态 | 迁移方向 |
| --- | --- | --- |
| `driver/` | 旧 IddCx / UMDF 2 虚拟显示驱动 | 归档后删除 |
| `graphics/` | 旧 D3D11 / Media Foundation 桌面编码器 | 归档后删除 |
| `app/` | 旧 SCM Host、网络门禁和输入代理 | 只复用安全/网络思路；改写为普通用户态 Relay |
| `common/` | 协议 v1 与边界解析 | 升级为协议 v2 |
| `tests/` | 旧协议、驱动和输入测试 | 保留可复用边界测试，删除失去职责的测试 |

新 Windows 端由两部分组成：

1. Edge Manifest V3 扩展：捕获当前标签页视频并用 WebCodecs 编码；
2. 普通用户态 Relay：通过受限本地桥接接收编码帧，主动连接 HarmonyOS Receiver。

新路径不得注册 Windows Service、安装显示驱动、创建局域网入站防火墙规则、调用 `SendInput` 或要求
管理员权限。

迁移完成前，旧代码和测试可以继续用于回归取证，但构建通过不能声称网页投屏 / TabReach 已实现。目标架构和
删除边界见 `docs/ADR-001-WEB-COMPANION-PIVOT.md`。
