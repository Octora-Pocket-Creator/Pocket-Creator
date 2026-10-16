<p align="right">
  <a href="specifications.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Product Specifications

This page defines the user-facing product specifications. Firmware pin assignments, buses, and runtime constraints are documented in the [hardware guide](s3r8-board.md) and `components/bsp/include/bsp_pins.h`.

| Item | Specification |
| --- | --- |
| Form | Handheld console with a blue enclosure |
| Dimensions | 60 × 95 × 8.5 mm |
| Weight | 100 g |
| MCU | ESP32-S3R8 with 16 MB Flash and 8 MB Octal PSRAM |
| Display | ST7789/ST7789P3, 240 × 320 color TFT, RGB565, SPI2 at 40 MHz, square corners |
| Backlight | GPIO13, active low, LEDC PWM dimming |
| Wireless | 2.4 GHz Wi-Fi 802.11 b/g/n; Bluetooth 5 LE |
| NFC | Passive NTAG213 tag supporting ordinary NDEF read/write |
| Function input | Six discrete GPIO keys, active low, no separate OK key: UP 38, DOWN 21, A 16, B 8, LEFT 39, RIGHT 14 |
| Power input | Dedicated hardware power button |
| Power behavior | Hold power for 0.5 seconds to start and about 2 seconds to shut down; press a function button to wake after automatic screen-off |
| Audio | Direct I2S amplifier output (no codec, no MCLK): BCLK 41, WS 42, DOUT 40. ZTS6672 microphone footprints are reserved on the board |
| Charging | USB Type-C 2.0, 5 V input |
| Battery | Built-in 520 mAh rechargeable lithium battery, estimated from a 1:1 divider (200K/200K) on GPIO1 (ADC1_CH0). Full 4.1 V, usable empty 3.3 V; no fuel-gauge chip |
| Console | USB Serial/JTAG (native USB, GPIO19/20) |
| Build target | `esp32s3` (repository default) |

The full pin map, resource ownership, and validation status are in the [S3R8 board profile](s3r8-board.md). Firmware pin assignments and board constants are defined only in `components/bsp/include/bsp_pins.h`; do not duplicate them here or in application code.

Enclosure finish, dimensions, weight, battery capacity, and charging behavior are product-level facts: revisit them together with each hardware revision.
