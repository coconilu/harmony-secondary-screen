# 兼容性策略

## 原则

产品不按型号判断功能，而是按运行时能力协商：

- HarmonyOS API 版本；
- `video/avc` 硬件解码能力；
- Surface 输出能力；
- 屏幕分辨率与刷新率；
- Wi-Fi 实际吞吐、抖动和丢包率。

型号用于记录可重复的测试证据，而不是控制业务逻辑。

## 首个验证基线

| 项目 | 值 |
| --- | --- |
| 设备 | HUAWEI MatePad Pro |
| 型号 | XYAO-W00 |
| 系统 | HarmonyOS 6.1.0 |
| API 基线 | API 23 |
| 网络 | 家庭 5 GHz Wi-Fi |
| 数据传输 | Wi-Fi only |

## 初始支持范围

- HarmonyOS 6.1.0 / API 23 及以上平板；
- 支持 H.264 Surface 硬件解码；
- 与 Windows 11 主机位于同一可信局域网。

扩大到更低 HarmonyOS 版本需要单独的构建和实机证据，不能仅根据编译成功宣称兼容。
