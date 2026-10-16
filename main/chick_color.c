// main/chick_color.c —— 屏幕色温补偿的纯计算实现(无 ESP-IDF/LVGL 依赖)。
#include "chick_color.h"

#include <stddef.h>

// 最暖档的增益(暖度 100)。取值克制是有意的:
//   * 抬红 +5.5%、压蓝 -12.5% 已经能明显纠正"偏冷"的观感;
//   * 再大就开始明显发黄、还会因为红色削顶而丢高光细节;
//   * 绿色保持 1.0:调绿色最容易把画面整体推成黄绿,不碰它更安全。
#define WARM_R_GAIN_AT_MAX 270
#define WARM_G_GAIN_AT_MAX 256
#define WARM_B_GAIN_AT_MAX 224
#define GAIN_UNITY        256

static uint16_t lerp_gain(uint16_t unity, uint16_t target, uint8_t warmth) {
    if (warmth > CHICK_COLOR_WARMTH_MAX) warmth = CHICK_COLOR_WARMTH_MAX;
    const int32_t delta = (int32_t)target - (int32_t)unity;
    return (uint16_t)((int32_t)unity + delta * (int32_t)warmth / CHICK_COLOR_WARMTH_MAX);
}

chick_color_gain_t chick_color_gain_from_warmth(uint8_t warmth) {
    const chick_color_gain_t gain = {
        .r = lerp_gain(GAIN_UNITY, WARM_R_GAIN_AT_MAX, warmth),
        .g = lerp_gain(GAIN_UNITY, WARM_G_GAIN_AT_MAX, warmth),
        .b = lerp_gain(GAIN_UNITY, WARM_B_GAIN_AT_MAX, warmth),
    };
    return gain;
}

// 把 5 位通道展开成 8 位:高位左移补低位,再复制高位当低位,保证满量程仍是满量程。
static uint16_t expand5(uint16_t value) { return (uint16_t)((value << 3) | (value >> 2)); }

// 把 6 位通道展开成 8 位。
static uint16_t expand6(uint16_t value) { return (uint16_t)((value << 2) | (value >> 4)); }

// 8 位通道按 Q8 增益缩放并夹取到 0..255。
static uint16_t scale8(uint16_t value, uint16_t gain) {
    const uint32_t scaled = ((uint32_t)value * gain) >> 8;
    return (uint16_t)(scaled > 255u ? 255u : scaled);
}

void chick_color_lut_build(const chick_color_gain_t *gain, chick_color_lut_t *lut) {
    if (gain == NULL || lut == NULL) return;
    for (uint16_t r = 0; r < 32; r++) {
        lut->r5[r] = (uint16_t)(scale8(expand5(r), gain->r) >> 3);
    }
    for (uint16_t g = 0; g < 64; g++) {
        lut->g6[g] = (uint16_t)(scale8(expand6(g), gain->g) >> 2);
    }
    for (uint16_t b = 0; b < 32; b++) {
        lut->b5[b] = (uint16_t)(scale8(expand5(b), gain->b) >> 3);
    }
}

uint16_t chick_color_apply_gain(uint16_t px, const chick_color_gain_t *gain) {
    if (gain == NULL) return px;
    const uint16_t r = (uint16_t)((px >> 11) & 0x1F);
    const uint16_t g = (uint16_t)((px >> 5) & 0x3F);
    const uint16_t b = (uint16_t)(px & 0x1F);
    const uint16_t r_out = (uint16_t)(scale8(expand5(r), gain->r) >> 3);
    const uint16_t g_out = (uint16_t)(scale8(expand6(g), gain->g) >> 2);
    const uint16_t b_out = (uint16_t)(scale8(expand5(b), gain->b) >> 3);
    return (uint16_t)((r_out << 11) | (g_out << 5) | b_out);
}

uint16_t chick_color_apply_lut(uint16_t px, const chick_color_lut_t *lut) {
    if (lut == NULL) return px;
    return (uint16_t)((lut->r5[(px >> 11) & 0x1F] << 11) |
                      (lut->g6[(px >> 5) & 0x3F] << 5) |
                      lut->b5[px & 0x1F]);
}
