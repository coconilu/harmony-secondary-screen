# Legacy Windows Relay（仅开发回退）

该目录保存 HWC2/UDP 旧链路，用于历史取证和与 #3 基线对比。

- 不属于 v0.1 用户安装；
- 不由 `scripts/test.ps1` 构建；
- Edge 扩展不调用 Native Messaging；
- HWC4 正常路径不启动本进程；
- 不得把旧 Relay 测试结果标为直连链路通过。

确需开发者对照旧基线时，可显式执行 `scripts/build-relay.ps1`。相关 Native Host 安装脚本仅用于旧
开发环境，不应提供给普通用户。
