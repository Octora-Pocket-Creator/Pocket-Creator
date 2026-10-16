<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

> **由发布负责人维护：** 普通功能、应用和文档 PR 不修改本文件。创建发布 tag 前，
> 发布负责人检查已合并的用户可见变化，并与 `CHANGELOG.md` 同步更新。当前
> `Unreleased` 下的条目是下次发布的待核对内容，不是已经定稿的发布历史。

## Unreleased

## v1.0.1-pockigo-pomodoro - 2026-10-16

- **README 字标。** PockiGo 品牌图已按图形本身裁切：去掉透明留白（上 111 px、下 42 px、
  左 29 px、右 16 px），不再出现字标周围的空白框；文件尺寸为 2090 × 583。
- **仓库检查在 Linux/GCC 下通过。** `tests/test_bsp_battery_math.c` 与 `main/demo_ble.c`
  现在显式包含 `<stdint.h>`。glibc 版 GCC 不像 macOS SDK 那样间接提供
  `int64_t`/`uint8_t`，因此静态门禁（`./tools/validate.sh --static`）在 Ubuntu 上失败、
  在 macOS 上通过；现在两套工具链都通过。

## v1.0.0-pockigo-pomodoro - 2026-10-16

PockiGo 基线的首个发布，面向 ESP32-S3R8 掌机。

- **新的产品基线。** 只保留一套板级配置：ESP32-S3R8（16 MB Flash + 8 MB Octal
  PSRAM）、240 × 320 ST7789 屏幕、六个独立 GPIO 按键（上、下、A、B、左、右，没有
  独立的 OK 键）、无 codec 的直连 I2S 功放输出，以及由 GPIO1 上 200K/200K 分压
  估算的电池电压。板上没有 I2C 从设备。
- **产物重命名。** CMake 工程与所有发布文件统一为 `PockiGo`：可直刷的合并镜像为
  `build/PockiGo-full.bin`，调试归档保留 `PockiGo.elf` / `PockiGo.map`。合并镜像
  从 `0x0` 烧录。旧的 `Pocket-Creator-full.bin` 名称不再使用。
- **应用。** `main` 构建番茄钟应用（build tag `1.0.0-s3`），自带 RGB565 画布渲染与
  中文字形表。旧的硬件测试页（`demo_*.c`、`ui_pixel.*`）保留在仓库里但不参与编译；
  新应用必须实现自己的 UI。
- **渲染路径策略。** 常规应用建议通过 `espressif/esp_lvgl_port` 使用 LVGL；只有像
  「小鸡快跑」那样追求高刷新率的全屏渲染，才直接用 `esp_lcd_panel_draw_bitmap()`
  绘制。两条路径共用同一套 BSP 显示 API 与圆角遮罩工具。
- **移除已下线的 ESP32-C3 配置。** 删除其 codec 与电量计代码、低功耗/电池演示、主机
  测试桩，以及固件校验中的相关表（8 MB Flash、`esp32c3` 芯片标识、RISC-V ELF）。
  固件校验现在只接受 16 MB 的 ESP32-S3 镜像。
- **文档。** 所有维护中的文档都是中英配对；产品规格、硬件指南与能力表只描述
  ESP32-S3 硬件；README 中英首页加入 PockiGo 字标（透明背景已裁到图形边缘），
  并链接到 <https://pocket.takecloud.cn>。
- **验证。** `./tools/validate.sh` 会执行仓库检查、主机测试、固件构建与合并镜像
  校验；推送 `v*` tag 会构建固件并把 `PockiGo-full.bin` 作为 release 发布。
