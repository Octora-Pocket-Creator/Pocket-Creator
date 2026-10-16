**English** · [简体中文](README.zh_CN.md)

# PockiGo

**Build it. Flash it. Make it anything.** Simple and open. Anyone can create.

<a href="https://pocket.takecloud.cn/en/">Website</a> ·
<a href="#start-development-with-one-requirement">Start building</a> ·
<a href="#documentation-index">Documentation</a>
---

**PockiGo** is an open-source DIY handheld built for people who love making
things. Start with a simple idea, build your own experience, and make it
anything—from a pocket game to something no one has imagined yet.

| Open and remixable | Easy to start | Yours to create |
| --- | --- | --- |
| Open firmware and reusable examples give you room to shape your own experience. | Start from an idea and follow clear guides to make it real, even if this is your first build. | Make a companion, a tool, a game—or anything you can imagine. |

## Find your starting point

| I want to… | Start here |
| --- | --- |
| Use the device or try an official play | [Getting started](https://pocket.takecloud.cn/guides/getting-started/) · [Official plays](https://pocket.takecloud.cn/plays/) |
| Build a custom application with AI | [Agent instructions](../AGENTS.md) · [AI development guide](development/ai-guide.md) · [Required skills](../skills/README.md) |
| Prepare my environment and build firmware | [Environment setup](development/engineering/environment-setup.md) · [Build and test](development/engineering/build-and-test.md) |
| Explore the board or contribute | [Hardware guide](hardware-design/s3r8-board.md) · [Contributing](../.github/CONTRIBUTING.md) |

> [!IMPORTANT]
> `main` currently builds the Pomodoro application. The older hardware-test pages
> (`demo_*.c`, `ui_pixel.*`) stay in the tree but are not compiled. Derivative
> applications must design their own UI instead of reusing either one; BSP APIs and
> non-UI logic remain reusable.

## Start development with one requirement

1. Open the repository in your AI coding tool and have it read [`AGENTS.md`](../AGENTS.md).
2. Let it check and install the [five required skills](../skills/README.md), with any permissions your environment requires.
3. Describe what you want to build. Start from `main` on a new `feature/*` branch.

Copy this prompt and adapt it to your idea:

```text
Build an offline habit-tracking application for PockiGo.
Use the six physical buttons and the 240×320 display, and preserve records across power loss.
Start from `main`, create a `feature/*` branch, and develop the application there.
Follow AGENTS.md and docs/hardware-design/s3r8-board.md.
Look at the existing, reusable implementations first.
Keep hardware logic in components/bsp and application logic in main.
Deliver a runnable implementation with tests; report build results,
unexecuted device checks, and on-device acceptance steps separately.
Redesign the application's UI; do not use the current demo test menu or screens.
```

Before starting, check the reference archive for an existing or
reference application and previously recorded, reusable experience, and the demo
branches. See what is already built and reusable.

<details>
<summary><strong>Write a clearer requirement</strong> — pages, controls, data, and acceptance</summary>

The more specific the requirement, the more likely the assistant is to implement it correctly in one pass. Useful details include:

- User flow: what each page displays and what short press, double press, and long press do for each button.
- State and data: whether the application needs timing, persistence across power loss, networking, recording, or communication with a computer.
- Experience goals: fonts, colors, animation, sound, response time, and error states.
- Constraints: application navigation and controls, permitted dependencies, and Flash/data usage. The current application UI and the demo pages are not application UI options.
- Acceptance criteria: which behaviors require automated tests and which must be observed on real hardware.

When details are omitted, the assistant may choose conservative defaults that do not change the product direction, but it must list those assumptions in the delivery. Decisions involving new wiring, electrical safety, board revisions, or irreversible data formats require confirmation first.

</details>

> [!NOTE]
> A successful build is not hardware validation. Test the application on a real
> device after implementation; flashing requires your approval. Deliver a verified
> merged `full.bin` for flashing at `0x0`. No original-firmware backup is required,
> but a merged flash may reset stored data. See the [flashing policy](development/engineering/firmware-layout.md#flashing-and-stored-data).

## Demo branches are design cases, not a feature pile

Each `demo/*` branch evolves the baseline into an independent application. The branches demonstrate how specific problems were solved. New applications should normally branch from `main` and consult relevant examples instead of merging multiple demos wholesale.

The menu and `demo_*.c` pages on `main` are hardware-capability tests, not an application UI. Every derivative application must redesign and implement its own screens and interaction flow; using the current test menu, screens, or visual shell is prohibited. Renaming or recoloring them does not satisfy this requirement. BSP APIs, ordinary LVGL widgets, lifecycle patterns, and isolated logic may still be reused. See the [mandatory UI redesign rule](development/ai-guide.md#mandatory-ui-redesign-for-derivative-applications); maintenance of the baseline hardware-test demo itself is a separate task.

| Branch | Application | Patterns worth reusing |
| --- | --- | --- |
| `demo/stopwatch` | Stopwatch | Minimal timer application, separation of pure logic from LVGL, host-side logic tests |
| `demo/cat-themed-pomodoro-timer` | Cat-themed Pomodoro timer | Monotonic time, pause/resume, NVS persistence, a detailed PRD, and a state model |
| `demo/rock-paper-scissors` | Rock paper scissors | RGB565 image assets, asset-generation scripts, and Flash resource tradeoffs |
| `demo/claude-buddy-port` | Desktop AI hardware companion | Replacing the demo menu with a complete application, encrypted BLE, protocol parsing, state reduction, task communication, and extensive host tests |

<details>
<summary><strong>Explore a demo and create your application branch</strong></summary>

Inspect an example without switching the current working tree:

```bash
git branch -r --list 'origin/demo/*'
git diff main...origin/demo/tetris-game -- main components tests
git show origin/demo/tetris-game:main/demo_tetris.c
```

Start a new application. This repository hosts several independent projects on one baseline: after starting from `main`, create a `feature/*` branch and develop the application there — do not develop directly on `main`. Each project's final branch is `feature/*` (e.g. `feature/my-pocketport-app`), kept separate so `main` stays a clean upstream baseline and the projects do not entangle.

```bash
git switch main
git switch -c feature/my-pocketport-app
```

Example branches may change the same menu, configuration, or driver in incompatible ways. Understand the differences before extracting a state model, asset pipeline, or concurrency pattern. Code appearing in an example branch is not automatically part of the current `main` BSP contract.

</details>

## Hardware capability contract

**ESP32-S3R8 · 16 MB Flash · 8 MB Octal PSRAM · 240 × 320 display · six physical buttons**

The default layout contains only **NVS, PHY data, and one factory application**
spanning the remaining Flash. User firmware may use another valid 16 MB layout.
See [firmware layout](development/engineering/firmware-layout.md).

<details>
<summary><strong>Expand the capability table</strong> — interfaces, limits, and implementation details</summary>

The table below describes the application capabilities implemented by the current `main` branch. It is not a list of everything that might be possible according to the chip datasheet.

| Capability | Confirmed implementation | Application interface | Boundaries that must be respected |
| --- | --- | --- | --- |
| Display | ST7789/ST7789P3, 240 × 320 portrait RGB565, SPI2 at 40 MHz; LEDC backlight on GPIO13 (active low) | `bsp_display_*` | Single internal-RAM canvas pushed as dirty rectangles; the BSP exposes no LCD MISO, touch, or TE interface |
| Input | Six discrete GPIO keys, active low: UP 38, DOWN 21, A 16, B 8, LEFT 39, RIGHT 14 | `bsp_button_*` | Short press 180 ms, long press 500 ms; callbacks run in the button task and must not block |
| Audio out | Direct I2S amplifier, no codec and no MCLK: BCLK 41, WS 42, DOUT 40 | `bsp_audio_*` | Playback only. The ZTS6672 microphone footprint (I2S1: WS 17 / BCK 18 / DIN 4) is a board reserve; `bsp_audio_read()` returns `ESP_ERR_NOT_SUPPORTED` |
| Battery | ADC estimate through a 200K/200K divider on GPIO1 (ADC1_CH0) | `bsp_battery_*` | Optional at runtime; this is a voltage estimate, not a fuel-gauge reading, and needs on-device calibration |
| Wi-Fi | On-demand 2.4 GHz STA scan demo | `main/demo_wifi.c` | Scans only; it does not connect, store credentials, or validate antenna/RF performance |
| Bluetooth LE | On-demand non-connectable NimBLE advertising | `main/demo_ble.c` | Radio range, coexistence, and power draw require device measurements |
| I2C | No bus devices on the board | `bsp_i2c_*` | The BSP returns "not supported"; applications that add an I2C device define their own bus |
| Logging and flashing | Native USB Serial/JTAG on GPIO19/20 | ESP-IDF console | Shared by logging and flashing; GPIO39-42 double as native JTAG pins and are used by keys and audio |

All pins, addresses, panel parameters and thresholds are defined only in [`components/bsp/include/bsp_pins.h`](../components/bsp/include/bsp_pins.h). Application code must not duplicate these constants. See the [S3R8 board profile](hardware-design/s3r8-board.md) for the pin map, six-key input, direct I2S output, and PSRAM/Flash baseline, and [firmware layout](development/engineering/firmware-layout.md) for partitions.

Applications may also use ESP-IDF timers, FreeRTOS tasks, and internal Flash/NVS. Wi-Fi and Bluetooth LE remain ESP-IDF application services rather than BSP APIs: their menu pages initialize each stack only while open and release it on exit.

### Capabilities outside the current contract

The public firmware contract is limited to the interfaces listed above. Do not infer additional board interfaces from the ESP32-S3 feature list or from another development board: this board has six GPIO keys, direct I2S output, a microphone footprint on the board, and no I2C devices. New hardware interfaces require an explicit BSP definition and on-device acceptance criteria.

</details>

## Project structure

Board support lives in `components/bsp`; application pages, state, and tasks live
in `main`. Keep that boundary when building your own firmware.

<details>
<summary><strong>Browse the repository map</strong></summary>

```text
components/bsp/include/  Public BSP APIs and bsp_pins.h hardware facts
components/bsp/src/      Display, button, audio, battery, and shared-I2C implementations
main/                    Application entry, the current application, shared drawing and glyph helpers, and reference demos kept out of the build
tests/                   Lightweight logic tests that can run without hardware
tools/                   Shared local/CI validation and firmware verification scripts
docs/                    Project docs, changelog, engineering/contribution rules, and design references
.github/                 GitHub community files, PR template, issue forms, and CI workflows
sdkconfig.defaults       ESP32-S3, USB console, Flash and PSRAM defaults
partitions.csv           Minimal default: NVS, PHY data, and one factory application
dependencies.lock        Reproducible ESP-IDF Managed Component resolution
AGENTS.md                Mandatory AI-agent entry point (paired with AGENTS.zh_CN.md)
CLAUDE.md                Claude Code pointer to AGENTS.md (paired Chinese version)
LICENSE                  Repository license
```

</details>

## Documentation index

Engineering and contribution guides define the rules; examples and archives
provide reference material. Choose the entry that matches your task.

| Resource | What you will find |
| --- | --- |
| [Development](development/README.md) | AI workflow, engineering conventions, CI, and release guidance |
| [AI skills](../skills/README.md) | Development, environment setup, builds, device testing, and debugging |
| [Hardware](hardware-design/README.md) | Board facts, interface boundaries, acceptance checklists, and troubleshooting |
| [Chinese fonts](development/engineering/lvgl-chinese-fonts.md) | Glyph coverage, widget font selection, and blank-text troubleshooting |
| [Wi-Fi provisioning](development/engineering/wifi-provisioning.md) | Bluetooth provisioning reference and device-side integration checks |
| Community projects and experience | Playbooks and reusable knowledge in the reference archive |
| [Contributing](contribution/README.md) | Documentation, commits, and pull-request conventions |
| Brand assets | Product visual references and brand language |
| [Fork guide](fork-guide.md) · [Changelog](CHANGELOG.md) | Downstream workflows and release history |

---

[Contribute](../.github/CONTRIBUTING.md) · [Get help](../.github/SUPPORT.md) · [Code of conduct](../.github/CODE_OF_CONDUCT.md) · [Security](../.github/SECURITY.md) · [MIT License](../LICENSE)

AI agents: start with [`AGENTS.md`](../AGENTS.md) and follow its task-specific routing.
