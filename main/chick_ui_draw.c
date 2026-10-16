// main/chick_ui_draw.c —— 2D 绘制原语(零 LVGL)。见 chick_ui_draw.h。
#include "chick_ui_draw.h"

#include <stdbool.h>
#include <stddef.h>

// --- 颜色 -------------------------------------------------------------------
uint16_t chick_rgb565(uint32_t rgb888) {
    const uint32_t r = (rgb888 >> 16) & 0xFF;
    const uint32_t g = (rgb888 >> 8) & 0xFF;
    const uint32_t b = rgb888 & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// 565 -> 888(混合用)。
static inline void rgb565_to_888(uint16_t c, int *r, int *g, int *b) {
    *r = (int)(((c >> 11) & 0x1F) << 3);
    *g = (int)(((c >> 5) & 0x3F) << 2);
    *b = (int)((c & 0x1F) << 3);
}

static inline uint16_t rgb888_to_565(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// --- 基本图元 ---------------------------------------------------------------
// 把矩形裁到画布内;返回是否还有效。
static bool clip_rect(const chick_canvas_t *c, int *x, int *y, int *w, int *h) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) return false;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

void chick_fill_rect(const chick_canvas_t *c, int x, int y, int w, int h, uint16_t color) {
    if (c == NULL || c->px == NULL || w <= 0 || h <= 0) return;
    if (!clip_rect(c, &x, &y, &w, &h)) return;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = c->px + (size_t)(y + row) * c->stride_px + (size_t)x;
        for (int col = 0; col < w; col++) dst[col] = color;
    }
}

void chick_fill_rect_blend(const chick_canvas_t *c, int x, int y, int w, int h,
                           uint16_t color, uint8_t alpha) {
    if (c == NULL || c->px == NULL || w <= 0 || h <= 0 || alpha == 0) return;
    if (alpha == 255) {
        chick_fill_rect(c, x, y, w, h, color);
        return;
    }
    if (!clip_rect(c, &x, &y, &w, &h)) return;
    int sr, sg, sb;
    rgb565_to_888(color, &sr, &sg, &sb);
    for (int row = 0; row < h; row++) {
        uint16_t *dst = c->px + (size_t)(y + row) * c->stride_px + (size_t)x;
        for (int col = 0; col < w; col++) {
            int dr, dg, db;
            rgb565_to_888(dst[col], &dr, &dg, &db);
            dst[col] = rgb888_to_565(dr + ((sr - dr) * alpha) / 255,
                                     dg + ((sg - dg) * alpha) / 255,
                                     db + ((sb - db) * alpha) / 255);
        }
    }
}

void chick_stroke_rect(const chick_canvas_t *c, int x, int y, int w, int h, int width,
                       uint16_t color) {
    if (width <= 0) return;
    chick_fill_rect(c, x, y, w, width, color);                       // 上
    chick_fill_rect(c, x, y + h - width, w, width, color);           // 下
    chick_fill_rect(c, x, y + width, width, h - 2 * width, color);   // 左
    chick_fill_rect(c, x + w - width, y + width, width, h - 2 * width, color);  // 右
}

// --- 精灵 -------------------------------------------------------------------
void chick_blit_sprite(const chick_canvas_t *c, int x, int y, const uint16_t *rgb,
                       const uint8_t *mask, int w, int h) {
    if (c == NULL || c->px == NULL || rgb == NULL || mask == NULL) return;
    int src_x = 0, src_y = 0, draw_w = w, draw_h = h;
    if (x < 0) { src_x = -x; draw_w += x; x = 0; }
    if (y < 0) { src_y = -y; draw_h += y; y = 0; }
    if (x + draw_w > c->w) draw_w = c->w - x;
    if (y + draw_h > c->h) draw_h = c->h - y;
    if (draw_w <= 0 || draw_h <= 0) return;
    for (int row = 0; row < draw_h; row++) {
        const uint16_t *src = rgb + (size_t)(src_y + row) * w + (size_t)src_x;
        const uint8_t *m = mask + (size_t)(src_y + row) * w + (size_t)src_x;
        uint16_t *dst = c->px + (size_t)(y + row) * c->stride_px + (size_t)x;
        for (int col = 0; col < draw_w; col++) {
            if (m[col]) dst[col] = src[col];
        }
    }
}

void chick_blit_mask_color(const chick_canvas_t *c, int x, int y, const uint8_t *mask,
                           int w, int h, uint16_t color) {
    if (c == NULL || c->px == NULL || mask == NULL) return;
    int src_x = 0, src_y = 0, draw_w = w, draw_h = h;
    if (x < 0) { src_x = -x; draw_w += x; x = 0; }
    if (y < 0) { src_y = -y; draw_h += y; y = 0; }
    if (x + draw_w > c->w) draw_w = c->w - x;
    if (y + draw_h > c->h) draw_h = c->h - y;
    if (draw_w <= 0 || draw_h <= 0) return;
    for (int row = 0; row < draw_h; row++) {
        const uint8_t *m = mask + (size_t)(src_y + row) * w + (size_t)src_x;
        uint16_t *dst = c->px + (size_t)(y + row) * c->stride_px + (size_t)x;
        for (int col = 0; col < draw_w; col++) {
            if (m[col]) dst[col] = color;
        }
    }
}

// --- 文字 -------------------------------------------------------------------
static uint32_t utf8_next(const char **text) {
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t code = 0;
    if (p[0] < 0x80) {
        code = p[0];
        *text += 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        code = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        *text += 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        code = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
               (uint32_t)(p[2] & 0x3F);
        *text += 3;
    } else {
        code = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
               ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        *text += 4;
    }
    return code;
}

const chick_glyph_dsc_t *chick_glyph(const chick_font_t *font, uint32_t code) {
    if (font == NULL) return NULL;
    uint32_t lo = 0, hi = font->map_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (font->map[mid].code < code) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= font->map_count || font->map[lo].code != code) return NULL;
    const uint32_t gid = font->map[lo].gid;
    if (gid >= font->glyph_count) return NULL;
    return &font->glyphs[gid];
}

// 前进量:LVGL 把 adv_w 存在 1/16 px,(adv_w + 8) >> 4 才是像素。
static inline int glyph_advance_px(const chick_glyph_dsc_t *g) {
    return (int)((g->adv_w + 8u) >> 4);
}

// 量一段文字(不含换行)的像素宽。行对齐要用它。
static int measure_span(const chick_font_t *font, const char *text, size_t length) {
    int width = 0;
    const char *p = text;
    const char *end = text + length;
    while (p < end && *p) {
        const uint32_t code = utf8_next(&p);
        const chick_glyph_dsc_t *g = chick_glyph(font, code);
        width += (g != NULL) ? glyph_advance_px(g) : (int)font->line_height / 2;
    }
    return width;
}

chick_text_metrics_t chick_text_measure(const chick_font_t *font, const char *utf8) {
    chick_text_metrics_t metrics = { 0, 0, 0 };
    if (font == NULL || utf8 == NULL) return metrics;
    const char *p = utf8;
    int line_w = 0;
    int lines = 1;
    while (*p) {
        const uint32_t code = utf8_next(&p);
        if (code == '\n') {
            if (line_w > metrics.width) metrics.width = line_w;
            line_w = 0;
            lines++;
            continue;
        }
        const chick_glyph_dsc_t *g = chick_glyph(font, code);
        line_w += (g != NULL) ? glyph_advance_px(g) : (int)font->line_height / 2;
    }
    if (line_w > metrics.width) metrics.width = line_w;
    metrics.lines = lines;
    metrics.height = lines * (int)font->line_height;
    return metrics;
}

// 画一个字形的 4bpp 位图。位图打包方式与 lv_font_conv 一致:
// 每个字节两个像素、高半字节在前、行与行之间【不补齐】(奇数宽度会跨字节)。
static void draw_glyph(const chick_canvas_t *c, const chick_font_t *font,
                       const chick_glyph_dsc_t *g, int gx, int gy, int fg_r, int fg_g, int fg_b) {
    if (g->box_w == 0 || g->box_h == 0) return;
    // 裁剪:整块在外就跳过。
    if (gx >= c->w || gy >= c->h || gx + g->box_w <= 0 || gy + g->box_h <= 0) return;
    const uint8_t *bitmap = font->bitmap + g->bitmap_index;
    int shift = 8;
    uint8_t cur = 0;
    for (int row = 0; row < g->box_h; row++) {
        const int y = gy + row;
        int x = gx;
        if (shift == 4) {                      // 上一行是奇数宽,低半字节属于本行第一个像素
            if (y >= 0 && y < c->h && x >= 0 && x < c->w) {
                const int cov = cur & 0xF;
                if (cov) {
                    uint16_t *dst = c->px + (size_t)y * c->stride_px + (size_t)x;
                    if (cov == 15) *dst = rgb888_to_565(fg_r, fg_g, fg_b);
                    else {
                        int dr, dg, db;
                        rgb565_to_888(*dst, &dr, &dg, &db);
                        *dst = rgb888_to_565(dr + ((fg_r - dr) * cov) / 15,
                                             dg + ((fg_g - dg) * cov) / 15,
                                             db + ((fg_b - db) * cov) / 15);
                    }
                }
            }
            x++;
            shift = 8;
        }
        while (x + 1 < gx + g->box_w) {
            cur = *bitmap++;
            const int hi = cur >> 4, lo = cur & 0xF;
            for (int part = 0; part < 2; part++) {
                const int cov = part == 0 ? hi : lo;
                const int px = x + part;
                if (cov && y >= 0 && y < c->h && px >= 0 && px < c->w) {
                    uint16_t *dst = c->px + (size_t)y * c->stride_px + (size_t)px;
                    if (cov == 15) *dst = rgb888_to_565(fg_r, fg_g, fg_b);
                    else {
                        int dr, dg, db;
                        rgb565_to_888(*dst, &dr, &dg, &db);
                        *dst = rgb888_to_565(dr + ((fg_r - dr) * cov) / 15,
                                             dg + ((fg_g - dg) * cov) / 15,
                                             db + ((fg_b - db) * cov) / 15);
                    }
                }
            }
            x += 2;
        }
        if (x < gx + g->box_w) {               // 奇数宽度:高半字节是本行最后一像素
            cur = *bitmap++;
            const int cov = cur >> 4;
            if (cov && y >= 0 && y < c->h && x >= 0 && x < c->w) {
                uint16_t *dst = c->px + (size_t)y * c->stride_px + (size_t)x;
                if (cov == 15) *dst = rgb888_to_565(fg_r, fg_g, fg_b);
                else {
                    int dr, dg, db;
                    rgb565_to_888(*dst, &dr, &dg, &db);
                    *dst = rgb888_to_565(dr + ((fg_r - dr) * cov) / 15,
                                         dg + ((fg_g - dg) * cov) / 15,
                                         db + ((fg_b - db) * cov) / 15);
                }
            }
            x++;
            shift = 4;
        }
    }
}

static void draw_line(const chick_canvas_t *c, int x, int y, const chick_font_t *font,
                      int fg_r, int fg_g, int fg_b, const char *text, size_t length) {
    const int baseline_y = y + (int)font->line_height - (int)font->base_line;
    int pen_x = x;
    const char *p = text;
    const char *end = text + length;
    while (p < end && *p) {
        const uint32_t code = utf8_next(&p);
        const chick_glyph_dsc_t *g = chick_glyph(font, code);
        if (g == NULL) {
            pen_x += (int)font->line_height / 2;
            continue;
        }
        draw_glyph(c, font, g, pen_x + g->ofs_x, baseline_y - g->box_h - g->ofs_y,
                   fg_r, fg_g, fg_b);
        pen_x += glyph_advance_px(g);
    }
}

static void draw_text_common(const chick_canvas_t *c, int x, int y, const chick_font_t *font,
                             uint16_t color, const char *utf8, int align, int anchor) {
    if (c == NULL || c->px == NULL || font == NULL || utf8 == NULL) return;
    int fg_r, fg_g, fg_b;
    rgb565_to_888(color, &fg_r, &fg_g, &fg_b);

    const char *line_start = utf8;
    const char *p = utf8;
    int line_y = y;
    for (;;) {
        if (*p == '\n' || *p == '\0') {
            const size_t length = (size_t)(p - line_start);
            if (length > 0) {
                int line_x = x;
                if (align != 0) {
                    const int width = measure_span(font, line_start, length);
                    line_x = (align < 0) ? anchor - width / 2 : anchor - width;
                }
                draw_line(c, line_x, line_y, font, fg_r, fg_g, fg_b, line_start, length);
            }
            if (*p == '\0') break;
            line_y += (int)font->line_height;
            line_start = ++p;
            continue;
        }
        p++;
    }
}

void chick_draw_text(const chick_canvas_t *c, int x, int y, const chick_font_t *font,
                     uint16_t color, const char *utf8) {
    draw_text_common(c, x, y, font, color, utf8, 0, x);
}

void chick_draw_text_center(const chick_canvas_t *c, int cx, int y, const chick_font_t *font,
                            uint16_t color, const char *utf8) {
    draw_text_common(c, cx, y, font, color, utf8, -1, cx);
}

void chick_draw_text_right(const chick_canvas_t *c, int rx, int y, const chick_font_t *font,
                           uint16_t color, const char *utf8) {
    draw_text_common(c, rx, y, font, color, utf8, 1, rx);
}
