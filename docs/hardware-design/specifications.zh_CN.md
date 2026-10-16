<p align="right">
  <strong>简体中文</strong> · <a href="specifications.md">English</a>
</p>

# 产品规格（Specifications）

本文定义面向用户的产品规格。固件引脚、总线和运行约束见[硬件开发指南](s3r8-board.zh_CN.md)和 `components/bsp/include/bsp_pins.h`。

| 项目 | 规格 |
| --- | --- |
| 形态 | 掌机、蓝色外壳 |
| 尺寸 | 60 × 95 × 8.5 mm |
| 重量 | 100 g |
| MCU | ESP32-S3R8，16 MB Flash，8 MB Octal PSRAM |
| 显示 | ST7789/ST7789P3，240 × 320 彩色 TFT，RGB565，SPI2 40 MHz，四角为直角 |
| 背光 | GPIO13，低电平点亮，LEDC PWM 调光 |
| 无线 | 2.4 GHz Wi-Fi 802.11 b/g/n；Bluetooth 5 LE |
| NFC | 被动 NTAG213，支持普通 NDEF 读写 |
| 功能输入 | 六个独立 GPIO 按键，低电平按下，没有独立的 OK 键：上 38、下 21、A 16、B 8、左 39、右 14 |
| 电源输入 | 独立硬件电源键 |
| 电源行为 | 按住电源键 0.5 秒开机、约 2 秒关机；自动息屏后按功能键唤醒 |
| 音频 | 直连 I2S 功放输出（无 codec、无 MCLK）：BCLK 41、WS 42、DOUT 40。板上预留 ZTS6672 麦克风焊盘 |
| 充电 | USB Type-C 2.0，5 V 输入 |
| 电池 | 内置 520 mAh 可充电锂电池，由 GPIO1（ADC1_CH0）上的 200K/200K 1:1 分压估算。满电 4.1 V、可用空电 3.3 V；无电量计芯片 |
| 控制台 | USB Serial/JTAG（原生 USB，GPIO19/20） |
| 构建目标 | `esp32s3`（仓库默认） |

完整引脚表、资源所有权与验收状态见 [S3R8 板级基线](s3r8-board.zh_CN.md)。固件引脚与板级常量只在 `components/bsp/include/bsp_pins.h` 定义，不要在这里或应用代码里复制。

外壳工艺、尺寸、重量、电池容量与充电行为属于产品级事实：每次硬件改版都要一并确认。
