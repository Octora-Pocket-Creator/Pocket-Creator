[English](README.md) · **简体中文**

<p align="center">
  <a href="https://pocket.takecloud.cn">
    <img src="assets/images/pockigo-logo.png" alt="PockiGo" width="360">
  </a>
</p>

# PockiGo 固件

开源 DIY 掌机固件：ESP32-S3R8、240×320 屏幕、六个实体按键、一路扬声器输出。
写一个应用，编译，通过 USB 烧录，把自己的作品带在身边。

这个仓库包含做出一个应用所需的全部东西：板级支持层、一个完整的应用示例、主机测试、
构建门禁，以及指导 AI Agent 开发的技能包。

---

## 这里有什么

| | |
| --- | --- |
| **开放可改** | 固件、板级支持与示例都摆在这里，供你改造。可以 fork 出去，可以砍到只剩底座，也可以在上面另起一个东西。 |
| **离线运行** | 这里没有任何东西依赖网络。基于这套底座做出来的应用可以独立运行。 |
| **为 Agent 而准备** | `AGENTS.md` 加上 `skills/` 技能包，把硬件事实、构建命令和开发规则交给编程 Agent，让它照着真板子开发，而不是靠猜。 |

## 开始开发

1. 在 AI 编程工具里打开这个仓库，让它先读 [`AGENTS.md`](AGENTS.md)。
2. 让它安装[必需的技能](skills/README.md)——共五个，覆盖环境准备、开发、构建、设备测试与调试。
3. 描述你想做的东西。

一段可用的提示词：

```text
为 PockiGo 开发一个离线的习惯打卡应用。
使用六个实体按键和 240×320 屏幕，并在断电后保留记录。
从 `main` 开始，新建 `feature/*` 分支，在那里开发。
遵循 AGENTS.md 与 docs/hardware-design/s3r8-board.md。
硬件逻辑放在 components/bsp，应用逻辑放在 main。
交付可运行的实现与测试；分别报告构建结果、未执行的设备验证项，以及实机验收步骤。
重新设计应用界面；不要沿用当前的测试菜单或界面。
```

> **编译成功不等于硬件验证通过。** 完成前请在真机上测试，并先读[烧录说明](docs/development/engineering/firmware-layout.md)
> ——从 `0x0` 写入合并镜像会覆盖已存数据，包括 NVS。

## 构建与验证

```bash
source <esp-idf>/export.sh          # ESP-IDF 5.4 或 5.5
./tools/validate.sh                 # 主机测试 + 固件布局 + 合并镜像
./tools/validate.sh --firmware      # 只构建
```

门禁产出 `build/PockiGo-full.bin`，即从 `0x0` 烧录的单个合并镜像。
它**不是逐字节可复现**的：ESP-IDF 会把构建时间写进镜像，同一份源码两次构建的哈希不同。

## 硬件

**ESP32-S3R8 · 16 MB Flash · 8 MB Octal PSRAM · 240×320 ST7789 · 六键 · I2S 扬声器输出**

| 子系统 | 说明 |
| --- | --- |
| 显示 | ST7789/ST7789P3，240×320 RGB565，SPI2 @ 40 MHz。MOSI 12、SCLK 11、CS 10、DC 9、RST 47、背光 13（低电平点亮，LEDC PWM 调光）。 |
| 按键 | 六个 GPIO 直连按键，低电平有效。UP 38、DOWN 21、A 16、B 8、LEFT 39、RIGHT 14。短按 180 ms，长按 500 ms。 |
| 音频 | 直连 I2S 功放——BCLK 41、WS 42、DOUT 40，无 codec。麦克风（ZTS6672，I2S1）板上有预留。 |
| 电量 | 可选，GPIO1 经 200K/200K 分压做 ADC 估算。不是电量计芯片。 |
| USB | 原生 USB Serial/JTAG，GPIO19/20，日志与烧录共用。 |
| 存储布局 | `nvs` 0x9000（24 KiB）、`phy_init` 0xF000、`factory` 0x10000（15 MiB）。单应用槽，无 OTA。 |

所有引脚、地址与阈值只在
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h) 里定义一次，
应用代码不得重复这些常量。

分区细节见[固件布局](docs/development/engineering/firmware-layout.md)。

## 仓库结构

```text
components/bsp/   板级支持：显示、按键、音频、电量
main/             应用本体、界面渲染，以及共用的绘图助手
tests/            无需硬件即可运行的主机测试
tools/            构建门禁、固件校验与素材生成脚本
skills/           Agent 技能：环境、开发、构建、测试、调试
docs/             工程规则、硬件说明与变更日志
```

板级支持层与应用层刻意分开：碰引脚的逻辑留在 `components/bsp`，其余都在 `main`。
守住这条界限，硬件改动就不会扩散到应用里。

## 文档

| 资源 | 内容 |
| --- | --- |
| [硬件指南](docs/hardware-design/s3r8-board.md) | 引脚图、屏幕初始化、音频通路、供电 |
| [构建与测试](docs/development/engineering/build-and-test.md) | 工具链、验证门禁、它检查什么 |
| [固件布局](docs/development/engineering/firmware-layout.md) | 分区、烧录、已存数据 |
| [AI 指南](docs/development/ai-guide.md) | 在本仓库里如何与 Agent 协作 |
| [技能](skills/README.md) | 五个必需的 Agent 技能 |
| [变更日志](docs/CHANGELOG.md) | 发布记录 |

---

[参与贡献](.github/CONTRIBUTING.md) · [获取帮助](.github/SUPPORT.md) · [行为准则](.github/CODE_OF_CONDUCT.md) · [安全](.github/SECURITY.md) · [MIT 许可证](LICENSE)

AI Agent：从 [`AGENTS.md`](AGENTS.md) 开始。
