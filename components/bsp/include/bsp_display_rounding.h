// components/bsp/include/bsp_display_rounding.h
// 圆角几何(半径由板级参数给出,本板为 0)。纯几何、不依赖 ESP-IDF/LVGL,主机测试直接覆盖;
// 自己在帧缓冲上渲染的开发者应当在推屏前用它把圆角外涂黑。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 返回该行可见的横向区间(含端点);行越界返回 false。
bool bsp_display_rounded_row_span(int32_t y, int32_t width, int32_t height,
                                  int32_t radius, int32_t *x1, int32_t *x2);

// 该像素是否落在圆角矩形之外(应涂黑)。
bool bsp_display_pixel_outside_rounded_rect(int32_t x, int32_t y,
                                            int32_t width, int32_t height,
                                            int32_t radius);
