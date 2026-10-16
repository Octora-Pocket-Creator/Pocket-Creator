// main/tetris_ui.c —— 俄罗斯方块的界面与交互(零 LVGL 自绘,240x320 ST7789 SPI 屏)。
//
// 本应用只做一件事:俄罗斯方块。开机直接进首页,没有游戏选择菜单 ——
// 同一份菜单在开发过程中用过(在小鸡快跑与本应用之间切换),但同一个界面同时承担
// "选游戏"与"玩游戏"会让渲染任务的生命周期变得复杂,而这里只需要一个游戏。
//   * 画布 = 一块内部 RAM 的 RGB565 位图,每帧只把【变化过的矩形】推给面板;
//   * 不依赖 PSRAM,不烘焙整屏;背景渐变按行算好颜色表现场填(与小鸡快跑共用
//     chick_gradient/chick_dither,所以天空那种"大条分层"的坑不会在这里重踩);
//   * 推屏前统一做色温补偿(chick_color),与小鸡快跑共用 NVS 里的暖度设置;
//   * 渲染任务不绑核(单核 C3 上绑核会直接创建失败)。
//
// 与小鸡快跑最大的不同是【脏区从哪来】:跑酷里每个元素自己登记脏区,方块游戏的状态
// 面更小(棋盘 + 当前方块 + 幽灵 + 侧栏数字),所以这里改成与上一帧快照对比自动算出
// 脏区 —— "忘记登记脏区"这类残影 bug 在实机上才看得见,自动对比从结构上排除了它。
#include "tetris_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "chick_color.h"
#include "chick_dither.h"
#include "chick_gradient.h"
#include "chick_run_glyphs.h"
#include "chick_run_settings.h"
#include "chick_run_store.h"
#include "chick_ui_draw.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rtttl_player.h"
#include "tetris_model.h"
#include "tetris_music.h"
#include "tetris_store.h"
#include "tetris_text.h"

static const char *TAG = "tetris_ui";

// --- 版面(竖屏 240x320,1 像素 = 1 逻辑像素)------------------------------
#define UI_W        240
#define UI_H        320
#define HEADER_H    30

// 棋盘:10 列 x 20 行,每格 14 像素 -> 140x280,靠左摆放。
#define CELL        14
#define BOARD_X     6
#define BOARD_Y     (HEADER_H + 2)          // 32
#define BOARD_W     (TETRIS_COLS * CELL)    // 140
#define BOARD_H     (TETRIS_ROWS * CELL)    // 280

// 侧栏:下一个方块预览 + 分数/等级/消行 + 帧率。
#define PANEL_X     150
#define PANEL_W     (UI_W - PANEL_X - 2)    // 88
#define PANEL_Y     (BOARD_Y - 2)
#define PANEL_H     (BOARD_H + 4)

#define PREVIEW_BOX    40
#define PREVIEW_GAP    4
#define PREVIEW_CELL   9
#define NEXT_LABEL_Y   (BOARD_Y + 2)        // 34
#define PREVIEW_Y      (NEXT_LABEL_Y + 24)  // 58
// 行距按 font 20 的行高 24 排:标签行与数值行必须各占满一行,否则会互相压字。
#define SCORE_LABEL_Y  (PREVIEW_Y + 3 * (PREVIEW_BOX + PREVIEW_GAP) + 2)   // 192
#define SCORE_VALUE_Y  (SCORE_LABEL_Y + 24)                                // 216
#define LEVEL_Y        (SCORE_VALUE_Y + 26)                                // 242
#define LINES_Y        (LEVEL_Y + 24)                                      // 266
// FPS 用 font 14(行高 17):292 起、面板底 314,正好收在面板内。
#define FPS_Y          (LINES_Y + 28)                                      // 294

// 节奏与上限。
#define UI_FRAME_MS      20       // 目标 50 FPS(实际帧率由推屏像素量决定)
#define UI_MAX_DT_MS     100      // 单帧最多推进 100ms:掉帧不会让方块瞬移
#define STATS_MS         5000
#define BATTERY_MS       10000
#define DAS_MS           160      // 左右键首次自动重复的延迟
#define ARR_MS           55       // 自动重复间隔
#define BLINK_MS         620      // 首页"按 A 开始"的闪烁周期
#define TOAST_MS         900      // 升级/四行消除提示的停留时间
#define TRAIL_MS         130      // 硬降轨迹线的停留时间(画一次、到点擦掉)
#define IDLE_PAUSE_MS    60000    // 游戏中一分钟没操作就自动暂停(护屏 + 省电)
#define FRAME_TASK_STACK 4096
#define FRAME_TASK_PRIO  4
#define DAMAGE_MAX       16
#define STAGE_ROWS       64

// --- 配色 -------------------------------------------------------------------
// 深色底 + 高饱和方块:深底让 7 种颜色都跳出来,长时间点亮也不刺眼。
#define COLOR_BG_TOP     0x0B1020
#define COLOR_BG_BOTTOM  0x1D1436
#define COLOR_BOARD_BG   0x080B14
#define COLOR_GRID       0x141B2B
#define COLOR_PANEL_BG   0x121A2B
#define COLOR_PANEL_LINE 0x27324A
#define COLOR_INK        0xE9EEF8
#define COLOR_INK_SOFT   0x8FA2C0
#define COLOR_ACCENT     0xFACC15
#define COLOR_DANGER     0xEF4444
#define COLOR_BAT_GOOD   0x4ADE80
#define COLOR_BAT_LOW    0xF87171
#define COLOR_OVERLAY    0x04060C
#define COLOR_BAR_BG     0x1B2438
#define COLOR_BAR_FILL   0x38BDF8
#define COLOR_TOAST_BG   0xFACC15
#define COLOR_TOAST_INK  0x1A1405

// 七种方块的标准配色(拟物化的亮面/暗面由 palette_init 现算)。
static const uint32_t BLOCK_RGB[TETRIS_PIECE_COUNT] = {
    0x22D3EE,   // I 青
    0xFACC15,   // O 黄
    0xA855F7,   // T 紫
    0x22C55E,   // S 绿
    0xEF4444,   // Z 红
    0x3B82F6,   // J 蓝
    0xF97316,   // L 橙
};

// --- 类型与全局状态 ---------------------------------------------------------
typedef enum {
    PAGE_TITLE = 0,     // 首页(按 A 开始 / 进设置)
    PAGE_GAME,
    PAGE_SETTINGS,
} page_t;

typedef struct { int x, y, w, h; } rect_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_event_t;

static uint16_t *s_fb;
static chick_canvas_t s_canvas;
static uint16_t *s_stage;
static SemaphoreHandle_t s_flush_done;
static QueueHandle_t s_keys;
static TaskHandle_t s_task;
static volatile bool s_running;
static bool s_first_frame_pushed;
static bool s_backlight_on;

static tetris_model_t s_model;
static page_t s_page;
static uint32_t s_best;          // 俄罗斯方块最高分
static uint32_t s_best_saved;
static chick_run_settings_t s_settings;
static uint8_t s_settings_row;

static uint16_t s_block[TETRIS_PIECE_COUNT][3];   // [方块][0 面 / 1 亮 / 2 暗]
static chick_gradient_row_t s_bg_row[UI_H];
static chick_color_lut_t s_tint_lut;
static bool s_tint_ready;

typedef struct {
    bool held;
    uint32_t next_ms;
} repeat_t;
static repeat_t s_repeat[BSP_BTN_COUNT];

static uint32_t s_last_input_ms;
static const char *s_toast;
static uint32_t s_toast_until_ms;
static bool s_blink_on;

// 动效状态:硬降轨迹(从原位置到落点的一道速度线)。
// 它是"短命叠加层":不进快照对比,自己登记脏区、自己到点擦掉。
typedef struct {
    bool active;
    int8_t x;                 // 方块原点列(棋盘坐标)
    uint8_t rotation, piece;  // 原形状,用来算速度线要盖哪几列
    int from_y, land_y;       // 落下前的行与落点行
    int min_dx, max_dx, min_dy, max_dy;
    uint32_t until_ms;
} trail_t;
static trail_t s_trail;

static int s_battery_soc = -1;
static bool s_battery_present;
static uint32_t s_battery_next_ms;

static uint32_t s_fps_count;
static uint32_t s_fps_last_ms;
static uint32_t s_fps_value;

static uint32_t s_frames;
static uint32_t s_flush_px;
static uint32_t s_flush_us;
static uint32_t s_flush_calls;
static uint32_t s_stats_next_ms;

static rect_t s_damage[DAMAGE_MAX];
static int s_damage_n;

// 上一帧快照:脏区靠它对比出来。
typedef struct {
    uint8_t board[TETRIS_ROWS][TETRIS_COLS];
    uint8_t piece, rotation;
    int8_t x, y;
    bool active;
    int8_t ghost_y;
    uint32_t clear_rows;
    uint8_t clear_flash;
    uint32_t score;
    uint16_t lines;
    uint8_t level;
    uint8_t queue[TETRIS_PREVIEW];
    tetris_state_t state;
    page_t page;
    uint8_t settings_row;
    uint8_t settings_values[3];
    bool blink;
    const char *toast;
    int battery_soc;
    uint32_t fps;
} snapshot_t;
static snapshot_t s_prev;
static bool s_prev_valid;

static const chick_font_t *FONT_BIG = &chick_font_28;
static const chick_font_t *FONT_CJK = &chick_font_20;
static const chick_font_t *FONT_TINY = &chick_font_14;

// --- 小时钟 -----------------------------------------------------------------
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

static int min_int(int a, int b) { return a < b ? a : b; }
static int max_int(int a, int b) { return a > b ? a : b; }

// --- 颜色与背景 -------------------------------------------------------------
// 把 8 位分量朝白(正)或朝黑(负)按百分比插值。方块的高光/暗面一次性算好存表,
// 避免每帧做浮点或反复展开 5/6/5 通道。
static uint16_t shade_rgb565(uint32_t rgb888, int percent) {
    const int src[3] = { (int)((rgb888 >> 16) & 0xFF), (int)((rgb888 >> 8) & 0xFF),
                         (int)(rgb888 & 0xFF) };
    int out[3];
    for (int i = 0; i < 3; i++) {
        out[i] = percent >= 0 ? src[i] + (255 - src[i]) * percent / 100
                              : src[i] + src[i] * percent / 100;
        if (out[i] < 0) out[i] = 0;
        if (out[i] > 255) out[i] = 255;
    }
    return chick_rgb565(((uint32_t)out[0] << 16) | ((uint32_t)out[1] << 8) | (uint32_t)out[2]);
}

static void palette_init(void) {
    for (int i = 0; i < TETRIS_PIECE_COUNT; i++) {
        s_block[i][0] = shade_rgb565(BLOCK_RGB[i], 0);
        s_block[i][1] = shade_rgb565(BLOCK_RGB[i], 45);
        s_block[i][2] = shade_rgb565(BLOCK_RGB[i], -40);
    }
}

static void bg_prepare(void) {
    for (int y = 0; y < UI_H; y++) {
        chick_gradient_row(COLOR_BG_TOP, COLOR_BG_BOTTOM, y, UI_H, &s_bg_row[y]);
    }
}

static void tint_rebuild(void) {
    const chick_color_gain_t gain = chick_color_gain_from_warmth(s_settings.warmth);
    chick_color_lut_build(&gain, &s_tint_lut);
    s_tint_ready = true;
}

// 背景渐变逐像素填(带蓝噪声抖动),只填 clip 覆盖的行与列。
static void paint_bg(const rect_t *clip) {
    for (int y = clip->y; y < clip->y + clip->h; y++) {
        const chick_gradient_row_t *row = &s_bg_row[y];
        uint16_t *dst = s_fb + (size_t)y * UI_W;
        for (int x = clip->x; x < clip->x + clip->w; x++) {
            dst[x] = chick_gradient_apply(row, chick_dither_mask(x, y));
        }
    }
}

// --- 脏区 -------------------------------------------------------------------
static rect_t rect_make(int x, int y, int w, int h) {
    rect_t r = { x, y, w, h };
    if (r.w <= 0 || r.h <= 0) { r.w = 0; r.h = 0; return r; }
    // 夹到屏幕内:脏区只用于推屏,屏幕外的部分没有意义。
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > UI_W) r.w = UI_W - r.x;
    if (r.y + r.h > UI_H) r.h = UI_H - r.y;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static bool rect_empty(const rect_t *r) { return r->w <= 0 || r->h <= 0; }

static void rect_union_into(rect_t *dst, const rect_t *src) {
    const int x0 = min_int(dst->x, src->x);
    const int y0 = min_int(dst->y, src->y);
    const int x1 = max_int(dst->x + dst->w, src->x + src->w);
    const int y1 = max_int(dst->y + dst->h, src->y + src->h);
    dst->x = x0; dst->y = y0; dst->w = x1 - x0; dst->h = y1 - y0;
}

// 加入脏区:与已有矩形相交就合并(合并后只多推一点像素,但少了重复绘制);
// 超过上限时并进第一块,绝不丢区域 —— 丢区域就是残影。
static void damage_add(rect_t r) {
    if (rect_empty(&r)) return;
    for (int i = 0; i < s_damage_n; i++) {
        const rect_t *o = &s_damage[i];
        const bool overlap = r.x <= o->x + o->w && o->x <= r.x + r.w &&
                             r.y <= o->y + o->h && o->y <= r.y + r.h;
        if (overlap) {
            rect_union_into(&s_damage[i], &r);
            return;
        }
    }
    if (s_damage_n < DAMAGE_MAX) {
        s_damage[s_damage_n++] = r;
        return;
    }
    rect_union_into(&s_damage[0], &r);
}

static void damage_add_screen(void) { damage_add(rect_make(0, 0, UI_W, UI_H)); }

static rect_t cell_rect(int bx, int by) {
    return rect_make(BOARD_X + bx * CELL, BOARD_Y + by * CELL, CELL, CELL);
}

// 当前方块(或幽灵)在屏幕上的包围盒。
static rect_t piece_rect(const tetris_model_t *m, int8_t px, int8_t py) {
    int min_x = 4, min_y = 4, max_x = -1, max_y = -1;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (!tetris_piece_cell(m->piece, m->rotation, x, y)) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < 0) return rect_make(0, 0, 0, 0);
    return rect_make(BOARD_X + (px + min_x) * CELL, BOARD_Y + (py + min_y) * CELL,
                     (max_x - min_x + 1) * CELL, (max_y - min_y + 1) * CELL);
}

// --- 方块绘制 ---------------------------------------------------------------
// 拟物化:面色 + 左上亮边 + 右下暗边 + 四边各留 1 像素缝。留缝让相邻的同色方块
// 也能分辨 —— 一屏 200 格,没有缝就是一大片色块。
static void draw_block(int x, int y, int size, uint8_t piece) {
    if (piece == 0 || piece > TETRIS_PIECE_COUNT) return;
    const uint16_t *pal = s_block[piece - 1];
    const int bevel = size >= 12 ? 2 : 1;
    chick_fill_rect(&s_canvas, x + 1, y + 1, size - 2, size - 2, pal[0]);
    chick_fill_rect(&s_canvas, x + 1, y + 1, size - 2, bevel, pal[1]);
    chick_fill_rect(&s_canvas, x + 1, y + 1, bevel, size - 2, pal[1]);
    chick_fill_rect(&s_canvas, x + 1, y + size - 1 - bevel, size - 2, bevel, pal[2]);
    chick_fill_rect(&s_canvas, x + size - 1 - bevel, y + 1, bevel, size - 2, pal[2]);
}

// 幽灵(落点预览):只画 1 像素轮廓。深色底上轮廓最干净,也不会和实体方块混淆。
static void draw_ghost(int x, int y, int size, uint8_t piece) {
    if (piece == 0 || piece > TETRIS_PIECE_COUNT) return;
    chick_stroke_rect(&s_canvas, x + 2, y + 2, size - 4, size - 4, 1, s_block[piece - 1][2]);
}

// --- 棋盘 -------------------------------------------------------------------
static void paint_board(const rect_t *clip) {
    const int bx0 = max_int(0, (clip->x - BOARD_X) / CELL);
    const int bx1 = min_int(TETRIS_COLS - 1, (clip->x + clip->w - 1 - BOARD_X) / CELL);
    const int by0 = max_int(0, (clip->y - BOARD_Y) / CELL);
    const int by1 = min_int(TETRIS_ROWS - 1, (clip->y + clip->h - 1 - BOARD_Y) / CELL);

    // 板底:先把 clip 与棋盘的相交部分铺满,保证即使没有格子落在这一区域也是干净的。
    const int px0 = max_int(clip->x, BOARD_X);
    const int py0 = max_int(clip->y, BOARD_Y);
    const int px1 = min_int(clip->x + clip->w, BOARD_X + BOARD_W);
    const int py1 = min_int(clip->y + clip->h, BOARD_Y + BOARD_H);
    if (px1 > px0 && py1 > py0) {
        chick_fill_rect(&s_canvas, px0, py0, px1 - px0, py1 - py0,
                        chick_rgb565(COLOR_BOARD_BG));
    }

    const uint8_t ghost_piece = (uint8_t)(s_model.active ? s_model.piece + 1 : 0);
    const int gy = tetris_ghost_y(&s_model);
    const uint8_t flash = tetris_clear_flash(&s_model);

    for (int by = by0; by <= by1; by++) {
        for (int bx = bx0; bx <= bx1; bx++) {
            const int x = BOARD_X + bx * CELL;
            const int y = BOARD_Y + by * CELL;
            const uint8_t value = tetris_cell_at(&s_model, bx, by);
            const bool ghost = value == 0 && s_model.active && by == gy &&
                               tetris_piece_cell(s_model.piece, s_model.rotation,
                                                 bx - s_model.x, by - s_model.y);
            if (ghost) {
                draw_ghost(x, y, CELL, ghost_piece);
            } else if (value != 0) {
                draw_block(x, y, CELL, value);
            } else {
                // 空格子画一格细网格:给棋盘一个"格子"的体感,也让落点更好判断。
                chick_fill_rect(&s_canvas, x, y, CELL, 1, chick_rgb565(COLOR_GRID));
                chick_fill_rect(&s_canvas, x, y, 1, CELL, chick_rgb565(COLOR_GRID));
            }
            // 消行闪烁:整行压一层白色脉冲(强度由模型给的三角波决定)。
            if (flash > 0 && tetris_row_clearing(&s_model, by)) {
                chick_fill_rect_blend(&s_canvas, x, y, CELL, CELL, 0xFFFF, flash);
            }
        }
    }

    // 外框:2 像素描边,把棋盘框成一个实体。
    chick_stroke_rect(&s_canvas, BOARD_X - 2, BOARD_Y - 2, BOARD_W + 4, BOARD_H + 4, 2,
                      chick_rgb565(COLOR_PANEL_LINE));
}

// --- 动效:硬降轨迹 ---------------------------------------------------------
// 一个短命叠加层。它不进快照对比(那不是棋盘状态),而是自己登记脏区:硬降时画一次,
// 130ms 后登记一次脏区把它擦掉 —— 不需要每帧重画,那道线最高能盖 15k 像素,
// 每帧重画会白白吃掉 10ms 推屏时间。
static rect_t trail_rect(void) {
    const int x0 = BOARD_X + (s_trail.x + s_trail.min_dx) * CELL;
    const int x1 = BOARD_X + (s_trail.x + s_trail.max_dx + 1) * CELL;
    const int y0 = BOARD_Y + (s_trail.from_y + s_trail.min_dy) * CELL;
    const int y1 = BOARD_Y + (s_trail.land_y + s_trail.max_dy + 1) * CELL;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

static void trail_start(int8_t x, uint8_t rotation, uint8_t piece, int from_y, int land_y,
                        uint32_t now) {
    int min_dx = 4, max_dx = -1, min_dy = 4, max_dy = -1;
    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 4; dx++) {
            if (!tetris_piece_cell((tetris_piece_t)(piece - 1), rotation, dx, dy)) continue;
            if (dx < min_dx) min_dx = dx;
            if (dx > max_dx) max_dx = dx;
            if (dy < min_dy) min_dy = dy;
            if (dy > max_dy) max_dy = dy;
        }
    }
    // 已经在底上就不必画：零距离的"轨迹"只是多一次推屏。
    if (max_dx < 0 || land_y <= from_y) {
        s_trail.active = false;
        return;
    }
    s_trail = (trail_t){ .active = true, .x = x, .rotation = rotation, .piece = piece,
                         .from_y = from_y, .land_y = land_y, .min_dx = min_dx,
                         .max_dx = max_dx, .min_dy = min_dy, .max_dy = max_dy,
                         .until_ms = now + TRAIL_MS };
    damage_add(trail_rect());
}

// 速度线:每 3 像素一道横线,而不是整块矩形 —— 整块会把落点糊住,
// 而且方块本身只有 1~2 格宽,细线更"快"。
static void trail_paint(void) {
    if (!s_trail.active) return;
    const rect_t r = trail_rect();
    for (int y = r.y + 2; y < r.y + r.h; y += 3) {
        chick_fill_rect_blend(&s_canvas, r.x + 3, y, r.w - 6, 1, 0xFFFF, 110);
    }
}

static void trail_tick(uint32_t now) {
    if (!s_trail.active || (int32_t)(now - s_trail.until_ms) < 0) return;
    s_trail.active = false;
    damage_add(trail_rect());          // 重画这块棋盘,速度线就没了
}

// --- 侧栏 -------------------------------------------------------------------
static void paint_preview_box(const rect_t *clip, int index) {
    const int y = PREVIEW_Y + index * (PREVIEW_BOX + PREVIEW_GAP);
    const int x = PANEL_X + (PANEL_W - PREVIEW_BOX) / 2;
    if (x + PREVIEW_BOX < clip->x || x > clip->x + clip->w) return;
    if (y + PREVIEW_BOX < clip->y || y > clip->y + clip->h) return;
    chick_fill_rect(&s_canvas, x, y, PREVIEW_BOX, PREVIEW_BOX, chick_rgb565(COLOR_BOARD_BG));
    chick_stroke_rect(&s_canvas, x, y, PREVIEW_BOX, PREVIEW_BOX, 1,
                      chick_rgb565(COLOR_PANEL_LINE));

    const tetris_piece_t piece = tetris_queue_at(&s_model, index);
    // 先算这个形状的包围盒,再摆到框中央(不同形状的包围盒不一样,直接居中会偏)。
    int min_x = 4, min_y = 4, max_x = -1, max_y = -1;
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            if (!tetris_piece_cell(piece, 0, cx, cy)) continue;
            if (cx < min_x) min_x = cx;
            if (cx > max_x) max_x = cx;
            if (cy < min_y) min_y = cy;
            if (cy > max_y) max_y = cy;
        }
    }
    const int w = (max_x - min_x + 1) * PREVIEW_CELL;
    const int h = (max_y - min_y + 1) * PREVIEW_CELL;
    const int ox = x + (PREVIEW_BOX - w) / 2 - min_x * PREVIEW_CELL;
    const int oy = y + (PREVIEW_BOX - h) / 2 - min_y * PREVIEW_CELL;
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            if (!tetris_piece_cell(piece, 0, cx, cy)) continue;
            draw_block(ox + cx * PREVIEW_CELL, oy + cy * PREVIEW_CELL, PREVIEW_CELL,
                       (uint8_t)(piece + 1));
        }
    }
}

static void paint_panel(const rect_t *clip) {
    chick_fill_rect(&s_canvas, PANEL_X, PANEL_Y, PANEL_W, PANEL_H,
                    chick_rgb565(COLOR_PANEL_BG));
    chick_stroke_rect(&s_canvas, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 1,
                      chick_rgb565(COLOR_PANEL_LINE));

    const int cx = PANEL_X + PANEL_W / 2;
    char text[24];
    chick_draw_text_center(&s_canvas, cx, NEXT_LABEL_Y, FONT_CJK,
                           chick_rgb565(COLOR_INK_SOFT), TETRIS_TEXT_NEXT);
    for (int i = 0; i < TETRIS_PREVIEW; i++) paint_preview_box(clip, i);

    chick_draw_text_center(&s_canvas, cx, SCORE_LABEL_Y, FONT_CJK,
                           chick_rgb565(COLOR_INK_SOFT), TETRIS_TEXT_SCORE);
    snprintf(text, sizeof(text), TETRIS_TEXT_SCORE_FMT, (unsigned)s_model.score);
    chick_draw_text_center(&s_canvas, cx, SCORE_VALUE_Y, FONT_CJK,
                           chick_rgb565(COLOR_ACCENT), text);

    snprintf(text, sizeof(text), TETRIS_TEXT_LEVEL_FMT, (unsigned)s_model.level);
    chick_draw_text_center(&s_canvas, cx, LEVEL_Y, FONT_CJK, chick_rgb565(COLOR_INK), text);
    snprintf(text, sizeof(text), TETRIS_TEXT_LINES_FMT, (unsigned)s_model.lines);
    chick_draw_text_center(&s_canvas, cx, LINES_Y, FONT_CJK, chick_rgb565(COLOR_INK), text);

    snprintf(text, sizeof(text), "%u FPS", (unsigned)s_fps_value);
    chick_draw_text_center(&s_canvas, cx, FPS_Y, FONT_TINY,
                           chick_rgb565(COLOR_INK_SOFT), text);
}

// --- 顶栏 -------------------------------------------------------------------
// 电量用进度条表示(与小鸡快跑同一套画法:外壳 + 正极凸点 + 按百分比填充)。
// 数字百分比在深色主题里既占地方又不如"槽有多满"直观。
#define BATTERY_W        30
#define BATTERY_H        14
#define BATTERY_BORDER   2
#define BATTERY_X        (UI_W - 8 - BATTERY_W - 3)   // 3 = 正极凸点
#define BATTERY_Y        ((HEADER_H - BATTERY_H) / 2)

static void paint_battery(void) {
    chick_fill_rect(&s_canvas, BATTERY_X + BATTERY_W, BATTERY_Y + 4, 3, 5,
                    chick_rgb565(COLOR_INK));
    chick_fill_rect(&s_canvas, BATTERY_X, BATTERY_Y, BATTERY_W, BATTERY_H,
                    chick_rgb565(COLOR_BAR_BG));
    chick_stroke_rect(&s_canvas, BATTERY_X, BATTERY_Y, BATTERY_W, BATTERY_H, BATTERY_BORDER,
                      chick_rgb565(COLOR_INK));
    if (s_battery_soc > 0) {
        // 低电量换红色:进度条本身已经表达"剩多少",颜色只用来提醒"该充电了"。
        const uint32_t fill = s_battery_soc <= 20 ? COLOR_BAT_LOW : COLOR_BAT_GOOD;
        int w = ((BATTERY_W - 2 * BATTERY_BORDER) * s_battery_soc) / 100;
        if (w < 1) w = 1;
        chick_fill_rect(&s_canvas, BATTERY_X + BATTERY_BORDER, BATTERY_Y + BATTERY_BORDER, w,
                        BATTERY_H - 2 * BATTERY_BORDER, chick_rgb565(fill));
    } else if (!s_battery_present) {
        // 本板没有电压采集:整条删除而不是画空槽,免得用户以为电池没电了。
        chick_fill_rect(&s_canvas, BATTERY_X - 1, BATTERY_Y - 1, BATTERY_W + 5, BATTERY_H + 2,
                        chick_rgb565(COLOR_BG_TOP));
    }
}

static void paint_header(void) {
    chick_draw_text(&s_canvas, BOARD_X, 4, FONT_CJK, chick_rgb565(COLOR_INK),
                    TETRIS_TEXT_TITLE);
    paint_battery();
}

// --- 提示条(升级 / 四行消除)----------------------------------------------
static void paint_toast(void) {
    if (s_toast == NULL) return;
    const rect_t box = rect_make(BOARD_X + 10, BOARD_Y + 92, BOARD_W - 20, 34);
    chick_fill_rect(&s_canvas, box.x, box.y, box.w, box.h, chick_rgb565(COLOR_TOAST_BG));
    chick_stroke_rect(&s_canvas, box.x, box.y, box.w, box.h, 2, chick_rgb565(COLOR_INK));
    chick_draw_text_center(&s_canvas, box.x + box.w / 2, box.y + 7, FONT_CJK,
                           chick_rgb565(COLOR_TOAST_INK), s_toast);
}

// --- 弹层(暂停 / 结束)---------------------------------------------------
// 一行内容 = 文案 + 字体 + 颜色;弹层尺寸由这些行反推(见 paint_overlay)。
typedef struct {
    const char *text;
    const chick_font_t *font;
    uint32_t color;
} row_t;

static void paint_overlay(void) {
    const tetris_state_t st = s_model.state;
    if (st != TETRIS_STATE_PAUSED && st != TETRIS_STATE_OVER) return;
    const bool over = st == TETRIS_STATE_OVER;

    // 只压暗棋盘:侧栏的分数/等级仍然可读 —— 结算时正要看这些数字。
    const rect_t board = rect_make(BOARD_X - 2, BOARD_Y - 2, BOARD_W + 4, BOARD_H + 4);
    chick_fill_rect_blend(&s_canvas, board.x, board.y, board.w, board.h,
                          chick_rgb565(COLOR_OVERLAY), 205);

    // 先把内容排好,再按内容反推弹层尺寸。尺寸写死过一次,结果暂停页的
    // "B 回首页"被画到了框外(框高 76 装不下三行正文):
    //   宽 = 最宽一行实测宽度 + 左右边距(封顶到棋盘内),
    //   高 = 各行行高之和 + 行间距 + 上下边距。
    char score_text[16];
    snprintf(score_text, sizeof(score_text), TETRIS_TEXT_SCORE_FMT, (unsigned)s_model.score);
    row_t rows[5];
    int row_count = 0;
    if (over) {
        rows[row_count++] = (row_t){ TETRIS_TEXT_GAME_OVER, FONT_CJK, COLOR_DANGER };
        rows[row_count++] = (row_t){ s_model.record ? TETRIS_TEXT_RECORD : TETRIS_TEXT_THIS_RUN,
                                     FONT_CJK, COLOR_INK_SOFT };
        rows[row_count++] = (row_t){ score_text, FONT_BIG, COLOR_ACCENT };
        rows[row_count++] = (row_t){ TETRIS_TEXT_AGAIN_HINT, FONT_CJK, COLOR_INK };
        rows[row_count++] = (row_t){ TETRIS_TEXT_BACK_HINT, FONT_CJK, COLOR_INK_SOFT };
    } else {
        rows[row_count++] = (row_t){ TETRIS_TEXT_PAUSE, FONT_CJK, COLOR_INK };
        rows[row_count++] = (row_t){ TETRIS_TEXT_RESUME_HINT, FONT_CJK, COLOR_ACCENT };
        rows[row_count++] = (row_t){ TETRIS_TEXT_BACK_HINT, FONT_CJK, COLOR_INK_SOFT };
    }

    const int pad_x = 10, pad_y = 8, gap = 6;
    int content_h = 0, content_w = 0;
    for (int i = 0; i < row_count; i++) {
        content_h += (int)rows[i].font->line_height + (i > 0 ? gap : 0);
        const int w = chick_text_measure(rows[i].font, rows[i].text).width;
        if (w > content_w) content_w = w;
    }
    int box_w = content_w + 2 * pad_x;
    if (box_w > BOARD_W - 8) box_w = BOARD_W - 8;
    if (box_w < 96) box_w = 96;                  // 太窄会像一条缝,不像弹层
    const int box_h = content_h + 2 * pad_y;
    const rect_t box = rect_make(BOARD_X + (BOARD_W - box_w) / 2,
                                 BOARD_Y + (BOARD_H - box_h) / 2, box_w, box_h);
    chick_fill_rect(&s_canvas, box.x, box.y, box.w, box.h, chick_rgb565(COLOR_PANEL_BG));
    chick_stroke_rect(&s_canvas, box.x, box.y, box.w, box.h, 2,
                      chick_rgb565(over ? COLOR_DANGER : COLOR_BAR_FILL));

    const int cx = box.x + box.w / 2;
    int y = box.y + pad_y;
    for (int i = 0; i < row_count; i++) {
        chick_draw_text_center(&s_canvas, cx, y, rows[i].font,
                               chick_rgb565(rows[i].color), rows[i].text);
        y += (int)rows[i].font->line_height + gap;
    }
}

// --- 首页 -------------------------------------------------------------------
// logo:一个大号 T 方块(26 像素格)。比只放一行字更像"游戏",也不用额外素材。
static void paint_logo(void) {
    const int size = 26;
    static const int cells[4][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 1, 1 } };
    const int x0 = (UI_W - 3 * size) / 2;
    const int y0 = 34;
    for (int i = 0; i < 4; i++) {
        draw_block(x0 + cells[i][0] * size, y0 + cells[i][1] * size, size,
                   (uint8_t)(TETRIS_PIECE_T + 1));
    }
}

static void paint_title(void) {
    paint_logo();
    chick_draw_text_center(&s_canvas, UI_W / 2, 104, FONT_BIG, chick_rgb565(COLOR_INK),
                           TETRIS_TEXT_TITLE);
    // "按 A 开始"闪烁:静止的提示容易被当成"卡住了"。
    if (s_blink_on) {
        chick_draw_text_center(&s_canvas, UI_W / 2, 158, FONT_CJK,
                               chick_rgb565(COLOR_ACCENT), TETRIS_TEXT_START_HINT);
    }
    chick_draw_text_center(&s_canvas, UI_W / 2, 196, FONT_CJK, chick_rgb565(COLOR_INK_SOFT),
                           TETRIS_TEXT_CONTROLS);
    char text[32];
    snprintf(text, sizeof(text), TETRIS_TEXT_BEST, (unsigned)s_best);
    chick_draw_text_center(&s_canvas, UI_W / 2, 246, FONT_CJK, chick_rgb565(COLOR_INK), text);
    chick_draw_text_center(&s_canvas, UI_W / 2, 278, FONT_CJK, chick_rgb565(COLOR_INK_SOFT),
                           TETRIS_TEXT_SETTINGS_HINT);
}

// --- 设置页 -----------------------------------------------------------------
static uint8_t settings_value_of(uint8_t row) {
    switch (row) {
        case 1: return s_settings.volume;
        case 2: return s_settings.warmth;
        default: return s_settings.brightness;
    }
}

static void paint_settings_row(uint8_t row, const char *label, int y) {
    const bool active = row == s_settings_row;
    const uint16_t ink = chick_rgb565(active ? COLOR_ACCENT : COLOR_INK);
    chick_draw_text(&s_canvas, 28, y, FONT_CJK, ink, label);

    char text[16];
    snprintf(text, sizeof(text), TETRIS_TEXT_PERCENT_FMT, (unsigned)settings_value_of(row));
    chick_draw_text_right(&s_canvas, UI_W - 28, y, FONT_CJK, ink, text);

    const int bar_x = 28, bar_w = UI_W - 56, bar_h = 12, bar_y = y + 26;
    chick_fill_rect(&s_canvas, bar_x, bar_y, bar_w, bar_h, chick_rgb565(COLOR_BAR_BG));
    chick_stroke_rect(&s_canvas, bar_x, bar_y, bar_w, bar_h, 1, ink);
    const uint8_t value = settings_value_of(row);
    int fill = (bar_w - 2) * value / 100;
    if (value > 0 && fill < 1) fill = 1;
    chick_fill_rect(&s_canvas, bar_x + 1, bar_y + 1, fill, bar_h - 2,
                    chick_rgb565(active ? COLOR_ACCENT : COLOR_BAR_FILL));
}

static void paint_settings(void) {
    chick_draw_text_center(&s_canvas, UI_W / 2, 20, FONT_BIG, chick_rgb565(COLOR_INK),
                           TETRIS_TEXT_SETTINGS);
    paint_settings_row(0, TETRIS_TEXT_SET_BRIGHT, 92);
    paint_settings_row(1, TETRIS_TEXT_SET_VOLUME, 152);
    paint_settings_row(2, TETRIS_TEXT_SET_WARMTH, 212);
    chick_draw_text_center(&s_canvas, UI_W / 2, 274, FONT_CJK, chick_rgb565(COLOR_INK_SOFT),
                           TETRIS_TEXT_SETTINGS_HELP);
}

// --- 页面合成 ---------------------------------------------------------------
// 把一个矩形区域按当前页面完整重画一遍。脏区推屏前必须保证这块区域是"当帧内容",
// 所以这里不做局部增量:重画范围通常只有 1~4k 像素,整块重画最不容易出错。
static void paint_all(const rect_t *clip) {
    paint_bg(clip);
    if (s_page == PAGE_TITLE) {
        paint_header();
        paint_title();
        return;
    }
    if (s_page == PAGE_SETTINGS) {
        paint_header();
        paint_settings();
        return;
    }
    paint_header();
    paint_board(clip);
    trail_paint();
    paint_panel(clip);
    paint_toast();
    paint_overlay();
}

// --- 推屏 -------------------------------------------------------------------
static bool IRAM_ATTR on_flush_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata, void *ctx) {
    (void)io; (void)edata; (void)ctx;
    BaseType_t need_yield = pdFALSE;
    if (s_flush_done != NULL) xSemaphoreGiveFromISR(s_flush_done, &need_yield);
    return need_yield == pdTRUE;
}

static void flush_rect(const rect_t *r) {
    if (rect_empty(r)) return;
    const uint32_t t0 = now_us();
    for (int row = 0; row < r->h; row += STAGE_ROWS) {
        const int rows = (r->h - row) > STAGE_ROWS ? STAGE_ROWS : (r->h - row);
        uint16_t *dst = s_stage;
        // ST7789 走 SPI 要【大端】RGB565,画布是小端 -> 拷贝时交换高低字节。
        for (int rr = 0; rr < rows; rr++) {
            const uint16_t *src = s_fb + (size_t)(r->y + row + rr) * UI_W + r->x;
            for (int i = 0; i < r->w; i++) {
                uint16_t px = src[i];
                if (s_tint_ready) px = chick_color_apply_lut(px, &s_tint_lut);
                dst[i] = (uint16_t)((px >> 8) | (px << 8));
            }
            dst += r->w;
        }
        esp_lcd_panel_draw_bitmap(bsp_display_panel(), r->x, r->y + row, r->x + r->w,
                                  r->y + row + rows, s_stage);
        (void)xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
        s_flush_calls++;
    }
    s_flush_px += (uint32_t)r->w * (uint32_t)r->h;
    s_flush_us += now_us() - t0;
}

static void damage_flush(void) {
    for (int i = 0; i < s_damage_n; i++) {
        paint_all(&s_damage[i]);
        flush_rect(&s_damage[i]);
    }
    s_damage_n = 0;
}

// 圆角屏(C3 Passport)才需要把圆角外涂黑;直角屏(S3R8)半径 0,直接跳过。
static void mask_corners(void) {
    const int radius = BSP_LVGL_SCREEN_RADIUS;
    if (radius <= 0) return;
    for (int y = 0; y < UI_H; y++) {
        int32_t x1 = 0, x2 = UI_W - 1;
        if (!bsp_display_rounded_row_span(y, UI_W, UI_H, radius, &x1, &x2)) continue;
        if (x1 == 0 && x2 == UI_W - 1) continue;
        for (int x = 0; x < x1; x++) s_fb[(size_t)y * UI_W + x] = 0;
        for (int x = x2 + 1; x < UI_W; x++) s_fb[(size_t)y * UI_W + x] = 0;
    }
}

// --- 脏区对比 ---------------------------------------------------------------
static void snapshot_take(snapshot_t *s) {
    memcpy(s->board, s_model.board, sizeof(s->board));
    s->piece = (uint8_t)s_model.piece;
    s->rotation = s_model.rotation;
    s->x = s_model.x;
    s->y = s_model.y;
    s->active = s_model.active;
    s->ghost_y = s_model.ghost_y;
    s->clear_rows = s_model.clear_rows;
    s->clear_flash = tetris_clear_flash(&s_model);
    s->score = s_model.score;
    s->lines = s_model.lines;
    s->level = s_model.level;
    for (int i = 0; i < TETRIS_PREVIEW; i++) s->queue[i] = (uint8_t)tetris_queue_at(&s_model, i);
    s->state = s_model.state;
    s->page = s_page;
    s->settings_row = s_settings_row;
    s->settings_values[0] = s_settings.brightness;
    s->settings_values[1] = s_settings.volume;
    s->settings_values[2] = s_settings.warmth;
    s->blink = s_blink_on;
    s->toast = s_toast;
    s->battery_soc = s_battery_soc;
    s->fps = s_fps_value;
}

// 与上一帧快照对比 -> 脏区。任何"会改变画面"的状态都必须出现在快照里,否则漏推。
static void damage_from_diff(const snapshot_t *prev) {
    const snapshot_t *now = &s_prev;

    if (now->page != prev->page || now->state != prev->state) {
        // 换页 / 弹层出现:整屏重画一次(这些时刻本来就不需要高帧率)。
        damage_add_screen();
        if (now->page != prev->page) return;
    }

    if (now->page == PAGE_SETTINGS) {
        if (now->settings_row != prev->settings_row) damage_add(rect_make(0, 70, UI_W, 190));
        for (int i = 0; i < 3; i++) {
            if (now->settings_values[i] != prev->settings_values[i]) {
                damage_add(rect_make(0, 88 + i * 60, UI_W, 48));
            }
        }
        return;
    }
    if (now->page == PAGE_TITLE) {
        if (now->blink != prev->blink) damage_add(rect_make(0, 154, UI_W, 26));
        return;
    }

    // --- 游戏页 ---
    // 棋盘:逐行 memcmp 快速跳过,变化的格子按格登记(再自动合并成少量矩形)。
    for (int y = 0; y < TETRIS_ROWS; y++) {
        if (memcmp(now->board[y], prev->board[y], TETRIS_COLS) == 0) continue;
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (now->board[y][x] != prev->board[y][x]) damage_add(cell_rect(x, y));
        }
    }
    // 当前方块与幽灵:旧位置和新位置都要重画,否则会留下残影。
    const bool piece_changed = now->piece != prev->piece || now->rotation != prev->rotation ||
                               now->x != prev->x || now->y != prev->y ||
                               now->active != prev->active || now->ghost_y != prev->ghost_y;
    if (piece_changed) {
        tetris_model_t old = s_model;
        old.piece = (tetris_piece_t)prev->piece;
        old.rotation = prev->rotation;
        old.x = prev->x;
        old.y = prev->y;
        old.active = prev->active;
        if (prev->active) {
            damage_add(piece_rect(&old, prev->x, prev->y));
            damage_add(piece_rect(&old, prev->x, prev->ghost_y));
        }
        if (now->active) {
            damage_add(piece_rect(&s_model, now->x, now->y));
            damage_add(piece_rect(&s_model, now->x, now->ghost_y));
        }
    }
    // 消行闪烁:闪烁期间整行每帧都在变。
    if (now->clear_flash != prev->clear_flash || now->clear_rows != prev->clear_rows) {
        for (int y = 0; y < TETRIS_ROWS; y++) {
            if (now->clear_rows & ((uint32_t)1u << y)) {
                damage_add(rect_make(BOARD_X, BOARD_Y + y * CELL, BOARD_W, CELL));
            }
        }
    }
    // 侧栏:每项各自一小块,避免改个分数就重推整个侧栏。
    if (now->score != prev->score) damage_add(rect_make(PANEL_X, SCORE_VALUE_Y - 2, PANEL_W, 24));
    if (now->level != prev->level) damage_add(rect_make(PANEL_X, LEVEL_Y - 2, PANEL_W, 24));
    if (now->lines != prev->lines) damage_add(rect_make(PANEL_X, LINES_Y - 2, PANEL_W, 24));
    for (int i = 0; i < TETRIS_PREVIEW; i++) {
        if (now->queue[i] != prev->queue[i]) {
            damage_add(rect_make(PANEL_X, PREVIEW_Y + i * (PREVIEW_BOX + PREVIEW_GAP),
                                 PANEL_W, PREVIEW_BOX));
        }
    }
    if (now->fps != prev->fps) damage_add(rect_make(PANEL_X, FPS_Y - 2, PANEL_W, 18));
    if (now->toast != prev->toast) damage_add_screen();
}

// --- 音效与事件 -------------------------------------------------------------
static void toast_show(const char *text, uint32_t now) {
    s_toast = text;
    s_toast_until_ms = now + TOAST_MS;
}

static void sound(const char *song) {
    if (song != NULL && rtttl_player_play(song) != ESP_OK) ESP_LOGD(TAG, "音频忙,跳过音效");
}

// 模型事件 -> 声音与提示。注意顺序:硬降已经包含了"落子",所以只播硬降音,
// 不再叠一个锁定音(两声音效叠在一起会变成噪音)。
static void model_events_handle(const tetris_events_t *ev, uint32_t now) {
    if (ev->dropped) sound(TETRIS_SFX_HARDDROP);
    else if (ev->locked) sound(TETRIS_SFX_LOCK);
    if (ev->rotated) sound(TETRIS_SFX_ROTATE);
    else if (ev->moved) sound(TETRIS_SFX_MOVE);

    if (ev->clear_begin) {
        const char *song = TETRIS_SFX_CLEAR1;
        if (s_model.clear_count == 2) song = TETRIS_SFX_CLEAR2;
        else if (s_model.clear_count == 3) song = TETRIS_SFX_CLEAR3;
        else if (s_model.clear_count >= 4) song = TETRIS_SFX_CLEAR4;
        sound(song);
        if (s_model.clear_count >= 4) toast_show(TETRIS_TEXT_TETRIS, now);
    }
    if (ev->level_up) {
        sound(TETRIS_SFX_LEVELUP);
        toast_show(TETRIS_TEXT_LEVEL_UP, now);
    }
    if (ev->game_over) {
        sound(TETRIS_SFX_GAMEOVER);
        rtttl_player_set_bgm(NULL);
        if (s_model.score > s_best) {
            s_best = s_model.score;
            if (s_best > s_best_saved) {
                s_best_saved = s_best;
                (void)tetris_store_save_best(s_best);
            }
        }
    }
}

// --- 页面切换 ---------------------------------------------------------------
static void game_start(uint32_t now) {
    tetris_begin(&s_model, (uint32_t)esp_random());
    tetris_set_best(&s_model, s_best);   // 模型只读它来判断"新纪录"
    s_page = PAGE_GAME;
    s_last_input_ms = now;
    s_toast = NULL;
    s_trail.active = false;              // 上一局的轨迹不能带进新一局
    rtttl_player_set_bgm(TETRIS_BGM_THEME);
    sound(TETRIS_SFX_START);
}

static void back_to_title(uint32_t now) {
    s_page = PAGE_TITLE;
    s_toast = NULL;
    s_last_input_ms = now;
    rtttl_player_set_bgm(NULL);
}

static void settings_open(uint32_t now) {
    s_page = PAGE_SETTINGS;
    s_settings_row = 0;
    s_last_input_ms = now;
}

static void settings_apply(void) {
    if (s_backlight_on) bsp_display_backlight(s_settings.brightness);
    bsp_audio_set_volume(s_settings.volume);
    tint_rebuild();
}

static void settings_close(uint32_t now) {
    settings_apply();
    (void)chick_run_store_save_settings(&s_settings);   // 离开设置页时落盘一次
    back_to_title(now);
}

static void settings_step(int32_t steps) {
    switch (s_settings_row) {
        case 1:
            s_settings.volume = chick_run_settings_step(s_settings.volume, steps,
                                                        CHICK_RUN_VOLUME_MIN,
                                                        CHICK_RUN_VOLUME_MAX);
            bsp_audio_set_volume(s_settings.volume);
            break;
        case 2:
            s_settings.warmth = chick_run_settings_step(s_settings.warmth, steps,
                                                        CHICK_RUN_WARMTH_MIN,
                                                        CHICK_RUN_WARMTH_MAX);
            tint_rebuild();
            break;
        default:
            s_settings.brightness = chick_run_settings_step(s_settings.brightness, steps,
                                                            CHICK_RUN_BRIGHTNESS_MIN,
                                                            CHICK_RUN_BRIGHTNESS_MAX);
            if (s_backlight_on) bsp_display_backlight(s_settings.brightness);
            break;
    }
}

// --- 输入 -------------------------------------------------------------------
// 左右键的自动重复:按下走一次,按住后按 DAS/ARR 继续(方块游戏没有自动重复
// 就没法把方块挪到边上)。
static void repeat_track(bsp_btn_t btn, bool held, uint32_t now) {
    if ((int)btn >= BSP_BTN_COUNT) return;
    if (held) {
        s_repeat[btn].held = true;
        s_repeat[btn].next_ms = now + DAS_MS;
    } else {
        s_repeat[btn].held = false;
    }
}

static void repeat_tick(uint32_t now) {
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        if (!s_repeat[i].held) continue;
        if ((int32_t)(now - s_repeat[i].next_ms) < 0) continue;
        s_repeat[i].next_ms = now + ARR_MS;
        const bsp_btn_t btn = (bsp_btn_t)i;
        if (s_page == PAGE_SETTINGS) {
            if (btn == BSP_BTN_LEFT) settings_step(-1);
            else if (btn == BSP_BTN_RIGHT) settings_step(1);
            continue;
        }
        if (s_page != PAGE_GAME || s_model.state != TETRIS_STATE_PLAYING) continue;
        tetris_events_t ev;
        if (btn == BSP_BTN_LEFT) tetris_move(&s_model, -1, &ev);
        else if (btn == BSP_BTN_RIGHT) tetris_move(&s_model, 1, &ev);
        // 自动重复刻意不发音:每秒十几下的"嘀嘀"比操作本身还吵。
    }
}

static void handle_key(const key_event_t *key, uint32_t now) {
    const bsp_btn_t btn = key->btn;
    const bsp_btn_ev_t ev = key->ev;
    s_last_input_ms = now;

    if (s_page == PAGE_TITLE) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_A) {
            game_start(now);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_B) {
            settings_open(now);
        } else if (ev == BSP_BTN_LONG &&
                   (btn == BSP_BTN_B || btn == BSP_BTN_A)) {
            settings_open(now);      // 长按也进设置:首页只有两个入口,少一层菜单
        }
        return;
    }

    if (s_page == PAGE_SETTINGS) {
        if (ev == BSP_BTN_PRESS && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            const int delta = btn == BSP_BTN_UP ? 2 : 1;     // +2 再取模 = -1
            s_settings_row = (uint8_t)((s_settings_row + delta) % 3);
        } else if (btn == BSP_BTN_LEFT || btn == BSP_BTN_RIGHT) {
            const int32_t steps = btn == BSP_BTN_RIGHT ? 1 : -1;
            if (ev == BSP_BTN_PRESS) {
                settings_step(steps);
                repeat_track(btn, true, now);
            } else if (ev == BSP_BTN_RELEASE) {
                repeat_track(btn, false, now);
            }
        } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_A || btn == BSP_BTN_B)) {
            settings_close(now);
        }
        return;
    }

    // --- 游戏页 ---
    if (s_model.state == TETRIS_STATE_PAUSED) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_A) {
            (void)tetris_toggle_pause(&s_model);
            rtttl_player_set_bgm(TETRIS_BGM_THEME);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_B) {
            back_to_title(now);
        }
        return;
    }
    if (s_model.state == TETRIS_STATE_OVER) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_A) game_start(now);
        else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_B) back_to_title(now);
        return;
    }
    // CLEARING 期间不接受操作(模型自己会忽略),但可以暂停页面外的返回键。
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_B) {
        (void)tetris_toggle_pause(&s_model);
        if (s_model.state == TETRIS_STATE_PAUSED) {
            sound(TETRIS_SFX_PAUSE);
            rtttl_player_set_bgm(NULL);
        } else {
            rtttl_player_set_bgm(TETRIS_BGM_THEME);
        }
        return;
    }
    if (s_model.state != TETRIS_STATE_PLAYING) return;

    tetris_events_t model_ev;
    if ((btn == BSP_BTN_LEFT || btn == BSP_BTN_RIGHT)) {
        if (ev == BSP_BTN_PRESS) {
            tetris_move(&s_model, btn == BSP_BTN_LEFT ? -1 : 1, &model_ev);
            model_events_handle(&model_ev, now);
            repeat_track(btn, true, now);
        } else if (ev == BSP_BTN_RELEASE) {
            repeat_track(btn, false, now);
        }
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        // 按下加速、松手恢复:用 RELEASE 而不是 CLICK —— 长按超过阈值后只会收到
        // LONG 和 RELEASE,只认 CLICK 的话按住加速会在半秒后自己停掉。
        if (ev == BSP_BTN_PRESS) {
            tetris_set_soft_drop(&s_model, true);
            sound(TETRIS_SFX_SOFTDROP);
        } else if (ev == BSP_BTN_RELEASE) {
            tetris_set_soft_drop(&s_model, false);
        }
        return;
    }
    // 旋转与硬降用 PRESS(按下即响应):这两个是"手速"操作,等松手再触发会让
    // 快速连续旋转明显发木。菜单类操作(A 开始/继续/重开)仍然用 CLICK。
    if (ev != BSP_BTN_PRESS) return;
    if (btn == BSP_BTN_UP) {
        // 硬降:先记下落点与形状 —— 模型会立刻把方块合并进棋盘并换上新方块,
        // 原位置、落点、形状都取不到了。速度线要用它们算。
        const int from_y = s_model.y;
        const int land_y = tetris_ghost_y(&s_model);
        const int8_t tx = s_model.x;
        const uint8_t rot = s_model.rotation;
        const uint8_t piece = (uint8_t)(s_model.piece + 1);
        tetris_hard_drop(&s_model, &model_ev);
        trail_start(tx, rot, piece, from_y, land_y, now);
        model_events_handle(&model_ev, now);
    } else if (btn == BSP_BTN_A) {
        tetris_rotate(&s_model, 1, &model_ev);
        model_events_handle(&model_ev, now);
    }
}

// --- 帧推进 -----------------------------------------------------------------
static void update(uint32_t now, uint32_t dt) {
    if (s_toast != NULL && (int32_t)(now - s_toast_until_ms) >= 0) s_toast = NULL;

    s_blink_on = ((now / BLINK_MS) & 1u) == 0;

    // 电量(可选硬件:只有接了电池分压/电量计的板子才有读数)
    if (s_battery_present && (int32_t)(now - s_battery_next_ms) >= 0) {
        s_battery_next_ms = now + BATTERY_MS;
        const int soc = bsp_battery_soc();
        if (soc >= 0 && soc <= 100) s_battery_soc = soc;
    }

    // 游戏中一分钟没操作 -> 自动暂停(护屏 + 省电),避免棋盘长期静止留在屏上
    if (s_page == PAGE_GAME && s_model.state == TETRIS_STATE_PLAYING &&
        (int32_t)(now - s_last_input_ms) >= (int32_t)IDLE_PAUSE_MS) {
        (void)tetris_toggle_pause(&s_model);
        rtttl_player_set_bgm(NULL);
        ESP_LOGI(TAG, "无操作 %u 秒,自动暂停", (unsigned)(IDLE_PAUSE_MS / 1000));
    }

    repeat_tick(now);
    trail_tick(now);

    if (s_page == PAGE_GAME && s_model.state != TETRIS_STATE_PAUSED) {
        tetris_events_t ev;
        tetris_tick(&s_model, dt, &ev);
        model_events_handle(&ev, now);
    }
}

static void stats_log(uint32_t now) {
    if (s_stats_next_ms != 0 && (int32_t)(now - s_stats_next_ms) < 0) return;
    const uint32_t frames = s_frames ? s_frames : 1u;
    ESP_LOGI(TAG, "帧统计 %u 帧 %.1f FPS | 推送 %u px/帧 %u us/帧 %u 次/帧 | 分数 %u 行 %u 等级 %u | 堆 %u",
             (unsigned)s_frames, (double)s_frames * 1000.0 / (double)STATS_MS,
             (unsigned)(s_flush_px / frames), (unsigned)(s_flush_us / frames),
             (unsigned)(s_flush_calls / frames), (unsigned)s_model.score,
             (unsigned)s_model.lines, (unsigned)s_model.level,
             (unsigned)esp_get_free_heap_size());
    s_frames = 0;
    s_flush_px = 0;
    s_flush_us = 0;
    s_flush_calls = 0;
    s_stats_next_ms = now + STATS_MS;
}

// --- 字形覆盖自检 -----------------------------------------------------------
static void font_check_coverage(void) {
    static const char *const inventory[] = { TETRIS_TEXT_INVENTORY };
    const chick_font_t *const fonts[] = { FONT_CJK, FONT_TINY, FONT_BIG };
    unsigned missing = 0;
    for (size_t f = 0; f < sizeof(fonts) / sizeof(fonts[0]); f++) {
        for (size_t s = 0; s < sizeof(inventory) / sizeof(inventory[0]); s++) {
            const char *text = inventory[s];
            while (*text) {
                const uint8_t *p = (const uint8_t *)text;
                uint32_t code = 0;
                if (*p < 0x80) { code = *p; p += 1; }
                else if ((*p & 0xE0) == 0xC0) {
                    code = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
                    p += 2;
                } else {
                    code = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                           (uint32_t)(p[2] & 0x3F);
                    p += 3;
                }
                text = (const char *)p;
                if (code == '\n') continue;
                if (chick_glyph(fonts[f], code) == NULL) {
                    ESP_LOGE(TAG, "字体缺字:U+%04X 字体%u", (unsigned)code, (unsigned)f);
                    missing++;
                }
            }
        }
    }
    if (missing == 0) ESP_LOGI(TAG, "字形覆盖检查通过");
    else ESP_LOGE(TAG, "缺字 %u 处", missing);
}

// --- 渲染任务 ---------------------------------------------------------------
static void frame_task(void *arg) {
    (void)arg;
    uint32_t last_ms = now_ms();
    uint32_t next_frame_ms = last_ms;
    while (s_running) {
        key_event_t key;
        while (s_keys != NULL && xQueueReceive(s_keys, &key, 0) == pdTRUE) {
            handle_key(&key, now_ms());
        }

        const uint32_t time_ms = now_ms();
        uint32_t dt = time_ms - last_ms;
        last_ms = time_ms;
        if (dt > UI_MAX_DT_MS) dt = UI_MAX_DT_MS;
        update(time_ms, dt);

        const snapshot_t prev = s_prev;
        const bool had_prev = s_prev_valid;
        snapshot_take(&s_prev);
        if (had_prev) damage_from_diff(&prev);
        else damage_add_screen();

        mask_corners();
        damage_flush();

        if (!s_first_frame_pushed) {
            s_first_frame_pushed = true;
            s_backlight_on = true;
            bsp_display_backlight(s_settings.brightness);
            ESP_LOGI(TAG, "首帧已推送,开屏(面板就绪到首帧全程黑屏,避免开机花屏)");
        }

        s_frames++;
        s_fps_count++;
        if ((int32_t)(time_ms - s_fps_last_ms) >= 1000) {
            s_fps_value = s_fps_count * 1000u / (uint32_t)(time_ms - s_fps_last_ms);
            s_fps_count = 0;
            s_fps_last_ms = time_ms;
        }
        s_prev_valid = true;
        stats_log(time_ms);

        next_frame_ms += UI_FRAME_MS;
        const uint32_t now = now_ms();
        const int32_t wait = (int32_t)(next_frame_ms - now);
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)wait));
        else next_frame_ms = now;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// --- 对外接口 ---------------------------------------------------------------
esp_err_t tetris_ui_start(void) {
    if (s_running) return ESP_OK;

    s_keys = xQueueCreate(12, sizeof(key_event_t));
    if (s_keys == NULL) return ESP_ERR_NO_MEM;

    // 画布:内部 RAM(推送时会先按行拷进 staging 缓冲,画布本身不必是 DMA 内存;
    // 加上 MALLOC_CAP_DMA 反而会让 150KB 的可选块变小,真机踩过)。
    const size_t canvas_bytes = (size_t)UI_W * UI_H * 2;
    s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_DEFAULT);
    if (s_fb == NULL) s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_fb == NULL) s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_INTERNAL);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "画布分配失败(需要 %u 字节),剩余堆 %u",
                 (unsigned)canvas_bytes, (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    s_stage = heap_caps_malloc((size_t)UI_W * STAGE_ROWS * 2,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_stage == NULL) {
        ESP_LOGE(TAG, "推送缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    s_flush_done = xSemaphoreCreateCounting(1, 0);
    if (s_flush_done == NULL) return ESP_ERR_NO_MEM;
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_flush_done };
    (void)esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &cbs, NULL);

    s_canvas = (chick_canvas_t){ s_fb, UI_W, UI_W, UI_H };
    palette_init();
    bg_prepare();

    // 设置与小鸡快跑共用 NVS 命名空间:同一台设备同一个用户,亮度/音量/色温
    // 不应该在两个游戏之间各记一份。
    chick_run_settings_defaults(&s_settings);
    if (chick_run_store_init() == ESP_OK) {
        (void)chick_run_store_load_settings(&s_settings);
    }
    chick_run_settings_sanitize(&s_settings);
    tint_rebuild();

    if (tetris_store_init() == ESP_OK) {
        s_best = tetris_store_load_best();
        s_best_saved = s_best;
    }

    if (bsp_audio_init() == ESP_OK) {
        if (rtttl_player_start() != ESP_OK) ESP_LOGW(TAG, "音频任务创建失败,静音运行");
    } else {
        ESP_LOGW(TAG, "音频初始化失败,静音运行");
    }
    bsp_audio_set_volume(s_settings.volume);

    // 电量是可选的:接了就显示百分比,没接就不画(界面不留空槽)。
    s_battery_present = (bsp_battery_init() == ESP_OK);
    s_battery_next_ms = 0;
    ESP_LOGI(TAG, "电量%s", s_battery_present ? "可用" : "不可用(本板无分压/电量计)");

    tetris_begin(&s_model, (uint32_t)esp_random());
    tetris_set_best(&s_model, s_best);
    s_page = PAGE_TITLE;         // 开机直接进首页
    s_settings_row = 0;
    s_last_input_ms = now_ms();
    s_fps_last_ms = s_last_input_ms;
    // 背光先不开:等首帧推完再亮,避免开机那几百毫秒闪一段花屏。
    // ★ 这几行状态必须在 start 时重置:stop() 之后静态变量仍保留上次的值,
    //   而本应用可能被重新 start(切换应用/回退重试)。漏掉
    //   s_first_frame_pushed = false 的后果是"切回来黑屏":背光只在首帧推完后打开,
    //   标志停留在 true 就永远不再开背光 —— 画面在推,用户看到的却是黑屏。
    s_backlight_on = false;
    s_first_frame_pushed = false;
    s_prev_valid = false;        // 首帧按"整屏脏区"重画,不拿上一局的棋盘做对比
    s_damage_n = 0;
    s_toast = NULL;
    bsp_display_backlight(0);
    settings_apply();
    font_check_coverage();

    s_running = true;
    if (xTaskCreate(frame_task, "tetris_frame", FRAME_TASK_STACK, NULL, FRAME_TASK_PRIO,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "渲染任务创建失败,剩余堆 %u", (unsigned)esp_get_free_heap_size());
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "就绪:零 LVGL 自绘 %dx%d,脏区推送,目标 %d FPS", UI_W, UI_H,
             1000 / UI_FRAME_MS);
    return ESP_OK;
}

void tetris_ui_stop(void) {
    if (!s_running) return;
    s_running = false;
    (void)chick_run_store_save_settings(&s_settings);
    rtttl_player_stop();
    for (int i = 0; i < 100 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_keys != NULL) {
        vQueueDelete(s_keys);
        s_keys = NULL;
    }
    // 画布/推送缓冲/信号量还给系统:切到另一个游戏时对方要重新分配同样大小的
    // 画布(150KB),不释放就必然分配失败。字形表是编译进来的常量,不用管。
    if (s_stage != NULL) { heap_caps_free(s_stage); s_stage = NULL; }
    if (s_fb != NULL) { heap_caps_free(s_fb); s_fb = NULL; }
    if (s_flush_done != NULL) { vSemaphoreDelete(s_flush_done); s_flush_done = NULL; }
}

void tetris_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_running || s_keys == NULL) return;
    const key_event_t key = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_keys, &key, 0);
}
