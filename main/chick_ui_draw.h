// main/chick_ui_draw.h —— 应用自己的 2D 绘制原语(零 LVGL)。
//
// 这一层只认“一块 RGB565 像素缓冲 + 行跨距”,不依赖 LVGL / ESP-IDF,可在主机上
// 直接编译测试。页面(HUD/首页/设置页)与游戏世界都用它画。
//
// 文字排版公式与 LVGL 一致(见 .c 里的注释),这样从 LVGL 字体换到自有字形表时
// 画面不会变样。
#pragma once

#include <stdint.h>

#include "chick_run_glyphs.h"

// 一块可写的 RGB565 画布。w/h 用于裁剪,越界自动丢弃。
typedef struct {
    uint16_t *px;
    uint32_t stride_px;
    int w, h;
} chick_canvas_t;

// --- 颜色 -------------------------------------------------------------------
// 0xRRGGBB -> RGB565(与 lv_color_hex/to_u16 的结果一致)。
uint16_t chick_rgb565(uint32_t rgb888);

// --- 基本图元 ---------------------------------------------------------------
void chick_fill_rect(const chick_canvas_t *c, int x, int y, int w, int h, uint16_t color);
// 半透明填充(alpha 0..255):调光层、弹层底色用。
void chick_fill_rect_blend(const chick_canvas_t *c, int x, int y, int w, int h,
                           uint16_t color, uint8_t alpha);
// 矩形描边(向内画 width 像素)。
void chick_stroke_rect(const chick_canvas_t *c, int x, int y, int w, int h, int width,
                       uint16_t color);

// --- 精灵 -------------------------------------------------------------------
// RGB565 平面 + 1 字节掩码(非 0 处覆盖)。掩码为 0 处保留底图。
void chick_blit_sprite(const chick_canvas_t *c, int x, int y, const uint16_t *rgb,
                       const uint8_t *mask, int w, int h);
// 单色遮罩:掩码非 0 处涂成 color。HUD 图标(心形)用。
void chick_blit_mask_color(const chick_canvas_t *c, int x, int y, const uint8_t *mask,
                           int w, int h, uint16_t color);

// --- 文字 -------------------------------------------------------------------
typedef struct {
    int width;    // 最宽一行的像素宽
    int height;   // 总高 = 行数 * line_height
    int lines;
} chick_text_metrics_t;

const chick_glyph_dsc_t *chick_glyph(const chick_font_t *font, uint32_t code);
chick_text_metrics_t chick_text_measure(const chick_font_t *font, const char *utf8);

// 在 (x, y) 画一行/多行文字,y 是行框顶部。支持 '\n'。颜色带 Alpha 混合。
void chick_draw_text(const chick_canvas_t *c, int x, int y, const chick_font_t *font,
                     uint16_t color, const char *utf8);
// 居中对齐:x 是中心线。
void chick_draw_text_center(const chick_canvas_t *c, int cx, int y, const chick_font_t *font,
                            uint16_t color, const char *utf8);
// 右对齐:x 是右边界(不含)。
void chick_draw_text_right(const chick_canvas_t *c, int rx, int y, const chick_font_t *font,
                           uint16_t color, const char *utf8);
