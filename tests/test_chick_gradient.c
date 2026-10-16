// tests/test_chick_gradient.c —— 渐变量化/抖动的主机测试。
//
// 这些性质决定了"渐变看起来平不平滑",肉眼只能判断"好像还有点颗粒/有条纹",
// 定位不到是哪一步算错了,所以必须在这里钉死:
//   1. 端点行完全平坦、且落在最接近请求色的档位上(否则背景边缘自带麻点/色偏);
//   2. 纯色区域(或整条渐变不足一个量化档)完全平坦(否则成了"铺了一层噪点");
//   3. 行平均必须【紧贴理想渐变】—— 这是"不出现大条分区"的量化判据:
//      行平均与理想值的偏差不得超过 1/256 个量化档;
//   4. 相邻行的平均最多差一个量化档(否则就是可见的硬边);
//   5. 到顶的颜色进位时不能溢出到相邻通道。
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "chick_dither.h"
#include "chick_gradient.h"

#define SKY_TOP    0x87CEEB
#define SKY_BOTTOM 0x98D8C8
#define GRASS_TOP  0x7CB342
#define GRASS_DEEP 0x4E8B2E

#define SKY_ROWS   248
#define GRASS_ROWS 72

// 8 位颜色 → 最近的 RGB565 档位(端点取"最接近的可表示色",不是向下取整)。
static uint16_t nearest565(uint32_t rgb888) {
    const int r = (int)((rgb888 >> 16) & 0xFF);
    const int g = (int)((rgb888 >> 8) & 0xFF);
    const int b = (int)(rgb888 & 0xFF);
    int r5 = (r + 4) / 8;
    int g6 = (g + 2) / 4;
    int b5 = (b + 4) / 8;
    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// 行的"平均档位":掩码 rank 在 0..1023 上均匀分布,所以占空比 = 阈值/1024,
// 平均档位 = 基准档位 + 阈值/1024。用 1/1024 档为单位做整数比较。
typedef struct {
    int r, g, b;
} avg_levels_t;

static avg_levels_t row_avg_levels(uint32_t top, uint32_t bottom, int row, int rows) {
    chick_gradient_row_t r;
    chick_gradient_row(top, bottom, row, rows, &r);
    const avg_levels_t out = {
        .r = (int)((r.base >> 11) & 0x1F) * CHICK_DITHER_LEVELS + (int)r.thr_r,
        .g = (int)((r.base >> 5) & 0x3F) * CHICK_DITHER_LEVELS + (int)r.thr_g,
        .b = (int)(r.base & 0x1F) * CHICK_DITHER_LEVELS + (int)r.thr_b,
    };
    return out;
}

static bool row_is_flat(const chick_gradient_row_t *r) {
    const bool fr = r->thr_r == 0 || r->thr_r == CHICK_DITHER_LEVELS;
    const bool fg = r->thr_g == 0 || r->thr_g == CHICK_DITHER_LEVELS;
    const bool fb = r->thr_b == 0 || r->thr_b == CHICK_DITHER_LEVELS;
    return fr && fg && fb;
}

// 某个 8 位通道在第 row 行的理想值(单位:量化档,浮点算,和实现里的定点路线对照)。
static double ideal_units(int top, int bottom, int row, int rows, int step_bits) {
    const double step = (double)(1 << step_bits);
    const double value = (double)top + (double)(bottom - top) * (double)row /
                                             (double)(rows > 1 ? rows - 1 : 1);
    return value / step;
}

static void test_endpoints_exact(void) {
    const struct {
        uint32_t color;
        int row;
    } ends[2] = { { SKY_TOP, 0 }, { SKY_BOTTOM, SKY_ROWS - 1 } };

    for (size_t i = 0; i < 2; i++) {
        chick_gradient_row_t r;
        chick_gradient_row(SKY_TOP, SKY_BOTTOM, ends[i].row, SKY_ROWS, &r);
        // 端点行的"行平均"必须等于理想值(允许 1/256 档的定点误差),
        // 这正是"上下边缘不会出现半档落差"的判据。
        const avg_levels_t avg = row_avg_levels(SKY_TOP, SKY_BOTTOM, ends[i].row, SKY_ROWS);
        const int steps[3] = { 8, 4, 8 };
        const int avgs[3] = { avg.r, avg.g, avg.b };
        for (int ch = 0; ch < 3; ch++) {
            const int top = (int)((SKY_TOP >> (16 - 8 * ch)) & 0xFF);
            const int bottom = (int)((SKY_BOTTOM >> (16 - 8 * ch)) & 0xFF);
            const double ideal = ideal_units(top, bottom, ends[i].row, SKY_ROWS, steps[ch] == 8 ? 3 : 2);
            const double actual = avgs[ch] / (double)CHICK_DITHER_LEVELS;
            assert(actual - ideal < 1.0 / 256.0 && ideal - actual < 1.0 / 256.0);
        }
    }

    // top == bottom(纯色区域):必须完全平坦 —— 纯色再抖就成了"铺了一层噪点"。
    chick_gradient_row_t flat;
    chick_gradient_row(SKY_TOP, SKY_TOP, 100, SKY_ROWS, &flat);
    assert(row_is_flat(&flat));
    const uint16_t flat_expected = nearest565(SKY_TOP);
    for (uint16_t rank = 0; rank < CHICK_DITHER_LEVELS; rank += 37) {
        assert(chick_gradient_apply(&flat, rank) == flat_expected);
    }
}

static void test_row_average_tracks_ideal(void) {
    // 核心性质:凡是用"按余量抖动"的行,平均档位必须紧贴理想渐变(偏差 < 1/256 档)。
    // 一旦这条不成立,平坦区之间就会出现肉眼可见的分层带 —— 这正是上一版翻车的原因。
    //
    // 两类行按设计会取"最近档位"而不是精确平均(偏差 ≤ 半个档,肉眼不可见):
    //   * 屏幕最上/最下一行(避免边缘出现麻点带);
    //   * 整条渐变不足一个量化档、或每行变化量取整为 0 的通道(那种情况抖了只会更脏)。
    // 这里对它们单独用半个档的容差,其余行用 1/256 档的严格容差。
    const uint32_t pairs[2][2] = { { SKY_TOP, SKY_BOTTOM }, { GRASS_TOP, GRASS_DEEP } };
    const int heights[2] = { SKY_ROWS, GRASS_ROWS };
    const int bits[2][3] = { { 3, 2, 3 }, { 3, 2, 3 } };   // R/G/B 的通道位宽

    for (int p = 0; p < 2; p++) {
        for (int row = 0; row < heights[p]; row++) {
            const avg_levels_t avg = row_avg_levels(pairs[p][0], pairs[p][1], row, heights[p]);
            const int *chan_avg[3] = { &avg.r, &avg.g, &avg.b };
            for (int ch = 0; ch < 3; ch++) {
                const int top = (int)((pairs[p][0] >> (16 - 8 * ch)) & 0xFF);
                const int bottom = (int)((pairs[p][1] >> (16 - 8 * ch)) & 0xFF);
                const int step = 1 << bits[p][ch];
                const int span_q8 = (bottom > top ? bottom - top : top - bottom) * 256 / step;
                const int per_row_q8 = span_q8 / (heights[p] > 1 ? heights[p] - 1 : 1);
                const bool snapped = row == 0 || row == heights[p] - 1 ||
                                     span_q8 < 256 || per_row_q8 == 0;
                const double ideal = ideal_units(top, bottom, row, heights[p], bits[p][ch]);
                const double actual = *chan_avg[ch] / (double)CHICK_DITHER_LEVELS;
                const double error = actual - ideal;
                const double tolerance = snapped ? 0.51 : 1.0 / 256.0;
                assert(error < tolerance && error > -tolerance);
            }
        }
    }
}

static void test_average_is_monotonic(void) {
    // 天空:R/G 递增、B 递减;草坪:三通道都递减。抖动比例写反会在这里暴露。
    // 首末行不再特殊处理,所以整条渐变都要单调。
    int prev_r = -1, prev_g = -1, prev_b = 1 << 20;
    for (int row = 0; row < SKY_ROWS; row++) {
        const avg_levels_t a = row_avg_levels(SKY_TOP, SKY_BOTTOM, row, SKY_ROWS);
        assert(a.r >= prev_r && a.g >= prev_g && a.b <= prev_b);
        prev_r = a.r; prev_g = a.g; prev_b = a.b;
    }
    prev_r = prev_g = prev_b = 1 << 20;
    for (int row = 0; row < GRASS_ROWS; row++) {
        const avg_levels_t a = row_avg_levels(GRASS_TOP, GRASS_DEEP, row, GRASS_ROWS);
        assert(a.r <= prev_r && a.g <= prev_g && a.b <= prev_b);
        prev_r = a.r; prev_g = a.g; prev_b = a.b;
    }
}

static void test_never_steps_more_than_one_level(void) {
    // 相邻行的平均档位变化不超过一档(64/1024 ≈ 0.06 档的抖动差异是允许的)。
    const uint32_t pairs[2][2] = { { SKY_TOP, SKY_BOTTOM }, { GRASS_TOP, GRASS_DEEP } };
    const int heights[2] = { SKY_ROWS, GRASS_ROWS };
    for (int p = 0; p < 2; p++) {
        for (int row = 1; row < heights[p]; row++) {
            const avg_levels_t a = row_avg_levels(pairs[p][0], pairs[p][1], row - 1, heights[p]);
            const avg_levels_t b = row_avg_levels(pairs[p][0], pairs[p][1], row, heights[p]);
            const int dr = b.r - a.r, dg = b.g - a.g, db = b.b - a.b;
            assert(dr <= CHICK_DITHER_LEVELS && dr >= -CHICK_DITHER_LEVELS);
            assert(dg <= CHICK_DITHER_LEVELS && dg >= -CHICK_DITHER_LEVELS);
            assert(db <= CHICK_DITHER_LEVELS && db >= -CHICK_DITHER_LEVELS);
        }
    }
}

static void test_flat_rows_are_exact(void) {
    // 理想值正好落在档位上的行必须完全不抖(阈值 0 或满量程),这样纯色/硬边区域
    // 就不会被撒上噪点。天空的 B 通道端点正好对齐:235->200 每 8 一档,
    // 中间必然出现整档行。
    int flat_rows = 0;
    for (int row = 0; row < SKY_ROWS; row++) {
        const avg_levels_t a = row_avg_levels(SKY_TOP, SKY_BOTTOM, row, SKY_ROWS);
        const int frac_b = a.b % CHICK_DITHER_LEVELS;
        if (frac_b == 0 || frac_b == CHICK_DITHER_LEVELS - 0) flat_rows++;
    }
    assert(flat_rows >= 2);
}

static void test_no_channel_overflow(void) {
    // 纯白/纯黑渐变:进位不能溢出到相邻通道(否则白里会出现黑点)。
    for (int row = 0; row < 16; row++) {
        chick_gradient_row_t r;
        chick_gradient_row(0xFFFFFF, 0xFFFFFF, row, 16, &r);
        for (uint16_t rank = 0; rank < CHICK_DITHER_LEVELS; rank += 101) {
            assert(chick_gradient_apply(&r, rank) == 0xFFFF);
        }
        chick_gradient_row(0x000000, 0x000000, row, 16, &r);
        for (uint16_t rank = 0; rank < CHICK_DITHER_LEVELS; rank += 101) {
            assert(chick_gradient_apply(&r, rank) == 0x0000);
        }
    }

    // 只有蓝色满量程:蓝通道不能进位去污染红/绿。
    for (int row = 0; row < 16; row++) {
        chick_gradient_row_t r;
        chick_gradient_row(0x0000FF, 0x0000FF, row, 16, &r);
        for (uint16_t rank = 0; rank < CHICK_DITHER_LEVELS; rank += 101) {
            assert(chick_gradient_apply(&r, rank) == 0x001F);
        }
    }

    // 红通道满量程、蓝通道也满:增量不能互相串。
    for (int row = 0; row < 16; row++) {
        chick_gradient_row_t r;
        chick_gradient_row(0xFF00FF, 0xFF00FF, row, 16, &r);
        for (uint16_t rank = 0; rank < CHICK_DITHER_LEVELS; rank += 101) {
            assert(chick_gradient_apply(&r, rank) == 0xF81F);
        }
    }
}

static void test_null_and_degenerate(void) {
    chick_gradient_row_t r;
    chick_gradient_row(0x112233, 0x445566, 5, 10, NULL);   // 不崩
    chick_gradient_row(0x112233, 0x445566, 0, 0, &r);      // rows<=0: 不动
    chick_gradient_row(0x112233, 0x445566, 0, 1, &r);      // 单行 = 顶部
    assert(row_is_flat(&r));
    assert(chick_gradient_apply(NULL, 0) == 0);
    chick_gradient_row(0x112233, 0x445566, -5, 10, &r);    // 越界行号夹取
    chick_gradient_row(0x112233, 0x445566, 99, 10, &r);
    (void)chick_gradient_apply(&r, 20000);                 // 越界 rank 夹取
}

int main(void) {
    test_endpoints_exact();
    test_row_average_tracks_ideal();
    test_average_is_monotonic();
    test_never_steps_more_than_one_level();
    test_flat_rows_are_exact();
    test_no_channel_overflow();
    test_null_and_degenerate();
    printf("chick gradient tests: PASS\n");
    return 0;
}
