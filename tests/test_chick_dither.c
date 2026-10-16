// tests/test_chick_dither.c —— 蓝噪声抖动掩码的主机测试。
//
// 掩码是生成物,最容易出问题的地方是"生成参数变了但没人发现":掩码一旦退化成
// 有规则的图案,实机上就会看到网格或条纹。这里把两条硬性质钉住:
//   1. rank 必须是 0..1023 的一个排列(每个阈值都有对应像素,占空比才精确);
//   2. 各占空比下,同时被点亮的相邻对必须明显少于独立随机 —— 这是"蓝噪声"的定义,
//      也是"看不出规则纹理"的量化判据。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "chick_dither.h"

static void test_rank_is_a_permutation(void) {
    int seen[CHICK_DITHER_LEVELS] = { 0 };
    for (int y = 0; y < CHICK_DITHER_SIZE; y++) {
        for (int x = 0; x < CHICK_DITHER_SIZE; x++) {
            const uint16_t rank = chick_dither_mask(x, y);
            assert(rank < CHICK_DITHER_LEVELS);
            seen[rank]++;
        }
    }
    for (int i = 0; i < CHICK_DITHER_LEVELS; i++) assert(seen[i] == 1);
}

static void test_no_axis_clustering(void) {
    // on-on 比率 = 相邻两点同时被点亮的比例 / 独立随机下的期望(p²)。
    // 白噪声约为 1.0;蓝噪声 < 1(相邻像素倾向于互相排斥);> 1 说明成团(有纹理)。
    const double duty[] = { 0.1, 0.25, 0.5, 0.75, 0.9 };
    const int offsets[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };

    for (size_t d = 0; d < sizeof(duty) / sizeof(duty[0]); d++) {
        const int threshold = (int)(CHICK_DITHER_LEVELS * duty[d]);
        const double expected = duty[d] * duty[d];
        double worst = 0.0;
        for (int o = 0; o < 4; o++) {
            int pairs = 0;
            for (int y = 0; y < CHICK_DITHER_SIZE; y++) {
                for (int x = 0; x < CHICK_DITHER_SIZE; x++) {
                    const int nx = (x + offsets[o][0] + CHICK_DITHER_SIZE) % CHICK_DITHER_SIZE;
                    const int ny = (y + offsets[o][1] + CHICK_DITHER_SIZE) % CHICK_DITHER_SIZE;
                    if (chick_dither_mask(x, y) < (uint16_t)threshold &&
                        chick_dither_mask(nx, ny) < (uint16_t)threshold) {
                        pairs++;
                    }
                }
            }
            const double ratio = (pairs / (double)(CHICK_DITHER_SIZE * CHICK_DITHER_SIZE)) /
                                 expected;
            if (ratio > worst) worst = ratio;
        }
        printf("  占空比 %.2f:最差方向 on-on 比率 %.2f\n", duty[d], worst);
        assert(worst < 1.0);        // 必须优于独立随机(不能成团)
    }
}

static void test_no_small_period(void) {
    // 另一种"看得出纹理"的坏情况:掩码在小周期上自我重复(老实现的 4 行周期就是这样)。
    // 50% 阈值下,把图案与自身平移 s 比较:独立随机约 0.5,周期图案接近 1.0。
    const uint16_t threshold = CHICK_DITHER_LEVELS / 2;
    for (int s = 1; s <= 8; s++) {
        int match = 0, total = 0;
        for (int y = 0; y < CHICK_DITHER_SIZE; y++) {
            for (int x = 0; x < CHICK_DITHER_SIZE; x++) {
                const int sx = (x + s) % CHICK_DITHER_SIZE;
                const int sy = (y + s) % CHICK_DITHER_SIZE;
                const int a = chick_dither_mask(x, y) < threshold;
                const int bx = chick_dither_mask(sx, y) < threshold;
                const int by = chick_dither_mask(x, sy) < threshold;
                if (a == bx) match++;
                if (a == by) match++;
                total += 2;
            }
        }
        const double rate = match / (double)total;
        printf("  平移 %d 像素的自相似率 %.3f\n", s, rate);
        assert(rate < 0.65);
    }
}

static void test_same_duty_is_evenly_spread(void) {
    // 25% 占空比时,任何 4x4 窗口里的点亮数量不能太多 —— 局部成块就是可见的"颗粒块"。
    const int threshold = CHICK_DITHER_LEVELS / 4;
    int max_in_window = 0;
    for (int y = 0; y < CHICK_DITHER_SIZE; y++) {
        for (int x = 0; x < CHICK_DITHER_SIZE; x++) {
            int on = 0;
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    if (chick_dither_mask((x + dx) % CHICK_DITHER_SIZE,
                                          (y + dy) % CHICK_DITHER_SIZE) < (uint16_t)threshold) {
                        on++;
                    }
                }
            }
            if (on > max_in_window) max_in_window = on;
        }
    }
    printf("  25%% 占空比 4x4 窗口最多 %d 个点亮(均值 4)\n", max_in_window);
    assert(max_in_window <= 8);
}

static void test_coordinates_wrap_and_are_safe(void) {
    // 坐标语义(实现里写明的两条,测试按它钉住):
    //   * 正方向越界按位与环绕(周期 = 掩码边长);
    //   * 负坐标取绝对值(镜像),避免 C 的负数取模返回负值。
    // 正常填充不会给出负坐标(脏区已裁剪),这里只要求"不越界、结果确定"。
    for (int i = 0; i < CHICK_DITHER_SIZE; i++) {
        assert(chick_dither_mask(-i, 0) == chick_dither_mask(i, 0));
        assert(chick_dither_mask(0, -i) == chick_dither_mask(0, i));
        assert(chick_dither_mask(i + CHICK_DITHER_SIZE, i) == chick_dither_mask(i, i));
        assert(chick_dither_mask(i, i + 2 * CHICK_DITHER_SIZE) == chick_dither_mask(i, i));
    }
    assert(chick_dither_mask(100000, -100000) < CHICK_DITHER_LEVELS);
}

static void test_mask_is_stable(void) {
    // 掩码是查表,同一坐标必须永远返回同一个值(画面可重复、不会每帧闪)。
    for (int y = 0; y < CHICK_DITHER_SIZE; y++) {
        for (int x = 0; x < CHICK_DITHER_SIZE; x++) {
            const uint16_t a = chick_dither_mask(x, y);
            for (int k = 0; k < 4; k++) assert(chick_dither_mask(x, y) == a);
        }
    }
    // 相邻两行不能完全一样(否则纵向会出现 2 行的重复纹理)。
    int identical = 0;
    for (int x = 0; x < CHICK_DITHER_SIZE; x++) {
        if (chick_dither_mask(x, 0) == chick_dither_mask(x, 1)) identical++;
    }
    assert(identical < CHICK_DITHER_SIZE / 2);
}

int main(void) {
    test_rank_is_a_permutation();
    test_no_axis_clustering();
    test_no_small_period();
    test_same_duty_is_evenly_spread();
    test_coordinates_wrap_and_are_safe();
    test_mask_is_stable();
    printf("chick dither tests: PASS\n");
    return 0;
}
