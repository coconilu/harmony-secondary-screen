# Harmony Secondary Screen

把 HarmonyOS 平板作为非华为 Windows 11 电脑的原生无线扩展屏。

本项目坚持两条边界：

- 平板端使用 ArkTS + HarmonyOS NDK 原生实现，不使用 HTML5，也不依赖 Android APK。
- 第一阶段只考虑可信家庭 Wi-Fi；USB 不承担视频数据传输。

## 当前状态

仓库目前处于 `v0.1` 工程启动阶段，已经提供：

- 可构建的 HarmonyOS 6.1.0+ 接收端应用骨架；
- Windows IddCx 虚拟显示器、编码和传输的架构边界；
- 视频与控制通道的初始协议；
- 首个真实验证基线：HUAWEI MatePad Pro（XYAO-W00），HarmonyOS 6.1.0。

当前代码还不是可用副屏产品。完整的第一版范围与验收标准由 GitHub Issue 和 Project 跟踪。

## 项目管理

- [v0.1 原生 Wi-Fi 扩展屏 MVP](https://github.com/coconilu/harmony-secondary-screen/issues/1)
- [Product & Development Board](https://github.com/users/coconilu/projects/7)

## 目标架构

```text
Windows 11 IddCx 虚拟显示器
        ↓ D3D11 帧
硬件 H.264 低延迟编码
        ↓ 家庭 Wi-Fi
HarmonyOS 原生接收端
ArkTS 控制界面 + C++ AVCodec + XComponent
        ↓
触控事件回传 Windows
```

详细说明：

- [系统架构](docs/ARCHITECTURE.md)
- [传输协议](docs/PROTOCOL.md)
- [兼容性策略](docs/COMPATIBILITY.md)
- [v0.1 里程碑](docs/V0_1_MILESTONE.md)

## 构建接收端

要求：DevEco Studio 6.1.1 Release SDK 或兼容版本。

```powershell
.\scripts\build-receiver.ps1
```

构建产物位于 `receiver\entry\build\default\outputs\default\`。

## 许可证

Apache-2.0
