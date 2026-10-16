// main/chick_run_glyphs.h —— 应用自己的字形表(生成物)。
//
// 零 LVGL:运行时不链接 LVGL,字形位图与度量都在这里。排版语义与 LVGL 完全一致
// (公式见 main/chick_ui_draw.c),所以换引擎不会让画面变样。
#pragma once

#include <stdint.h>

// 4bpp 位图按【连续半字节】打包,高半字节在前,行间不补齐(与 lv_font_conv 输出一致)。
typedef struct {
    uint32_t bitmap_index;   // 在本字体位图里的字节偏移
    uint16_t adv_w;          // 前进量,单位 1/16 px(和 LVGL 一样)
    uint8_t  box_w;          // 字形位图宽(像素)
    uint8_t  box_h;          // 字形位图高(像素)
    int8_t   ofs_x;          // 相对笔位置的左偏移
    int8_t   ofs_y;          // 相对基线的偏移(与 LVGL 同向)
} chick_glyph_dsc_t;

// 码点 -> 字形下标,按码点升序(二分查找)。
typedef struct {
    uint32_t code;
    uint16_t gid;
} chick_glyph_map_t;

typedef struct {
    const uint8_t *bitmap;
    uint32_t bitmap_size;
    const chick_glyph_dsc_t *glyphs;
    uint32_t glyph_count;
    const chick_glyph_map_t *map;
    uint32_t map_count;
    uint8_t line_height;    // 行高(像素)
    uint8_t base_line;      // 基线到行底的距离(像素)
} chick_font_t;

extern const chick_font_t chick_font_14;   // 最小号(FPS 这类纯数字)
extern const chick_font_t chick_font_20;   // 正文 / HUD 小字
extern const chick_font_t chick_font_28;   // 标题 / HUD 大号分数
