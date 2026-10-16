// main/chick_run_settings.h —— 设置项(背光/音量/色温)的数值规则,不含 LVGL/ESP-IDF。
//
// 单独成一层是为了能在主机上直接测:夹取、步进、脏数据自愈这些判断最容易写错,
// 而它们在设备上只表现为"按一下没反应"或者"屏幕全黑",排查成本很高。
#pragma once

#include <stdint.h>

#include "chick_color.h"

// 背光下限 10%:再低在强光下等于黑屏,用户会以为设备坏了。
#define CHICK_RUN_BRIGHTNESS_MIN   10
#define CHICK_RUN_BRIGHTNESS_MAX   100
#define CHICK_RUN_VOLUME_MIN       0     // 0 = 静音(设置界面上是可选项)
#define CHICK_RUN_VOLUME_MAX       100
// 色温(暖度):0 = 原色,100 = 最暖。范围来自 chick_color.h,这里只做持久化与夹取。
#define CHICK_RUN_WARMTH_MIN       CHICK_COLOR_WARMTH_MIN
#define CHICK_RUN_WARMTH_MAX       CHICK_COLOR_WARMTH_MAX
#define CHICK_RUN_SETTINGS_STEP    10    // 一次点击调整的步长(百分比)
#define CHICK_RUN_SETTINGS_FAST_STEPS 3  // 长按快调:一次 3 档

typedef struct {
    uint8_t brightness;   // 背光百分比
    uint8_t volume;       // 音量百分比
    uint8_t warmth;       // 屏幕色温补偿强度 0..100(见 chick_color.h)
} chick_run_settings_t;

// 首次开机 / NVS 无记录时的取值。
void chick_run_settings_defaults(chick_run_settings_t *settings);

// 把两项都收敛到合法范围。NVS 里可能存着旧版本或损坏的值,
// 读出来先过一遍这里,避免 0 背光这种"看起来是死机"的状态。
void chick_run_settings_sanitize(chick_run_settings_t *settings);

// 按 steps 个步长调整 current(例如 +1 表示加 10%),结果夹取到 [min, max]。
uint8_t chick_run_settings_step(uint8_t current, int32_t steps, uint8_t min, uint8_t max);
