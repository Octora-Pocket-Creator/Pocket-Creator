// tests/test_chick_color.c —— 屏幕色温补偿的主机测试。
//
// 为什么要测:补偿算错的表现是"画面颜色怪",但看不出是通道顺序错了、
// 定点位数错了,还是恒等映射不恒等。这里把三件事钉死:
//   1. 暖度 0 必须严格等于不改动(否则"关掉补偿"也是错的);
//   2. 查表与直接计算必须一致(推屏走查表,别让两条路算出两个结果);
//   3. 越界输入不能产生越界的通道值(RGB565 只有 5/6/5 位,溢出会串色)。
#include <assert.h>
#include <stdio.h>

#include "chick_color.h"

// 参考实现:按定义(A 通道 8 位展开 → 乘 Q8 增益 → 夹取 → 收回原位数)算一遍。
static uint16_t reference_apply(uint16_t px, chick_color_gain_t gain) {
    uint16_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    uint16_t r8 = (uint16_t)((r << 3) | (r >> 2));
    uint16_t g8 = (uint16_t)((g << 2) | (g >> 4));
    uint16_t b8 = (uint16_t)((b << 3) | (b >> 2));
    r8 = (uint16_t)(((uint32_t)r8 * gain.r) >> 8);
    g8 = (uint16_t)(((uint32_t)g8 * gain.g) >> 8);
    b8 = (uint16_t)(((uint32_t)b8 * gain.b) >> 8);
    if (r8 > 255) r8 = 255;
    if (g8 > 255) g8 = 255;
    if (b8 > 255) b8 = 255;
    return (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
}

static void test_unity_is_identity(void) {
    const chick_color_gain_t gain = chick_color_gain_from_warmth(0);
    assert(gain.r == 256 && gain.g == 256 && gain.b == 256);

    chick_color_lut_t lut;
    chick_color_lut_build(&gain, &lut);

    // 遍历全部 65536 个 RGB565 取值:暖度 0 时必须一个像素都不变。
    for (uint32_t px = 0; px <= 0xFFFF; px++) {
        const uint16_t p = (uint16_t)px;
        assert(chick_color_apply_gain(p, &gain) == p);
        assert(chick_color_apply_lut(p, &lut) == p);
    }
}

static void test_gain_endpoints(void) {
    const chick_color_gain_t off = chick_color_gain_from_warmth(0);
    assert(off.r == 256 && off.g == 256 && off.b == 256);

    const chick_color_gain_t max = chick_color_gain_from_warmth(100);
    assert(max.r > 256);          // 抬红
    assert(max.g == 256);         // 绿色不动
    assert(max.b < 256);          // 压蓝
    assert(max.r == 270 && max.g == 256 && max.b == 224);

    // 越界按端点夹取:0 以下用 0,100 以上用 100。
    const chick_color_gain_t over = chick_color_gain_from_warmth(200);
    assert(over.r == max.r && over.b == max.b);

    // 暖度越高,红越高、蓝越低(单调)。
    uint16_t last_r = 0, last_b = 65535;
    for (uint8_t w = 0; w <= 100; w += 10) {
        const chick_color_gain_t g = chick_color_gain_from_warmth(w);
        assert(g.r >= last_r);
        assert(g.b <= last_b);
        last_r = g.r;
        last_b = g.b;
    }
}

static void test_lut_matches_reference(void) {
    // 抽取几个暖度,遍历若干代表色,查表结果必须与直接计算/参考实现一致。
    const uint8_t warmths[] = { 0, 25, 50, 75, 100 };
    const uint16_t samples[] = {
        0x0000, 0xFFFF, 0x001F, 0x07E0, 0xF800,   // 黑/白/三原色
        0x8410, 0xC618, 0x4208,                   // 中灰/浅灰/深灰
        0x07FF, 0xFFE0, 0xF81F,                   // 青/黄/品红
        0x5AEB, 0x2C7B, 0x9CD3,                   // 天空/深蓝/草绿
    };
    for (size_t i = 0; i < sizeof(warmths) / sizeof(warmths[0]); i++) {
        const chick_color_gain_t gain = chick_color_gain_from_warmth(warmths[i]);
        chick_color_lut_t lut;
        chick_color_lut_build(&gain, &lut);
        for (size_t k = 0; k < sizeof(samples) / sizeof(samples[0]); k++) {
            const uint16_t px = samples[k];
            assert(chick_color_apply_gain(px, &gain) == reference_apply(px, gain));
            assert(chick_color_apply_lut(px, &lut) == reference_apply(px, gain));
        }
    }
}

static void test_warm_white_becomes_warm(void) {
    const chick_color_gain_t gain = chick_color_gain_from_warmth(100);
    const uint16_t white = 0xFFFF;
    const uint16_t out = chick_color_apply_gain(white, &gain);
    const uint16_t r = (out >> 11) & 0x1F;
    const uint16_t g = (out >> 5) & 0x3F;
    const uint16_t b = out & 0x1F;

    // 白点必须保持"白"但蓝通道明显低于红绿 —— 这就是"变暖"的定义。
    assert(r >= 30 && g >= 60);
    assert(b < g);
    assert((int)g - (int)b >= 4);        // 至少 4 档(≈8%)的蓝差,肉眼能看出来

    // 黑色永远是黑色:乘性补偿不能把背景提成灰色。
    assert(chick_color_apply_gain(0x0000, &gain) == 0x0000);
}

static void test_channels_do_not_overflow(void) {
    // 逐通道遍历所有取值:补偿后仍必须在 5/6/5 位范围内,不能串到相邻通道。
    const uint8_t warmths[] = { 0, 50, 100, 255 };
    for (size_t i = 0; i < sizeof(warmths) / sizeof(warmths[0]); i++) {
        const chick_color_gain_t gain = chick_color_gain_from_warmth(warmths[i]);
        chick_color_lut_t lut;
        chick_color_lut_build(&gain, &lut);
        for (uint16_t r = 0; r < 32; r++) assert(lut.r5[r] < 32);
        for (uint16_t g = 0; g < 64; g++) assert(lut.g6[g] < 64);
        for (uint16_t b = 0; b < 32; b++) assert(lut.b5[b] < 32);

        // 只有红通道变化的像素,补偿后蓝绿通道必须原样(不串色)。
        for (uint16_t r = 0; r < 32; r++) {
            const uint16_t px = (uint16_t)(r << 11);
            const uint16_t out = chick_color_apply_gain(px, &gain);
            assert(((out >> 5) & 0x3F) == 0);
            assert((out & 0x1F) == 0);
        }
    }
}

static void test_null_is_safe(void) {
    // 传入空指针时按"不处理"返回,而不是崩掉(推屏路径不希望因为配置没准备好就死)。
    assert(chick_color_apply_gain(0x1234, NULL) == 0x1234);
    assert(chick_color_apply_lut(0x1234, NULL) == 0x1234);
    chick_color_lut_build(NULL, NULL);   // 不应崩
}

int main(void) {
    test_unity_is_identity();
    test_gain_endpoints();
    test_lut_matches_reference();
    test_warm_white_becomes_warm();
    test_channels_do_not_overflow();
    test_null_is_safe();
    printf("chick color tests: PASS\n");
    return 0;
}
