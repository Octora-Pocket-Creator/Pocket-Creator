<p align="right">
  <a href="s3r8-board.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# ESP32-S3R8 Board Profile

This document is the engineering reference for the ESP32-S3R8 handheld built by
this repository. It records confirmed board facts, the firmware decisions that
follow from them, and what still needs physical verification.

- Firmware source of truth: [`components/bsp/include/bsp_pins.h`](../../components/bsp/include/bsp_pins.h).
- The pin facts below come from the board vendor pin map; visual, audio, and
  power results are recorded separately in the validation table at the end.
- Last updated: 2026-10-09.

## 1. Board baseline

```text
ESP32-S3-WROOM-1 / WROOM-1U (ESP32-S3R8: 16 MB Flash + 8 MB Octal PSRAM)
├─ ST7789/ST7789P3 LCD (240x320, SPI2, write-only)
│  ├─ GPIO11 -> SCLK
│  ├─ GPIO12 -> MOSI
│  ├─ GPIO10 -> CS
│  ├─ GPIO9  -> DC
│  ├─ GPIO47 -> RST
│  └─ GPIO13 -> BCKL, active low
├─ Six keys (active low, internal pull-up) — no separate OK key
│  ├─ GPIO38 -> UP     GPIO21 -> DOWN
│  ├─ GPIO39 -> LEFT   GPIO14 -> RIGHT
│  ├─ GPIO16 -> A      GPIO8  -> B
├─ Optional I2S0 amplifier output (no codec, no MCLK)
│  ├─ GPIO42 -> WS/LRCK
│  ├─ GPIO41 -> BCK
│  └─ GPIO40 -> DATA/DOUT
├─ Battery sense: VBAT -- 200K -- GPIO1(ADC1_CH0) -- 200K -- GND
│  └─ 1:1 divider, so the pin carries half the battery voltage
├─ Optional I2S1 ZTS6672 microphone (board reserve)
│  ├─ GPIO17 -> WS/LRC
│  ├─ GPIO18 -> BCK
│  └─ GPIO4  -> DATA/DIN
└─ No I2C devices on the board (no audio codec, no fuel-gauge chip)
```

| Subsystem | Device or mode | Resource | Firmware support |
| --- | --- | --- | --- |
| MCU | ESP32-S3R8 | 16 MB Flash, 8 MB Octal PSRAM | Configured (`sdkconfig.defaults`) |
| Display | ST7789/ST7789P3, 240 × 320, RGB565 | SPI2, 40 MHz, mode 0 | Driver in `bsp_display.c` |
| Backlight | LCD LED, active low | GPIO13, LEDC 5 kHz/10 bit | PWM brightness; the BSP inverts the percentage |
| Buttons | Six discrete GPIO keys (no OK key) | GPIO 38/21/39/14/16/8 | `bsp_button_gpio.c` |
| Audio out | Amplifier over I2S (no codec) | I2S0 TX: BCLK 41 / WS 42 / DOUT 40 | `bsp_audio_i2s_out.c` |
| Audio in | ZTS6672 microphone footprints (board reserve) | I2S1 RX: WS 17 / BCK 18 / DIN 4 | No driver; `bsp_audio_read()` returns `ESP_ERR_NOT_SUPPORTED` |
| Battery | No fuel-gauge chip; 1:1 divider into an ADC pin | VBAT — 200K — GPIO1 (ADC1_CH0) — 200K — GND | `bsp_battery_adc.c` + `bsp_battery_math.c` |
| I2C | No bus devices | — | `bsp_i2c_*` stub in `bsp_none.c` |
| Console | USB Serial/JTAG | Native USB, GPIO19/20 | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` |

## 2. Pin map and resource ownership

| GPIO | Function | Direction/peripheral | Notes |
| ---: | --- | --- | --- |
| 1 | battery sense | ADC1_CH0 input | 1:1 divider (200K/200K) from VBAT; carries half the battery voltage |
| 4 | I2S1 DIN | input (reserved) | ZTS6672 microphone DATA; board reserve |
| 8 | key B | input, pull-up | active low; back/pause |
| 9 | LCD DC | SPI output | command/data select |
| 10 | LCD CS | SPI output | ST7789 chip select |
| 11 | LCD SCLK | SPI output | SPI2, 40 MHz, mode 0 |
| 12 | LCD MOSI | SPI output | no MISO; the panel cannot be read |
| 13 | backlight PWM | LEDC output | **active low**; `BSP_LCD_BL_ACTIVE_LOW` |
| 14 | key RIGHT | input, pull-up | active low |
| 16 | key A | input, pull-up | active low; confirm/jump |
| 17 | I2S1 WS/LRC | output (reserved) | ZTS6672 microphone word select; board reserve |
| 18 | I2S1 BCK | output (reserved) | ZTS6672 microphone bit clock; board reserve |
| 19/20 | USB Serial/JTAG | USB | reserved for console and flashing |
| 21 | key DOWN | input, pull-up | active low |
| 38 | key UP | input, pull-up | active low |
| 39 | key LEFT | input, pull-up | native JTAG pin reused as a key |
| 40 | I2S DOUT | I2S output | MCU → amplifier |
| 41 | I2S BCLK | I2S output | native JTAG pin reused for audio |
| 42 | I2S WS/LRCK | I2S output | native JTAG pin reused for audio |
| 47 | LCD RST | output | hardware reset |

Reserved or risky pins:

- GPIO35/36/37 connect the Octal PSRAM inside the module; they are not general-purpose I/O.
- GPIO39/40/41/42 are the native JTAG pins. This board uses all four, so external
  JTAG debugging is unavailable; use USB Serial/JTAG instead.
- GPIO0/3/45/46 are strapping pins and are not assigned to peripherals here.
- GPIO19/20 carry the native USB D−/D+.
- GPIO4/17/18 (microphone) and GPIO40/41/42 (amplifier) are audio reserves. The
  BSP initialises only I2S0 TX; the microphone pins stay unowned until a
  derivative application enables them and updates the ownership table above.
- GPIO39/40/41/42 being JTAG pins means the amplifier reserve and JTAG share
  silicon: enabling external JTAG debugging requires re-planning those signals.

### 2.1 Peripheral ownership and coexistence

| Resource | Owner | Sharing rule or conflict |
| --- | --- | --- |
| SPI2 | display BSP | Dedicated to the ST7789; no MISO configured. |
| LEDC low-speed timer 0 / channel 0 | backlight BSP | Pick another timer/channel for new PWM users. |
| I2S0 TX | audio BSP | Transmit-only; there is no RX channel to share. |
| I2S1 RX | unowned | Reserved for the optional ZTS6672 microphone. No driver claims it; a derivative app that enables the microphone takes ownership and must update this table. |
| GPIO 38/21/39/14/16/8 | button BSP | Six keys are configured as pulled-up inputs with no interrupt; the button component polls them. |
| External Octal PSRAM | SPI0/SPI1 flash+PSRAM | Enabling PSRAM in `sdkconfig` is mandatory; the firmware deliberately does not set `SPIRAM_IGNORE_NOTFOUND`. |
| USB Serial/JTAG | console configuration | Shared by logging, flashing, and the monitor. |

## 3. Display

The panel is driven with the `esp_lcd` ST7789 vendor driver in SPI mode 0,
RGB565, plus the panel-vendor initialization sequence and `0x21` inversion
maintained in `bsp_display.c`:

- The reset line is wired (GPIO47), so `esp_lcd_panel_reset()` performs a hardware reset.
- The clock is 40 MHz (board-vendor validated value). Raising it improves frame
  rate; enable it only after checking for artifacts and color errors on the device.
- The backlight is active low. `bsp_display_backlight(percent)` inverts the duty
  cycle, and the "off" level used for initialization and deep sleep is 1, not 0.
- The application renders without LVGL: it draws into an internal-RAM canvas and
  pushes only dirty rectangles over SPI.
- The panel corners are square: `BSP_LCD_CORNER_RADIUS` is 0, so the application
  applies no corner mask and the whole 240 × 320 area shows content.

If a replacement panel shows negative (inverted) colors, set
`BSP_LCD_INVERT_COLOR` to 0 in `bsp_pins.h`; if brightness behaves backwards, the
panel is not active-low and `BSP_LCD_BL_ACTIVE_LOW` must be 0.

**Gradients and color banding.** RGB565 gives each channel only 5/6 bits, so the
long sky and grass gradients must be dithered; the number of dithered pixels is
fixed by the quantization error, and only their *distribution* can be chosen. The
application computes each row as a base level plus a duty cycle equal to that row's
quantization remainder (`main/chick_gradient.c`, dither mask in
`main/chick_dither.c`), so every row's average equals the ideal gradient exactly —
there is no row where the average jumps by a level. Which pixels round up is decided
by a generated 32x32 blue-noise mask (void-and-cluster, see
`tools/generate-dither-mask.py`): the threshold is spread over space with low
low-frequency energy, so the residual error reads as fine grain rather than as a
grid.

Two earlier revisions were rejected on hardware and must not be reintroduced:

1. dithering *every* row with a 4x4 ordered matrix produced blocky grain in the sky
   plus horizontal stripes every four rows in the grass;
2. confining the dither to a short band around each quantization step made the
   1-LSB differences between the remaining flat regions read as large horizontal
   bands — worse than the grain it was meant to remove.

Host tests pin the properties that keep the gradient smooth: endpoint rows match
the nearest representable color, row averages track the ideal ramp within 1/256 of
a level, per-channel averages are monotonic, adjacent rows differ by at most one
level, the mask is a permutation of 0..1023 with no small-period repetition, and no
channel ever overflows into its neighbour.

**White point / color temperature.** This batch of panels renders slightly cool
(blue-tinted white), which is a property of the panel and its backlight LEDs, not
of the firmware. Because the application owns its rendering, it compensates with a
per-channel gain applied as the single choke point of the flush path
(`main/chick_color.c`): RGB565 is expanded per channel, multiplied by a Q8 gain
(red up, blue down, green untouched), and clamped back to 5/6/5 bits. Multiplicative
gains keep black black, so the background does not turn grey. The strength is the
`warmth` setting (0 = untouched, 100 = warmest preset with R×1.055/B×0.875) and it
is adjustable on the device in the settings screen, so a unit can be matched to its
panel without a rebuild. Two properties are pinned by host tests: warmth 0 is a
strict identity over all 65536 RGB565 values, and the lookup table always agrees
with the direct per-pixel computation.

## 4. Buttons

Six discrete keys, each pulled up internally and read as active low. **This board
has no `OK` key:** the physical labels are UP, DOWN, LEFT, RIGHT, A and B. The BSP
maps them onto semantic keys so applications never depend on the silkscreen:

| Semantic key | Physical key | GPIO | Chick Run role |
| --- | --- | --- | --- |
| `BSP_BTN_UP` | UP | 38 | jump; menu up |
| `BSP_BTN_DOWN` | DOWN | 21 | slide; menu down |
| `BSP_BTN_A` | A | 16 | confirm/start; jump during a run |
| `BSP_BTN_B` | B | 8 | back; pause/resume during a run |
| `BSP_BTN_LEFT` | LEFT | 39 | title-screen mode switch; settings: decrease value |
| `BSP_BTN_RIGHT` | RIGHT | 14 | title-screen mode switch; settings: increase value |

Screen controls in the current application:

| Screen | Controls |
| --- | --- |
| Title | UP/DOWN/LEFT/RIGHT switch the play mode; A starts a run; A long-press or B opens settings |
| Running | UP or A jumps; DOWN slides; B pauses |
| Paused | A resumes; B returns to the title |
| Game over | A plays again; B returns to the title |
| Settings | UP/DOWN switch between rows (brightness, volume); LEFT/RIGHT adjust the value (long-press adjusts fast); A or B returns |

`BSP_BTN_HAS_B` (`BSP_BTN_COUNT >= 6`) keeps application wording and behavior tied
to the key count rather than a board name.

Press timing is fixed by the BSP: 180 ms short-press window and 500 ms long-press
threshold (`BSP_BTN_SHORT_PRESS_MS`, `BSP_BTN_LONG_PRESS_MS`), independent of the
button component's Kconfig defaults. `bsp_button_read_mv()` returns `-1` on this
board because there is no resistor-ladder node to measure.

## 5. Audio output

There is no audio codec. The MCU drives the amplifier directly over I2S0 in
Philips standard mode with 16-bit samples and no MCLK:

| Property | Value | Reason |
| --- | --- | --- |
| Port | I2S0, master TX only | No capture path exists |
| Pins | BCLK 41, WS 42, DOUT 40 | Board wiring |
| MCLK | Not used | The amplifier does not need it; leaving it unused avoids extra noise |
| Sample format | 16-bit, output as stereo | The source is mono; both slots carry the signal so either channel-selecting amplifier works |
| Volume | Digital gain in the BSP | No hardware volume register exists |
| Amplifier enable | Not connected (`BSP_I2S_PA_CTRL = -1`) | Set the GPIO if the final board adds one |

Audio degrades without failing the boot: if the amplifier is unpopulated the I2S
peripheral still initializes and the game simply runs silently.

## 6. Battery measurement

The board has no fuel-gauge chip. The battery voltage is divided 1:1 by two 200 kΩ
resistors and sampled on GPIO1 (ADC1_CH0), so the pin carries half the battery
voltage. Full charge is 4.1 V and the usable empty point is 3.3 V.

One reading runs this pipeline (all of it is host-tested):

| Step | Implementation | Why |
| --- | --- | --- |
| 1. Sample | 16 ADC samples, 12 dB attenuation, curve-fitting calibration | 1:1 divider keeps 2.05 V at full charge, well inside the ~3.1 V range |
| 2. Plausibility gate | Drop samples outside a sane battery window (`BSP_BATT_VBAT_MIN_MV` … `MAX_MV`, 2.5-4.4 V) | Coupled noise can read as an impossible 5 V+ battery; filtering can only weaken it, not remove it |
| 3. Trimmed mean | Sort, drop the lowest/highest quarter, average the middle half | Removes isolated spikes (e.g. backlight PWM edges) |
| 4. IIR smoothing | `value += (sample - value) / 8` across reads (`BSP_BATT_IIR_SHIFT`) | Rejects slow noise and load steps; keeps state between reads |
| 5. Divider restore | `Vbat = Vpin × (Rtop + Rbottom) / Rbottom × scale` | 200K/200K → ×2; `BSP_BATT_MV_SCALE_PPM` is the calibration factor |
| 6. Voltage → SOC | Piecewise linear table, 4.10 V = 100 %, 3.30 V = 0 % | The 3.9-3.7 V plateau needs a denser table than a straight line |
| 7. SOC hysteresis | Report only changes of ≥ 2 % (`BSP_BATT_SOC_HYSTERESIS`) | Stops the battery icon from flickering between neighbouring values |

Known accuracy limits, and what to do about them:

- **Source impedance.** The divider's Thévenin impedance is 100 kΩ, which is high for
the S3 SAR ADC: the sample capacitor does not fully charge, so readings are biased
low and jitter. Mitigations: the averaging above, a calibration factor, and — best —
a 100 nF capacitor from GPIO1 to GND (filters and supplies charge to the sample cap).
- **Calibration.** `bsp_pins.h` has `BSP_BATT_MV_SCALE_PPM` (default 1000000 = no
correction). Measure the battery with a multimeter, compare it with the battery
percentage/voltage line printed every 60 s, and set the factor to `actual / reported × 1000000`. Do this
on battery power, not while charging.
- **Charging.** While USB is connected, the pin usually reads the charger voltage,
so the percentage is optimistic. Treat the number as valid when running on battery.
- **No load compensation.** A heavy load sags the terminal voltage, so a nearly empty
battery can read a few percentage points high under load.
- The voltage window and the curve live in `bsp_pins.h` and `bsp_battery_math.c`; a
different battery chemistry or low-voltage cutoff means editing those two places.

## 7. Flash, PSRAM, and partitions

- Flash: 16 MB, `CONFIG_ESPTOOLPY_FLASHSIZE_16MB`, fixed image header (no size update).
- PSRAM: 8 MB Octal (`CONFIG_SPIRAM_MODE_OCT`, 80 MHz). The board must be an R8
  variant; a mismatch aborts the boot instead of silently continuing without PSRAM.
- Partition table: `partitions.csv` — `nvs` + `phy_init` + one `factory` app
  spanning the remaining 15 MB. No OTA slots are reserved.
- Console: USB Serial/JTAG on GPIO19/20, not UART0.

## 8. Building, flashing, and validating

The repository targets ESP32-S3; `sdkconfig.defaults` and `partitions.csv` configure it.

```bash
# S3R8 (default): 16 MB Flash + 8 MB Octal PSRAM
./tools/validate.sh                  # static checks + host tests + firmware build
idf.py -p /dev/cu.usbmodem* flash    # segmented flash: keeps the NVS contents

# ESP32-S3R8
```

Flashing the merged `build/PockiGo-full.bin` from `0x0` pads the gaps
between images and therefore rewrites the NVS region; use segmented `idf.py flash`
when saved settings must survive. See
[firmware layout](../development/engineering/firmware-layout.md#flashing-and-stored-data).

## 9. Validation status

Build identity of the recorded run: application build tag `0.8.0-s3`, ESP-IDF
v5.4.1 (the repository manifest currently allows `>=5.4,<5.6.0`), flashed with
segmented `idf.py flash` so NVS was preserved.

| Check | Method | Status |
| --- | --- | --- |
| Repository gate (static checks, host tests, firmware build + merged-image verification) | `./tools/validate.sh` | PASS (ESP32-S3R8 profile) |
| Chip identity, Flash, PSRAM | `esptool flash_id`; boot log | PASS — ESP32-S3 (QFN56) revision v0.2, 16 MB Flash, 8192 KB PSRAM added to the heap |
| Flash and boot of the application | `idf.py flash`; boot log | PASS — bootloader `0x0`, partition table `0x8000`, app `0x10000`; NVS not rewritten |
| Display initialization and first frame | Boot log; rendered output | Initialization PASS (240 × 320, backlight LEDC on GPIO13 active low, first frame pushed at `t≈854 ms`); **colors/orientation not confirmed by a human** |
| Frame rate | Runtime frame statistics | 235 frames in 5 s ≈ 47 FPS; dirty-rect push 1.3-1.7 kpx/frame, 0.7-0.9 ms/frame |
| Font coverage | Runtime glyph check; boot log reports the coverage check as passing | PASS with the regenerated subset (74 CJK + printable ASCII) |
| Color-temperature compensation | Host tests (`tests/test_chick_color.c`) plus visual check | Host tests PASS (identity at 0, LUT/direct agreement, no channel overflow); **perceived warmth not confirmed by a human** |
| Gradient banding/dither | Host tests (`tests/test_chick_gradient.c`, `tests/test_chick_dither.c`) plus visual check | Host tests PASS (row averages track the ideal ramp, monotonic averages, <=1 level between rows, blue-noise mask without small periods); **smoothness not yet confirmed on hardware** |
| Six keys | Boot log plus key events in the serial log | Six keys initialized on GPIO 38/21/16/8/39/14 and events are reported; **physical key-to-action mapping not confirmed by a human** |
| Battery ADC and SOC chain | Host tests plus the periodic battery percentage/voltage log line | Host tests PASS; **no device reading recorded yet** (the board was unplugged); calibration factor still at 1.000000 |
| Square (non-masked) screen corners | Rendered output | **Not confirmed by a human** — the corner mask is disabled in firmware for this board |
| Backlight polarity and brightness | Rendered output | **Not confirmed by a human** |
| Audio output through the amplifier | Listening on the device | **Not verified** — amplifier population unknown |
| Battery icon shows a plausible percentage | Rendered output on battery power | **Not verified** (the board was unplugged before the battery log could be captured) |
| Deep sleep and backlight hold levels | On-device test | **Not run** on this board |

Update this table with the physical result after each board revision; a successful
build, flash, or boot log is not visual or audible acceptance.

## 10. Troubleshooting

| Symptom | First checks |
| --- | --- |
| Black screen | GPIO13 backlight level (active low), GPIO47 reset, SPI wiring on 11/12/10/9, 3.3 V supply |
| Brightness inverted (0 = brightest) | `BSP_LCD_BL_ACTIVE_LOW` no longer matches the board |
| Washed-out or negative colors | SPI mode 0, 40 MHz, RGB565 byte order, or `BSP_LCD_INVERT_COLOR` |
| Gate shows "detected flash size 16MB" but boots with 8 MB layout | Stale `sdkconfig` from another target; delete `sdkconfig` and rebuild |
| Boot aborts inside `esp_psram` | Module is not an R8/Octal PSRAM part while `CONFIG_SPIRAM_MODE_OCT` is set |
| A key triggers on its own, or two keys fire together | Pull-up missing on the board, shorted key, or the key is wired to another GPIO; re-read the runtime GPIO log |
| Black corner wedges are still visible | `BSP_LCD_CORNER_RADIUS` is not 0 in `bsp_pins.h` (or a stale build) |
| No sound but logs show "audio ready" | Amplifier supply/enable, DATA/BCLK/WS wiring, or the amplifier is not populated |
| Battery always reads 0 % / 100 % / an impossible voltage | GPIO1 divider values, `BSP_BATT_DIVIDER_R_*`, or a floating pin; the plausibility window rejects impossible values, so a permanently rejected pin shows `-1` |
| Battery percentage is off by a constant factor | Measure with a multimeter and set `BSP_BATT_MV_SCALE_PPM`; check on battery power, not while charging |
| Battery percentage jumps around | Lower `BSP_BATT_IIR_SHIFT` effects by increasing it, or raise `BSP_BATT_SOC_HYSTERESIS`; also add the 100 nF capacitor at GPIO1 |
| No serial output at all | The board may use a USB-UART bridge instead of native USB; switch the console to UART0 (GPIO43/44) |
