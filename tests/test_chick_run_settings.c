// tests/test_chick_run_settings.c —— 背光/音量/色温数值规则的主机测试。
//
// 这些规则在设备上只表现为"按了没反应"或"屏幕全黑",所以夹取边界、
// 脏数据自愈、步进方向都必须在这里钉死。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "chick_run_settings.h"

static void test_defaults(void) {
    chick_run_settings_t s = { .brightness = 0, .volume = 0, .warmth = 0 };
    chick_run_settings_defaults(&s);
    assert(s.brightness == CHICK_RUN_BRIGHTNESS_MAX);
    assert(s.volume > 0);
    // 出厂默认带一点暖色补偿(本批屏白点偏冷),但不能顶到最暖。
    assert(s.warmth > CHICK_RUN_WARMTH_MIN);
    assert(s.warmth < CHICK_RUN_WARMTH_MAX);
    chick_run_settings_defaults(NULL);   // 空指针必须安全
}

static void test_sanitize(void) {
    chick_run_settings_t s = { .brightness = 0, .volume = 255, .warmth = 255 };
    chick_run_settings_sanitize(&s);
    assert(s.brightness == CHICK_RUN_BRIGHTNESS_MIN);   // 0 背光等于黑屏,必须抬到下限
    assert(s.volume == CHICK_RUN_VOLUME_MAX);
    assert(s.warmth == CHICK_RUN_WARMTH_MAX);           // 越界的暖度夹到上限
    chick_run_settings_sanitize(NULL);
}

static void test_warmth_range(void) {
    // 暖度是 0..100:0 必须是合法值(等于关掉补偿),不能像背光那样有下限。
    assert(CHICK_RUN_WARMTH_MIN == 0);
    assert(CHICK_RUN_WARMTH_MAX == 100);
    assert(chick_run_settings_step(0, -1, CHICK_RUN_WARMTH_MIN, CHICK_RUN_WARMTH_MAX) == 0);
    assert(chick_run_settings_step(0, +1, CHICK_RUN_WARMTH_MIN, CHICK_RUN_WARMTH_MAX) ==
           CHICK_RUN_SETTINGS_STEP);
    assert(chick_run_settings_step(100, +1, CHICK_RUN_WARMTH_MIN, CHICK_RUN_WARMTH_MAX) == 100);
}

static void test_step(void) {
    const uint8_t lo = CHICK_RUN_BRIGHTNESS_MIN;
    const uint8_t hi = CHICK_RUN_BRIGHTNESS_MAX;

    assert(chick_run_settings_step(50, +1, lo, hi) == 60);
    assert(chick_run_settings_step(50, -1, lo, hi) == 40);
    assert(chick_run_settings_step(hi, +1, lo, hi) == hi);       // 上限不回绕
    assert(chick_run_settings_step(lo, -1, lo, hi) == lo);       // 下限不回绕
    assert(chick_run_settings_step(lo, -100, lo, hi) == lo);

    // 音量下限是 0(静音),不能借用背光的下限。
    assert(chick_run_settings_step(10, -1, CHICK_RUN_VOLUME_MIN, CHICK_RUN_VOLUME_MAX) == 0);
    assert(chick_run_settings_step(0, -1, CHICK_RUN_VOLUME_MIN, CHICK_RUN_VOLUME_MAX) == 0);

    // 脏起点(超出范围)先收敛再步进。
    assert(chick_run_settings_step(200, +1, lo, hi) == hi);
    assert(chick_run_settings_step(1, -1, lo, hi) == lo);

    // 步长是 CHICK_RUN_SETTINGS_STEP(10%),一次点击增减一档。
    assert(CHICK_RUN_SETTINGS_STEP == 10);
    assert(chick_run_settings_step(55, 1, lo, hi) == 65);
}

int main(void) {
    test_defaults();
    test_sanitize();
    test_warmth_range();
    test_step();
    printf("chick run settings tests: PASS\n");
    return 0;
}
