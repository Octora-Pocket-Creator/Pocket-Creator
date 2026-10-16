**English** · [简体中文](README.zh_CN.md)

<p align="center">
  <a href="https://pocket.takecloud.cn">
    <img src="assets/images/pockigo-logo.png" alt="PockiGo" width="360">
  </a>
</p>

# PockiGo Firmware

Open firmware for a DIY handheld console: an ESP32-S3R8 with a 240×320 screen,
six physical buttons, and a speaker. Write an application, build it, flash it
over USB, and carry your own creation in your pocket.

This repository is everything needed to build one: the board support layer, a
worked application, host tests, a build gate, and the AI skills that guide
agent-assisted development.

---

## What is here

| | |
| --- | --- |
| **Open and remixable** | Firmware, board support and examples are here to be reshaped. Fork it, strip it back, build something else on top. |
| **Runs offline** | Nothing here needs a network. Applications built from this baseline run standalone. |
| **Built for agents** | `AGENTS.md` plus the `skills/` set give a coding agent the hardware facts, the build command, and the rules — so it can develop against the real board instead of guessing. |

## Start building

1. Open this repository in your AI coding tool and have it read [`AGENTS.md`](AGENTS.md).
2. Let it install the [required skills](skills/README.md) — five of them, covering setup, development, build, device testing and debugging.
3. Describe what you want to build.

A prompt that works:

```text
Build an offline habit-tracking application for PockiGo.
Use the six physical buttons and the 240×320 display, and keep records across power loss.
Start from `main`, create a `feature/*` branch, and develop there.
Follow AGENTS.md and docs/hardware-design/s3r8-board.md.
Keep hardware logic in components/bsp and application logic in main.
Deliver a runnable implementation with tests; report build results,
unexecuted device checks, and on-device acceptance steps separately.
Redesign the application's UI; do not reuse the current test menu or screens.
```

> **A successful build is not hardware validation.** Test on a real device before
> calling it done, and check the [flashing notes](docs/development/engineering/firmware-layout.md)
> — a merged image written from `0x0` overwrites stored data, including NVS.

## Build and verify

```bash
source <esp-idf>/export.sh          # ESP-IDF 5.4 or 5.5
./tools/validate.sh                 # host tests + firmware layout + merged image
./tools/validate.sh --firmware      # build only
```

The gate produces `build/PockiGo-full.bin` — the single merged image you
flash at `0x0`. It is not byte-for-byte reproducible: ESP-IDF stamps build time
into the image, so hashes differ between builds of identical source.

## Hardware

**ESP32-S3R8 · 16 MB Flash · 8 MB Octal PSRAM · 240 × 320 ST7789 · six buttons · I2S speaker output**

| Subsystem | Detail |
| --- | --- |
| Display | ST7789/ST7789P3, 240×320 RGB565, SPI2 at 40 MHz. MOSI 12, SCLK 11, CS 10, DC 9, RST 47, backlight 13 (active low, LEDC PWM). |
| Buttons | Six GPIO keys, active low. UP 38, DOWN 21, A 16, B 8, LEFT 39, RIGHT 14. Short press 180 ms, long press 500 ms. |
| Audio | Direct I2S amplifier — BCLK 41, WS 42, DOUT 40, no codec. Microphone (ZTS6672, I2S1) is reserved on the board. |
| Battery | Optional ADC estimate on GPIO1 through a 200K/200K divider. Not a fuel gauge. |
| USB | Native USB Serial/JTAG on GPIO19/20, shared by logging and flashing. |
| Storage layout | `nvs` at 0x9000 (24 KiB), `phy_init` at 0xF000, `factory` at 0x10000 (15 MiB). One application slot — no OTA. |

Every pin, address and threshold is defined once, in
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h).
Application code must not repeat those constants.

see [the S3R8 board profile](docs/hardware-design/s3r8-board.md) for the pin map
and [the firmware layout](docs/development/engineering/firmware-layout.md) for
partition details.

## Repository layout

```text
components/bsp/   Board support: display, buttons, audio, battery
main/             The application, its rendering, and the shared drawing helpers
tests/            Host tests that run without hardware
tools/            Build gate, firmware verification, and asset generators
skills/           Agent skills for setup, development, build, test and debug
docs/             Engineering rules, hardware notes, and the changelog
```

The board support layer and the application stay apart on purpose: logic that
touches pins lives in `components/bsp`, everything else in `main`. Keep that
boundary and hardware changes stay contained.

## Documentation

| Resource | What is in it |
| --- | --- |
| [Hardware guide](docs/hardware-design/s3r8-board.md) | Pin map, panel setup, audio path, power |
| [Build and test](docs/development/engineering/build-and-test.md) | Toolchain, the validation gate, what it checks |
| [Firmware layout](docs/development/engineering/firmware-layout.md) | Partitions, flashing, stored data |
| [AI guide](docs/development/ai-guide.md) | How to work with an agent in this repository |
| [Skills](skills/README.md) | The five required agent skills |
| [Changelog](docs/CHANGELOG.md) | Release history |

---

[Contributing](.github/CONTRIBUTING.md) · [Support](.github/SUPPORT.md) · [Code of conduct](.github/CODE_OF_CONDUCT.md) · [Security](.github/SECURITY.md) · [MIT License](LICENSE)

AI agents: start with [`AGENTS.md`](AGENTS.md).
