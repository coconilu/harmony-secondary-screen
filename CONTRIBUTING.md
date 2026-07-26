# Contributing

欢迎提交 Issue 和 Pull Request。

提交代码前请确认：

1. 产品仍是“用户主动选择 Edge 标签页 → HarmonyOS 原生伴随屏”，没有把镜像称为扩展屏。
2. 没有引入 HTML5/WebView/Android APK 接收端。
3. Edge 扩展只请求实现当前功能所需的最小权限；没有无依据的 `<all_urls>` 或页面脚本注入。
4. 没有采集或向平板传输音频，Windows 本地音频路径保持不变。
5. 没有引入 Windows 驱动、系统服务或管理员安装要求。
6. 没有把设备型号或具体视频网站写成业务逻辑条件。
7. 变更带有可重复的验证步骤；涉及协议时已经同步更新 `docs/PROTOCOL.md`。
8. 安全默认值仍只允许用户确认的可信局域网。

## 本地验证

提交前运行：

```powershell
.\scripts\test.ps1 -Configuration Release
```

默认 DevEco Studio 根目录是 `C:\Program Files\Huawei\DevEco Studio`。非默认安装可通过
`HSS_DEVECO_ROOT` 指定；不要把个人绝对路径、SDK、Hvigor 包或签名材料提交到仓库。Receiver 构建工具
及 Hvigor、npm、ohpm 缓存位于已忽略的 `out/harmony-build-tools/`，不会修改用户全局 cache。
