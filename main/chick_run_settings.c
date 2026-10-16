#include "chick_run_settings.h"

#include <stdbool.h>
#include <stddef.h>

static uint8_t clamp_u8(int32_t value, uint8_t min, uint8_t max) {
    if (value < (int32_t)min) return min;
    if (value > (int32_t)max) return max;
    return (uint8_t)value;
}

void chick_run_settings_defaults(chick_run_settings_t *settings) {
    if (settings == NULL) return;
    settings->brightness = 100;
    // 与 rtttl_player 的历史默认音量一致(72):默认值不改变原有听感。
    settings->volume = 72;
    // 出厂默认给一点暖色补偿:本批屏的白点偏冷(用户实测反馈),
    // 55 是"看得出但不夸张"的起始值,用户可在设置页里现场调到满意。
    settings->warmth = 55;
}

void chick_run_settings_sanitize(chick_run_settings_t *settings) {
    if (settings == NULL) return;
    settings->brightness =
        clamp_u8(settings->brightness, CHICK_RUN_BRIGHTNESS_MIN, CHICK_RUN_BRIGHTNESS_MAX);
    settings->volume = clamp_u8(settings->volume, CHICK_RUN_VOLUME_MIN, CHICK_RUN_VOLUME_MAX);
    settings->warmth = clamp_u8(settings->warmth, CHICK_RUN_WARMTH_MIN, CHICK_RUN_WARMTH_MAX);
}

uint8_t chick_run_settings_step(uint8_t current, int32_t steps, uint8_t min, uint8_t max) {
    if (min > max) {
        const uint8_t swap = min;
        min = max;
        max = swap;
    }
    const int32_t base = clamp_u8(current, min, max);
    return clamp_u8(base + steps * (int32_t)CHICK_RUN_SETTINGS_STEP, min, max);
}
