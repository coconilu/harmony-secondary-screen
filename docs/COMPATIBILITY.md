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

Windows Host 只绑定物理 Wi-Fi IPv4，不会把 VPN 虚拟网卡当作传输接口。VPN 可以保持连接，但其
“阻止局域网”或 Kill Switch 策略仍可能拦截平板流量；这种情况下需要在 VPN 客户端中允许本地网络访问，
不能通过放宽 Host 到公共接口来绕过。如果 Windows 把 VPN 与公用 Wi-Fi 聚合为同一个网络对象，Host
会拒绝自动修改整个对象的类别；可暂时断开 VPN，完成一次 Wi-Fi 信任后再重新连接。

扩大到更低 HarmonyOS 版本需要单独的构建和实机证据，不能仅根据编译成功宣称兼容。
