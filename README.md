# Harmony Web Companion

把用户明确选择的 Microsoft Edge 标签页画面发送到 HarmonyOS 6.1+ 平板，作为“网页伴随屏”使用。
音频不采集、不传输，继续由 Windows 输出到用户原有耳机。

它不是 Windows 扩展屏、虚拟显示器、远程桌面或桌面镜像。

## v0.1 用户安装

| 设备 | 只需安装 |
| --- | --- |
| Windows 11 | Edge Manifest V3 扩展 |
| HarmonyOS 平板 | 原生 Receiver HAP |

正常用户路径没有 Windows Relay、Native Host、服务、驱动、管理员安装或防火墙修改。

## 首次配对与日常使用

```text
平板打开 Receiver
  → 用户确认检测到的私网 Wi-Fi IPv4
  → 点击“扫码配对”
Edge 扩展
  → 显示 60 秒、单次使用二维码
平板扫描二维码
  → Edge 直连 Receiver WebSocket
  → 两端保存与 IP 无关的 deviceId / 设备凭据

后续：打开 Receiver → 在目标标签页点击“发送当前标签页”
```

- 摄像头不可用：在平板输入扩展显示的六位一次性短码。
- `.local` 解析失败：在扩展输入 Receiver 显示的私网 IPv4。
- IP 变化只更新连接地址，不清除配对身份。
- 两端均可“忘记设备”。
- 不扫描局域网，不连接公网。

HarmonyOS 使用公共 DNS-SD API 注册 `_hwc._tcp` 服务实例。是否能在目标 Windows/Edge 环境中把它
稳定解析为裸 `harmony-web-companion.local`，仍需真实设备验证；服务注册成功不等于裸主机名必然
可解析，因此手动私网 IPv4 是正式回退路径。

## 实现架构

```text
Edge tabCapture（video only）
  → offscreen document
  → WebCodecs H.264 Annex-B 1280×720 @ 60 fps（目标 8 Mbps，实测帧率以监控页为准）
  → ws://<Receiver>:44000/direct
  → HarmonyOS C++ WebSocket server
  → OH_VideoDecoder
  → XComponent Surface
```

协议在同一已鉴权 WebSocket 中承载配对、控制、心跳、关键帧请求、遥测和 H.264 二进制消息。
`sourceEpoch` 保证旧页面迟到帧不会覆盖最新来源；任意时刻仅有一个捕获轨、编码器和媒体流。

## 当前验证状态

| 项目 | 状态 |
| --- | --- |
| 扩展直连协议与逐字节 Annex-B 假 Receiver 测试 | 自动化通过 |
| Receiver WebSocket 握手、二进制头和 source epoch 单元测试 | 自动化通过 |
| HarmonyOS targetSdk 24 原生构建 | 通过 |
| 无签名配置的可移植构建 | 通过；真机安装需开发者在本机配置签名 |
| 扫码、直连、首帧、重启后信任、IP 变化重连 | **真实 Edge + 平板未验证** |
| 裸 `.local` 在 Windows/Edge 的解析 | **未验证**；保留手动 IP |
| 30 分钟、时延、丢包与恢复 | 由 #3 继续验收 |

旧 `relay/`、`host/` 和相关脚本只保留为历史取证/开发对照，不属于构建、安装或运行依赖。

## 构建与测试

```powershell
.\scripts\test.ps1
```

该命令执行扩展测试与依赖审计、Receiver 直连协议单元测试、静态安全检查和 HarmonyOS 原生构建。
默认从 `C:\Program Files\Huawei\DevEco Studio` 读取 DevEco Studio；若安装在其他位置，先设置项目专用
环境变量：

```powershell
$env:HSS_DEVECO_ROOT = 'D:\Huawei\DevEco Studio'
.\scripts\test.ps1 -Configuration Release
```

脚本读取 DevEco 自带插件版本，并把完全同版的 Hvigor 引擎与插件安装到已忽略的
`out/harmony-build-tools/`。它不会修改 DevEco 安装目录或用户全局 Hvigor cache；首次构建需要访问
HarmonyOS 官方 npm 仓库，后续复用项目隔离工具链。

Receiver 仓库配置不包含证书、私钥、口令或本机绝对签名路径。本地仍有效的旧签名材料应由拥有者
轮换；本 Issue 不重写 Git 历史。

## 文档

- [系统架构](docs/ARCHITECTURE.md)
- [HWC4 直连协议](docs/PROTOCOL.md)
- [真实设备测试](docs/REAL_DEVICE_TEST.md)
- [兼容性策略](docs/COMPATIBILITY.md)
- [Edge 扩展说明](extension/README.md)

## 许可证

Apache-2.0
