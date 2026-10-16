<p align="right">
  <strong>简体中文</strong> · <a href="wifi-provisioning.md">English</a>
</p>

# 通过蓝牙配网连接 Wi-Fi

当用户需要设备通过 Wi-Fi 连接网络时，可以参考
[`demo/blufi-provisioning` 分支](https://github.com/Octora-PockiGo/PockiGo/tree/demo/blufi-provisioning)
的代码实现蓝牙配网。手机通过 BLE 上的 BLUFI 协议发送 Wi-Fi 名称（SSID）和
密码，设备以 Wi-Fi STA 模式连接网络并回报连接状态。蓝牙用于传递配网信息，
不是承载应用的互联网流量。

这是可选的应用参考。当前 `main` 的 Wi-Fi demo 仅扫描网络，并未实现联网或
蓝牙配网；没有联网需求的应用不必启用网络功能。

## 配网流程

参考固件的蓝牙广播名为 `BLUFI_PocketPort`。预期流程：设备进入配网状态，
手机打开蓝牙并授予所需权限，用配网客户端连接目标设备，提供 2.4 GHz Wi-Fi
的名称和密码。确认设备取得 IP 地址后，再验证应用实际需要的网络请求；
仅蓝牙连接成功不能证明能够访问互联网。

配套配网小程序暂不纳入本基线：不要记录或依赖特定客户端应用，保持设备侧
BLUFI 服务可被任意兼容的 BLE 配网客户端使用。

## 代码参考入口

- [`main/demo_blufi.c`](https://github.com/Octora-PockiGo/PockiGo/blob/demo/blufi-provisioning/main/demo_blufi.c)：BLUFI 回调、Wi-Fi 扫描与连接、凭据处理、状态回报和生命周期。
- [`main/demo_blufi_security.c`](https://github.com/Octora-PockiGo/PockiGo/blob/demo/blufi-provisioning/main/demo_blufi_security.c) 及其头文件：BLUFI 安全协商回调，提取示例时不要丢弃这些逻辑。
- [`main/demo_radio.c`](https://github.com/Octora-PockiGo/PockiGo/blob/demo/blufi-provisioning/main/demo_radio.c)：NVS、`esp_netif` 和默认事件循环的共享初始化。
- [`main/CMakeLists.txt`](https://github.com/Octora-PockiGo/PockiGo/blob/demo/blufi-provisioning/main/CMakeLists.txt) 和 [`sdkconfig.defaults`](https://github.com/Octora-PockiGo/PockiGo/blob/demo/blufi-provisioning/sdkconfig.defaults)：源文件注册、依赖、NimBLE/BLUFI 和加密配置。不能只复制单个 C 文件就认为完成接入。

## 接入与验收

从应用当前基线出发，记录参考提交，仅移植适用的联网逻辑。不要直接合并整个
demo，也不要用该分支的旧版 BSP、分区表或配置覆盖当前版本。重新实现应用
自己的界面，将配网状态、任务及 Wi-Fi/BLE 服务放在应用层。

明确配网入口与退出、超时、重试上限、凭据持久化及主动清除凭据的方式。
禁止记录或提交 Wi-Fi 密码。保留 LVGL 锁、非阻塞回调、任务与事件处理器清理
机制，并评估 Wi-Fi、BLE 和 UI 同时运行的内部 RAM 占用。示例不等于完整的
生产环境授权或安全设计。

运行[验证门禁](build-and-test.zh_CN.md)，再遵循
[真机测试交接流程](../ai-guide.zh_CN.md#主动询问真机测试)。端到端验证配网：
发现设备、成功配网、密码错误与网络不可用、重连与重启、清除凭据、反复
进入退出配网，以及应用实际需要的网络请求。编译成功不能证明配网兼容性
或实机联网正常；未执行的检查列入 `Unverified`。
