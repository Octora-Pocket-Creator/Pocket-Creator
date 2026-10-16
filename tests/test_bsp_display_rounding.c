// tests/test_bsp_display_rounding.c —— 圆角几何(显示层不再有 LVGL,只测几何)。
#include <stdbool.h>
#include <stdio.h>

#include "bsp_display_rounding.h"

#define FAIL(...) do { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                       printf("\n"); failures++; } while (0)

static int failures;

int main(void) {
    const int32_t w = 240, h = 320, r = 30;
    // 中间行:全宽可见
    int32_t x1 = -1, x2 = -1;
    if (!bsp_display_rounded_row_span(h / 2, w, h, r, &x1, &x2)) FAIL("中间行应可见");
    if (x1 != 0 || x2 != w - 1) FAIL("中间行应满宽: %d..%d", (int)x1, (int)x2);
    // 顶行:两侧被切掉,且左右对称
    if (!bsp_display_rounded_row_span(0, w, h, r, &x1, &x2)) FAIL("顶行应可见");
    if (x1 <= 0 || x2 >= w - 1) FAIL("顶行两侧应被切: %d..%d", (int)x1, (int)x2);
    if (x1 != w - 1 - x2) FAIL("顶行左右应对称");
    // 圆角外/内判定与 span 一致
    for (int32_t y = 0; y < h; y += 7) {
        int32_t a = 0, b = w - 1;
        if (!bsp_display_rounded_row_span(y, w, h, r, &a, &b)) FAIL("行 %d 应可见", (int)y);
        if (bsp_display_pixel_outside_rounded_rect(a, y, w, h, r)) FAIL("span 左端点在圆角外");
        if (bsp_display_pixel_outside_rounded_rect(b, y, w, h, r)) FAIL("span 右端点在圆角外");
        if (!bsp_display_pixel_outside_rounded_rect(a - 1, y, w, h, r)) FAIL("span 左边一点应在圆角外");
        if (!bsp_display_pixel_outside_rounded_rect(b + 1, y, w, h, r)) FAIL("span 右边一点应在圆角外");
    }
    // 越界行返回 false
    if (bsp_display_rounded_row_span(-1, w, h, r, &x1, &x2)) FAIL("负行应返回 false");
    if (bsp_display_rounded_row_span(h, w, h, r, &x1, &x2)) FAIL("越界行应返回 false");
    // 半径为 0 时整屏可见
    if (!bsp_display_rounded_row_span(0, w, h, 0, &x1, &x2) || x1 != 0 || x2 != w - 1) {
        FAIL("半径为 0 时应满宽");
    }
    if (failures) { printf("test_bsp_display_rounding: %d 项失败\n", failures); return 1; }
    printf("test_bsp_display_rounding: PASS\n");
    return 0;
}
