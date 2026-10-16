// main/pocket_reflex_ui.c —— 《方向快手》渲染与推屏(零 LVGL 自绘)。
//
// 画面布局(240x320 竖屏):
//   y   8.. 42  分数(28 号字,居中)
//   y  50.. 62  本题剩余时间条
//   y  64.. 76  上一题反应时间
//   y  84..286  出题区:一个方向箭头
//   y 292..304  命数(三颗爱心,复用 chick_run_sprites 的美术)
//
// 推屏策略比跑酷简单:每帧整幅重画(240x320 的填充 + 一个箭头,很便宜),
// 但只推两块脏区 —— 顶部 HUD 每帧都推(时间条在动),出题区只在状态变化时推。
// 一帧 SPI 流量从 150KB 降到 36KB,20ms 的帧周期里绰绰有余。
//
// ponytail: 这一版不出声。判定反馈只走画面(整块出题区闪绿/红/橙 + 一个词),
// 少一条 I2S 初始化路径、也就少一个开机失败面。要加音效时不必改本文件的结构:
// bsp_audio_init() + rtttl_player_play() 已经是现成的(见 rtttl_player.h),
// 只需在判定那三处各播一段短 RTTTL —— 注意初始化必须排在建画布【之后】,
// 否则音频/I2S 会先把内部 RAM 的大块吃掉(跑酷那边踩过这个坑)。
#include "pocket_reflex_ui.h"

#include <stdio.h>
#include <string.h>

#include "chick_run_glyphs.h"
#include "chick_run_sprites.h"
#include "chick_run_text.h"
#include "chick_ui_draw.h"
#include "pocket_reflex_model.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pocket";

#define SCREEN_W BSP_LCD_W
#define SCREEN_H BSP_LCD_H

static const chick_font_t *const FONT_BIG = &chick_font_28;    // 分数 / 标题
static const chick_font_t *const FONT_MID = &chick_font_20;    // 提示 / 结算
static const chick_font_t *const FONT_SMALL = &chick_font_14;  // 反应时间

// --- 配色(和跑酷一套观感:深底 + 亮色强调)---------------------------------
#define COL_BG      0x12161F
#define COL_PANEL   0x1B2130
#define COL_INK     0xF2F5F7
#define COL_INK_DIM 0x8A93A6
#define COL_ACCENT  0x4FC3F7
#define COL_GOOD    0x35C46A
#define COL_BAD     0xE74C3C
#define COL_SLOW    0xF5A623
#define COL_BAR_BG  0x2A3242

// --- 布局 -------------------------------------------------------------------
#define HUD_H        88
#define SCORE_Y      8
#define BAR_X        20
#define BAR_Y        50
#define BAR_W        (SCREEN_W - 2 * BAR_X)
#define BAR_H        12
#define RT_Y         64
#define ARROW_CX     (SCREEN_W / 2)
#define ARROW_CY     184
#define LIVES_Y      290

// 箭头几何:típ 是顶点到中心的距离,head 是三角头长度,half 是三角头半宽。
#define ARROW_TIP    60
#define ARROW_HEAD   52
#define ARROW_HALF   44
#define ARROW_SHAFT  16

#define FRAME_MS     20      // 50 FPS:出题计时和判定闪烁都够细
#define STAGE_ROWS   64
#define KEY_QUEUE_DEPTH 8

static uint16_t *s_fb;              // SCREEN_W * SCREEN_H,内部 RAM
static chick_canvas_t s_canvas;
static uint16_t *s_stage;           // 推屏用的连续行缓冲(画布有行跨距)
static SemaphoreHandle_t s_flush_done;
static QueueHandle_t s_keys;
static TaskHandle_t s_task;
static volatile bool s_running;

static pocket_reflex_t s_game;
static uint16_t s_saved_best;       // 已经落盘的最好成绩(避免重复写 NVS)
static bool s_play_dirty = true;    // 出题区需要重推
static bool s_hud_dirty = true;     // HUD 需要重推
static bool s_first_frame = false;  // 首帧推完就开背光

// --- 反应时间的持久化(和 chick_run_store 一个套路:NVS 键值很小,破纪录才写)----
static const char *NVS_NAMESPACE = "pocket_reflex";
static const char *NVS_KEY_BEST = "best";
static bool s_nvs_ready;

static void store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化(%s),擦除后重试", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    // 拿不到 NVS 不是故障:最好成绩不落盘,游戏照常玩。
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s),最好成绩不保存", esp_err_to_name(err));
        return;
    }
    s_nvs_ready = true;
}

static uint16_t store_load_best(void) {
    if (!s_nvs_ready) return 0;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    uint16_t best = 0;
    (void)nvs_get_u16(handle, NVS_KEY_BEST, &best);
    nvs_close(handle);
    return best;
}

static void store_save_best(uint16_t best) {
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u16(handle, NVS_KEY_BEST, best);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// --- 绘制原语 ---------------------------------------------------------------
// 索引入色美术(命数爱心):字符图 -> 调色板 -> RGB565,下标 0 透明。
static void blit_sprite(const chick_canvas_t *c, int x, int y, const chick_sprite_t *sprite) {
    for (int row = 0; row < sprite->h; row++) {
        const char *line = sprite->rows[row];
        for (int col = 0; col < sprite->w; col++) {
            const int index = chick_run_sprite_index(line[col]);
            if (index <= 0) continue;
            const int px = x + col, py = y + row;
            if (px < 0 || py < 0 || px >= c->w || py >= c->h) continue;
            c->px[(size_t)py * c->stride_px + px] = chick_rgb565(chick_run_palette[index]);
        }
    }
}

// 画一个方向箭头:先按 pad 画一圈深色描边,再用本色填一遍,保证在亮色闪屏上也看得清。
static void arrow_shape(const chick_canvas_t *c, int cx, int cy, pocket_key_t dir,
                        uint16_t color, int pad) {
    const int tip = -(ARROW_TIP + pad);
    const int head = ARROW_HEAD + pad;
    const int half_max = ARROW_HALF + pad;
    const int shaft = ARROW_SHAFT + pad;
    for (int along = tip; along <= -tip; along++) {
        const int in_head = along - tip;
        const int half = (in_head <= head) ? (half_max * in_head) / head : shaft;
        if (half <= 0) continue;
        switch (dir) {
            case POCKET_KEY_UP:
                chick_fill_rect(c, cx - half, cy + along, 2 * half + 1, 1, color);
                break;
            case POCKET_KEY_DOWN:
                chick_fill_rect(c, cx - half, cy - along, 2 * half + 1, 1, color);
                break;
            case POCKET_KEY_LEFT:
                chick_fill_rect(c, cx + along, cy - half, 1, 2 * half + 1, color);
                break;
            default:  // POCKET_KEY_RIGHT
                chick_fill_rect(c, cx - along, cy - half, 1, 2 * half + 1, color);
                break;
        }
    }
}

// 本题状态 → 箭头颜色。按对/按错/超时时闪一下对应颜色,是给手指的即时反馈。
static uint16_t arrow_color(const pocket_reflex_t *g) {
    if (g->phase != POCKET_PHASE_JUDGE) return chick_rgb565(COL_ACCENT);
    switch (g->judge) {
        case POCKET_JUDGE_HIT: return chick_rgb565(COL_GOOD);
        case POCKET_JUDGE_WRONG: return chick_rgb565(COL_BAD);
        default: return chick_rgb565(COL_SLOW);
    }
}

static const char *judge_word(pocket_judge_t judge) {
    switch (judge) {
        case POCKET_JUDGE_HIT: return "GOOD";
        case POCKET_JUDGE_WRONG: return "WRONG";
        default: return "SLOW";
    }
}

static void draw_hud(const chick_canvas_t *c, const pocket_reflex_t *g, uint32_t now) {
    char text[24];

    snprintf(text, sizeof(text), "%u", (unsigned)g->score);
    chick_draw_text_center(c, SCREEN_W / 2, SCORE_Y, FONT_BIG, chick_rgb565(COL_INK), text);

    // 剩余时间条:绿 → 琥珀 → 红,颜色随剩余量变,不用盯着数字。
    const int pct = pocket_reflex_time_left_pct(g, now);
    chick_stroke_rect(c, BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, 2, chick_rgb565(COL_BAR_BG));
    chick_fill_rect(c, BAR_X, BAR_Y, BAR_W, BAR_H, chick_rgb565(COL_BAR_BG));
    const uint16_t bar_color = pct > 50 ? chick_rgb565(COL_GOOD)
                              : pct > 22 ? chick_rgb565(COL_SLOW)
                                         : chick_rgb565(COL_BAD);
    const int fill = BAR_W * pct / 100;
    if (fill > 0) chick_fill_rect(c, BAR_X, BAR_Y, fill, BAR_H, bar_color);

    if (g->last_rt_ms > 0) {
        snprintf(text, sizeof(text), "%u ms", (unsigned)g->last_rt_ms);
        chick_draw_text_center(c, SCREEN_W / 2, RT_Y, FONT_SMALL, chick_rgb565(COL_INK_DIM), text);
    }
}

static void draw_lives(const chick_canvas_t *c, const pocket_reflex_t *g) {
    const chick_sprite_t *full = chick_run_sprite(CHICK_SPRITE_HEART_FULL);
    const chick_sprite_t *empty = chick_run_sprite(CHICK_SPRITE_HEART_EMPTY);
    if (full == NULL || empty == NULL) return;
    const int step = CHICK_HEART_ART_W + 8;
    const int x0 = (SCREEN_W - (POCKET_LIVES * CHICK_HEART_ART_W + (POCKET_LIVES - 1) * 8)) / 2;
    for (int i = 0; i < POCKET_LIVES; i++) {
        blit_sprite(c, x0 + i * step, LIVES_Y, i < (int)g->lives ? full : empty);
    }
}

static void draw_overlay(const chick_canvas_t *c, const pocket_reflex_t *g) {
    if (g->phase != POCKET_PHASE_TITLE && g->phase != POCKET_PHASE_OVER) return;

    chick_fill_rect_blend(c, 0, HUD_H, SCREEN_W, SCREEN_H - HUD_H, chick_rgb565(COL_BG), 200);
    chick_stroke_rect(c, 16, 100, SCREEN_W - 32, 140, 3, chick_rgb565(COL_ACCENT));

    if (g->phase == POCKET_PHASE_TITLE) {
        chick_draw_text_center(c, SCREEN_W / 2, 122, FONT_BIG, chick_rgb565(COL_INK),
                               POCKET_TEXT_TITLE);
        chick_draw_text_center(c, SCREEN_W / 2, 196, FONT_MID, chick_rgb565(COL_ACCENT),
                               CHICK_TEXT_START_HINT);
    } else {
        char best[32];
        snprintf(best, sizeof(best), CHICK_TEXT_BEST_FMT, (unsigned)g->best);
        chick_draw_text_center(c, SCREEN_W / 2, 122, FONT_BIG, chick_rgb565(COL_INK),
                               CHICK_TEXT_GAME_OVER);
        chick_draw_text_center(c, SCREEN_W / 2, 168, FONT_MID, chick_rgb565(COL_INK_DIM), best);
        chick_draw_text_center(c, SCREEN_W / 2, 200, FONT_MID, chick_rgb565(COL_ACCENT),
                               CHICK_TEXT_AGAIN_HINT);
    }
}

static void draw(const pocket_reflex_t *g, uint32_t now) {
    const chick_canvas_t *c = &s_canvas;
    chick_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H, chick_rgb565(COL_BG));

    // 判定闪烁:整块出题区铺一层对应颜色,离得远也能一眼看出对错。
    if (g->phase == POCKET_PHASE_JUDGE) {
        chick_fill_rect_blend(c, 0, HUD_H, SCREEN_W, SCREEN_H - HUD_H,
                              arrow_color(g), 46);
        chick_draw_text_center(c, SCREEN_W / 2, HUD_H + 10, FONT_MID, arrow_color(g),
                               judge_word(g->judge));
    }

    if (g->phase == POCKET_PHASE_WAIT || g->phase == POCKET_PHASE_JUDGE) {
        arrow_shape(c, ARROW_CX, ARROW_CY, g->arrow, chick_rgb565(COL_PANEL), 4);
        arrow_shape(c, ARROW_CX, ARROW_CY, g->arrow, arrow_color(g), 0);
    }

    draw_lives(c, g);
    draw_hud(c, g, now);
    draw_overlay(c, g);
}

// --- 推屏 -------------------------------------------------------------------
static bool IRAM_ATTR on_flush_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata, void *ctx) {
    (void)io; (void)edata; (void)ctx;
    BaseType_t need_yield = pdFALSE;
    if (s_flush_done != NULL) xSemaphoreGiveFromISR(s_flush_done, &need_yield);
    return need_yield == pdTRUE;
}

static void flush_rect(int x, int y, int w, int h) {
    for (int row = 0; row < h; row += STAGE_ROWS) {
        const int rows = (h - row) > STAGE_ROWS ? STAGE_ROWS : (h - row);
        uint16_t *dst = s_stage;
        // ST7789 走 SPI 要大端 RGB565,画布是小端 → 拷贝时换字节(和 chick_run_ui 一致)。
        for (int r = 0; r < rows; r++) {
            const uint16_t *src = s_fb + (size_t)(y + row + r) * SCREEN_W + x;
            for (int i = 0; i < w; i++) {
                const uint16_t px = src[i];
                dst[i] = (uint16_t)((px >> 8) | (px << 8));
            }
            dst += w;
        }
        esp_lcd_panel_draw_bitmap(bsp_display_panel(), x, y + row, x + w, y + row + rows, s_stage);
        (void)xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
    }
}

static void flush(const pocket_reflex_t *g, uint32_t now) {
    // 时间条每帧都在动,所以 HUD 只要数字变了就重推;出题区只在状态变化时推,
    // 但闪烁期间颜色/透明度没变 —— 这期间每帧推的只有 HUD,省下大半 SPI 流量。
    static int last_pct = -1;
    const int pct = pocket_reflex_time_left_pct(g, now);
    if (pct != last_pct) {
        last_pct = pct;
        s_hud_dirty = true;
    }
    if (s_hud_dirty) {
        flush_rect(0, 0, SCREEN_W, HUD_H);
        s_hud_dirty = false;
    }
    if (s_play_dirty) {
        flush_rect(0, HUD_H, SCREEN_W, SCREEN_H - HUD_H);
        s_play_dirty = false;
        if (!s_first_frame) {
            s_first_frame = true;
            // 首帧已经在屏上了,这时点背光才是"亮屏即画面"。
            bsp_display_backlight(80);
        }
    }
}

// --- 主循环 -----------------------------------------------------------------
static pocket_key_t map_key(bsp_btn_t btn) {
    switch (btn) {
        case BSP_BTN_UP: return POCKET_KEY_UP;
        case BSP_BTN_DOWN: return POCKET_KEY_DOWN;
        case BSP_BTN_LEFT: return POCKET_KEY_LEFT;
        case BSP_BTN_RIGHT: return POCKET_KEY_RIGHT;
        case BSP_BTN_B: return POCKET_KEY_B;
        default: return POCKET_KEY_A;
    }
}

static void frame_task(void *arg) {
    (void)arg;
    pocket_key_t key;
    while (s_running) {
        const uint32_t now = now_ms();

        while (xQueueReceive(s_keys, &key, 0) == pdTRUE) {
            if (pocket_reflex_press(&s_game, key, now)) s_play_dirty = true;
        }
        if (pocket_reflex_tick(&s_game, now)) {
            s_play_dirty = true;
            s_hud_dirty = true;
            // 只在一局结束、且破了纪录时写一次 NVS。
            if (s_game.phase == POCKET_PHASE_OVER && s_game.best > s_saved_best) {
                s_saved_best = s_game.best;
                store_save_best(s_saved_best);
                ESP_LOGI(TAG, "新纪录 %u 分,已保存", (unsigned)s_saved_best);
            }
        }

        draw(&s_game, now);
        flush(&s_game, now);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t pocket_reflex_ui_start(void) {
    if (s_running) return ESP_OK;

    s_keys = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(pocket_key_t));
    if (s_keys == NULL) return ESP_ERR_NO_MEM;

    // 画布 150KB,要在内部 RAM 里一次拿到(和跑酷同一处坑:DMA staging 才能读它,
    // 但画布本身不必是 DMA 内存)。
    const size_t canvas_bytes = (size_t)SCREEN_W * SCREEN_H * 2;
    s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_fb == NULL) s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_DEFAULT);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "画布分配失败,最大可用块 %u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    s_stage = heap_caps_malloc((size_t)SCREEN_W * STAGE_ROWS * 2,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_flush_done = xSemaphoreCreateCounting(1, 0);
    if (s_stage == NULL || s_flush_done == NULL) {
        ESP_LOGE(TAG, "推屏缓冲/信号量分配失败,剩余堆 %u",
                 (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_flush_done };
    (void)esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &cbs, NULL);

    s_canvas = (chick_canvas_t){ s_fb, SCREEN_W, SCREEN_W, SCREEN_H };

    store_init();
    pocket_reflex_init(&s_game, now_ms());
    s_game.best = store_load_best();
    s_saved_best = s_game.best;

    s_running = true;
    if (xTaskCreate(frame_task, "pocket_frame", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "渲染任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "就绪:方向快手 %dx%d,最好成绩 %u", SCREEN_W, SCREEN_H, (unsigned)s_game.best);
    return ESP_OK;
}

void pocket_reflex_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 只认"按下":这个是反应类游戏,等 CLICK(抬起)再判会白丢几十毫秒。
    if (ev != BSP_BTN_PRESS || !s_running || s_keys == NULL) return;
    const pocket_key_t key = map_key(btn);
    (void)xQueueSend(s_keys, &key, 0);
}
