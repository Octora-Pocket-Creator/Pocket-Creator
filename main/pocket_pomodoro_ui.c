// main/pocket_pomodoro_ui.c —— 《番茄闹钟》渲染与推屏(零 LVGL 自绘)。
//
// 画面布局(240x320 竖屏):
//   y  12.. 36  阶段名 / 提醒大字(专注 / 休息 / 专注结束,20 号字,居中)
//   y  56..132  剩余时间 mm:ss —— 自绘七段数码管,76px 高
//   y 148..162  本段进度条
//   y 176..200  已完成 N 个
//   y 236..253  按键提示(两行,14 号字)
//
// 为什么时间用七段数码管而不是字库:这是全屏最大的元素(76px),而字形表是按
// "会显示哪些字"裁剪的——为了几个数字把字库放大一倍不划算。七段是纯几何绘制,
// 任意大、任意配色,顺带把字号从 28 拉到 76。中文仍然走 chick_run_glyphs。
//
// 提醒方式:专注走完时整屏按 400ms 明暗交替闪,同时大字亮出"专注结束"/"休息结束",
// 8 秒后自动进下一段。闪屏用背光 PWM 做（bsp_display_backlight）,是全屏级的明暗变化,
// 离得远也能看见,而且不额外画一帧。
//
// 提示音:专注结束放上行三音,休息结束放单声短音 —— 两种一听就能分辨,不用看屏幕;
// 按键只给"状态真的变了"的那个键一声短咔（A 开始/暂停/继续、B 重置）。
// 音频初始化必须排在建画布【之后】:它会吃掉几十 KB 内部 RAM,先建画布才拿得到
// 连续的 150KB。板子上没贴功放时初始化照样成功,只是听不到声 —— 不算故障。
#include "pocket_pomodoro_ui.h"

#include <stdio.h>

#include "chick_run_glyphs.h"
#include "chick_run_text.h"
#include "chick_ui_draw.h"
#include "pocket_pomodoro_model.h"

#include "bsp_audio.h"
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
#include "rtttl_player.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pomodoro";

#define SCREEN_W BSP_LCD_W
#define SCREEN_H BSP_LCD_H

static const chick_font_t *const FONT_MID = &chick_font_20;    // 阶段名 / 提示
static const chick_font_t *const FONT_SMALL = &chick_font_14;  // 按键提示 / 下一段

// --- 配色(延续跑酷那一套深底 + 亮色强调)-----------------------------------
#define COL_BG       0x12161F
#define COL_PANEL    0x1B2130
#define COL_INK      0xF2F5F7
#define COL_INK_DIM  0x8A93A6
#define COL_FOCUS    0xFF6B4A   // 专注:暖红,提神
#define COL_BREAK    0x35C46A   // 休息:绿,松弛
#define COL_DONE     0xF5C542   // 提醒:琥珀
#define COL_BAR_BG   0x2A3242

// --- 布局 -------------------------------------------------------------------
#define PHASE_Y      12
#define DIGIT_Y      56
#define BAR_X        24
#define BAR_Y        148
#define BAR_W        (SCREEN_W - 2 * BAR_X)
#define BAR_H        14
#define COUNT_Y      176
#define HINT_Y       236
#define HINT2_Y      254

// --- 七段数码管几何 ---------------------------------------------------------
// 数字宽 42,高 76,笔画粗 11,笔画两端各留 2px 缝(不然相邻段会粘成一坨)。
#define SEG_W        42
#define SEG_H        76
#define SEG_T        11
#define SEG_GAP      2
#define DIGIT_GAP    10
#define COLON_W      12
// "88:88" 的总宽:4 个数字 + 2 段间距 + 冒号 + 冒号两侧间距。
#define CLOCK_W      (4 * SEG_W + 2 * DIGIT_GAP + COLON_W + 2 * DIGIT_GAP)

#define FRAME_MS     30      // 33 FPS:倒计时按秒跳,这个速率已经绰绰有余
#define STAGE_ROWS   64
#define KEY_QUEUE_DEPTH 8

// 空闲降亮:连着这么久没按键就把背光压到 IDLE_BRIGHT。
// ponytail: 固定阈值 + 固定亮度,够用;真要按环境光自动调,得先有光敏,板上没有。
#define IDLE_MS      60000
#define IDLE_BRIGHT  14
#define ACTIVE_BRIGHT 80
// 提醒闪屏的暗相位。不是 0:全黑会让"屏幕坏了"和"在报警"分不清。
#define FLASH_BRIGHT 8

// 提示音旋律。时长都不到 1 秒:提醒音刚够听清,按键音短到连按也不烦。
static const char *const SFX_FOCUS_DONE = "focusdone:d=16,o=5,b=200:e,g,c6";
static const char *const SFX_BREAK_DONE = "breakdone:d=32,o=5,b=260:c6";
static const char *const SFX_CLICK      = "click:d=32,o=6,b=600:c6";

static uint16_t *s_fb;              // SCREEN_W * SCREEN_H,内部 RAM
static chick_canvas_t s_canvas;
static uint16_t *s_stage;           // 推屏用的连续行缓冲(画布有行跨距)
static SemaphoreHandle_t s_flush_done;
static QueueHandle_t s_keys;
static bool s_audio_ok;             // 音频不可用时只关提示音,不影响计时和画面
static TaskHandle_t s_task;
static volatile bool s_running;

static pomo_timer_t s_timer;
static uint32_t s_saved_focus, s_saved_break;   // 已经落盘的设置(避免重复写 NVS)
static bool s_hud_dirty = true;                 // 需要重推整屏
static bool s_first_frame = false;              // 首帧推完就开背光
static uint32_t s_last_input_ms;
static uint8_t s_backlight = ACTIVE_BRIGHT;
static uint32_t s_shown_sec = UINT32_MAX;       // 上一次画到屏上的秒数
static bool s_shown_flash;                      // 上一次画到屏上的闪屏相位

// --- 设置的持久化(和 pocket_reflex/chick_run_store 一个套路:值很小,改了才写)---
static const char *NVS_NAMESPACE = "pomodoro";
static const char *NVS_KEY_FOCUS = "focus_min";
static const char *NVS_KEY_BREAK = "break_min";
static const char *NVS_KEY_DONE = "done_cnt";
static bool s_nvs_ready;

static void store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区已满或是旧格式:擦掉重来。注意 NVS 是整片擦,这里只保证本应用的
        // 设置能写下去,不指望别的应用的数据还在(见 chick_run_store.c 同样的处理)。
        ESP_LOGW(TAG, "NVS 需要重新初始化(%s),擦除后重试", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    // 拿不到 NVS 不是故障:设置不落盘,闹钟照常能用。
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 不可用(%s),时长设置不保存", esp_err_to_name(err));
        return;
    }
    s_nvs_ready = true;
}

static void store_load(void) {
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint16_t focus = s_timer.focus_min, brk = s_timer.break_min, done = 0;
    (void)nvs_get_u16(handle, NVS_KEY_FOCUS, &focus);
    (void)nvs_get_u16(handle, NVS_KEY_BREAK, &brk);
    (void)nvs_get_u16(handle, NVS_KEY_DONE, &done);
    nvs_close(handle);
    // 走 pomo_set_duration():旧固件写进去的越界值会被夹回合法区间(自愈)。
    (void)pomo_set_duration(&s_timer, true, focus);
    (void)pomo_set_duration(&s_timer, false, brk);
    s_timer.done_count = done;
    s_saved_focus = s_timer.focus_min;
    s_saved_break = s_timer.break_min;
}

static void store_save(void) {
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u16(handle, NVS_KEY_FOCUS, s_timer.focus_min);
    (void)nvs_set_u16(handle, NVS_KEY_BREAK, s_timer.break_min);
    (void)nvs_set_u16(handle, NVS_KEY_DONE, s_timer.done_count);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// --- 七段数码管 -------------------------------------------------------------
// 每段一位:bit0=a(上) bit1=b(右上) bit2=c(右下) bit3=d(下) bit4=e(左下)
// bit5=f(左上) bit6=g(中)。
static const uint8_t SEG_MAP[10] = {
    0x3F,  // 0: a b c d e f
    0x06,  // 1: b c
    0x5B,  // 2: a b d e g
    0x4F,  // 3: a b c d g
    0x66,  // 4: b c f g
    0x6D,  // 5: a c d f g
    0x7D,  // 6: a c d e f g
    0x07,  // 7: a b c
    0x7F,  // 8: 全亮
    0x6F,  // 9: a b c d f g
};

static void draw_digit(const chick_canvas_t *c, int x, int y, int value, uint16_t color) {
    if (value < 0 || value > 9) return;
    const uint8_t seg = SEG_MAP[value];
    const int t = SEG_T, w = SEG_W, h = SEG_H, gap = SEG_GAP;
    const int mid = y + h / 2;

    // 横段:两端内缩 t,再由 gap 各让开一点,免得和竖段糊在一起。
    const int hx = x + t + gap, hw = w - 2 * (t + gap);
    if (seg & 0x01) chick_fill_rect(c, hx, y, hw, t, color);                    // a
    if (seg & 0x40) chick_fill_rect(c, hx, mid - t / 2, hw, t, color);          // g
    if (seg & 0x08) chick_fill_rect(c, hx, y + h - t, hw, t, color);            // d

    // 竖段:同理在上下各让开 gap。
    const int vy = y + t + gap, vh = h / 2 - t - 2 * gap;
    if (seg & 0x20) chick_fill_rect(c, x, vy, t, vh, color);                    // f
    if (seg & 0x02) chick_fill_rect(c, x + w - t, vy, t, vh, color);            // b
    if (seg & 0x10) chick_fill_rect(c, x, mid + t / 2 + gap, t, vh, color);     // e
    if (seg & 0x04) chick_fill_rect(c, x + w - t, mid + t / 2 + gap, t, vh, color);  // c
}

// 画 ":":两个方点。数码管的冒号不闪 —— 这里是闹钟不是钟表,少一个闪烁源。
static void draw_colon(const chick_canvas_t *c, int x, int y, uint16_t color) {
    const int dot = COLON_W;
    chick_fill_rect(c, x, y + SEG_H / 2 - dot * 2, dot, dot, color);
    chick_fill_rect(c, x, y + SEG_H / 2 + dot, dot, dot, color);
}

// "mm:ss"。秒数由 UI 向上取整(剩 1ms 也显示 00:01),这样数字归零和阶段结束是同一刻。
static void draw_clock(const chick_canvas_t *c, uint32_t seconds, uint16_t color) {
    char text[6];
    pomo_format_mmss(seconds, text);
    int x = (SCREEN_W - CLOCK_W) / 2;
    for (int i = 0; i < 5; i++) {
        if (i == 2) {
            draw_colon(c, x, DIGIT_Y, color);
            x += COLON_W + DIGIT_GAP;
            continue;
        }
        draw_digit(c, x, DIGIT_Y, text[i] - '0', color);
        x += SEG_W + DIGIT_GAP;
    }
}

// --- 画面 -------------------------------------------------------------------
static uint16_t phase_color(const pomo_timer_t *t) {
    switch (t->phase) {
        case POMO_PHASE_BREAK: return chick_rgb565(COL_BREAK);
        case POMO_PHASE_DONE: return chick_rgb565(COL_DONE);
        case POMO_PHASE_TITLE: return chick_rgb565(COL_INK);
        default: return chick_rgb565(COL_FOCUS);
    }
}

static const char *phase_label(const pomo_timer_t *t) {
    switch (t->phase) {
        case POMO_PHASE_BREAK: return POMODORO_TEXT_BREAK;
        case POMO_PHASE_FOCUS: return POMODORO_TEXT_FOCUS;
        case POMO_PHASE_DONE:
            // 画面要说的是"什么结束了",不是"接下来是什么"。
            return t->next == (uint8_t)POMO_PHASE_BREAK ? POMODORO_TEXT_FOCUS_END
                                                        : POMODORO_TEXT_BREAK_END;
        default: return POMODORO_TEXT_TITLE;
    }
}

static void draw_hint(const chick_canvas_t *c, const pomo_timer_t *t) {
    if (t->phase == POMO_PHASE_TITLE) {
        chick_draw_text_center(c, SCREEN_W / 2, HINT_Y, FONT_SMALL, chick_rgb565(COL_FOCUS),
                               POMODORO_TEXT_HINT_START);
        chick_draw_text_center(c, SCREEN_W / 2, HINT2_Y, FONT_SMALL, chick_rgb565(COL_INK_DIM),
                               POMODORO_TEXT_HINT_ADJUST);
        return;
    }
    chick_draw_text_center(c, SCREEN_W / 2, HINT_Y, FONT_SMALL, chick_rgb565(COL_INK_DIM),
                           t->running ? POMODORO_TEXT_HINT_PAUSE : POMODORO_TEXT_HINT_START);
    chick_draw_text_center(c, SCREEN_W / 2, HINT2_Y, FONT_SMALL, chick_rgb565(COL_INK_DIM),
                           POMODORO_TEXT_HINT_ADJUST);
}

static void draw(const pomo_timer_t *t) {
    const chick_canvas_t *c = &s_canvas;
    chick_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H, chick_rgb565(COL_BG));

    // 阶段名。通知窗口里整屏底下已经铺了琥珀色,文字用深底反白才看得清。
    const bool notifying = t->phase == POMO_PHASE_DONE;
    if (notifying) {
        chick_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H, chick_rgb565(COL_DONE));
    }
    const uint16_t label_color = notifying ? chick_rgb565(COL_BG) : phase_color(t);
    chick_draw_text_center(c, SCREEN_W / 2, PHASE_Y, FONT_MID, label_color, phase_label(t));

    // 剩余时间:通知窗口里 remaining_ms 摆的是下一段的整段时长,照实显示。
    // 那时整屏是亮琥珀底,数字必须用深色 —— 近白字在亮底上根本看不清。
    const uint32_t secs = (t->remaining_ms + 999u) / 1000u;
    if (notifying) {
        draw_clock(c, secs, chick_rgb565(COL_BG));
    } else {
        // 没在走的时候数字压暗:一眼看出"停着",不用去读提示行。
        draw_clock(c, secs, chick_rgb565(t->running ? COL_INK : COL_INK_DIM));
    }

    // 本段进度条。通知窗口没有"进度"可言,直接留空 —— 不然会显示成满格。
    const int pct = pomo_progress_pct(t);
    chick_stroke_rect(c, BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, 2,
                      chick_rgb565(notifying ? COL_BG : COL_BAR_BG));
    if (!notifying) {
        chick_fill_rect(c, BAR_X, BAR_Y, BAR_W, BAR_H, chick_rgb565(COL_BAR_BG));
        const int fill = BAR_W * pct / 100;
        if (fill > 0) chick_fill_rect(c, BAR_X, BAR_Y, fill, BAR_H, phase_color(t));
    }

    // 已完成番茄数。
    char count[24];
    snprintf(count, sizeof(count), POMODORO_TEXT_DONE_FMT, (unsigned)t->done_count);
    chick_draw_text_center(c, SCREEN_W / 2, COUNT_Y, FONT_MID,
                           chick_rgb565(notifying ? COL_BG : COL_INK), count);

    draw_hint(c, t);
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

// --- 主循环 -----------------------------------------------------------------
static pomo_key_t map_key(bsp_btn_t btn) {
    switch (btn) {
        case BSP_BTN_UP: return POMO_KEY_UP;
        case BSP_BTN_DOWN: return POMO_KEY_DOWN;
        case BSP_BTN_LEFT: return POMO_KEY_LEFT;
        case BSP_BTN_RIGHT: return POMO_KEY_RIGHT;
        case BSP_BTN_B: return POMO_KEY_B;
        default: return POMO_KEY_A;
    }
}

static void set_backlight(uint8_t percent) {
    if (percent == s_backlight) return;
    s_backlight = percent;
    bsp_display_backlight(percent);
}

// 提示音统一从这里出。播放器自己在任务里合成并通过 I2S 推样本,这里不等它。
static void play_sfx(const char *song) {
    if (s_audio_ok) (void)rtttl_player_play(song);
}

static void handle_key(pomo_key_t key, uint32_t now) {
    s_last_input_ms = now;
    switch (key) {
        case POMO_KEY_A: {
            const pomo_phase_t before = s_timer.phase;
            if (pomo_toggle(&s_timer, now) && before != POMO_PHASE_DONE) {
                // 通知窗口里按 A 只是跳过提醒,刚响过提示音,不要再盖一声按键音。
                play_sfx(SFX_CLICK);
            }
            break;
        }
        case POMO_KEY_B:
            if (pomo_reset(&s_timer, now)) play_sfx(SFX_CLICK);
            break;
        case POMO_KEY_UP: (void)pomo_adjust(&s_timer, true); break;
        case POMO_KEY_DOWN: (void)pomo_adjust(&s_timer, false); break;
        case POMO_KEY_LEFT: (void)pomo_switch(&s_timer, false); break;
        case POMO_KEY_RIGHT: (void)pomo_switch(&s_timer, true); break;
    }
    // 只置脏标记:真正重画/推屏交给帧循环(见 frame_task 的"变了才推")。
    // 番茄钟的画面变化很少(每秒一次数字,或者按键),整屏重推 150KB 完全够用,
    // 不值得为它切分脏区 —— 跑酷那边要分,是因为它每帧都在动。
    s_hud_dirty = true;
}

static void frame_task(void *arg) {
    (void)arg;
    pomo_key_t key;
    while (s_running) {
        const uint32_t now = now_ms();

        while (xQueueReceive(s_keys, &key, 0) == pdTRUE) {
            handle_key(key, now);
        }
        if (pomo_tick(&s_timer, now)) {
            s_hud_dirty = true;
            if (s_timer.phase == POMO_PHASE_DONE) {
                // next 是通知窗口结束后要走的阶段:next=BREAK 说明刚结束的是专注那段。
                play_sfx(s_timer.next == POMO_PHASE_BREAK ? SFX_FOCUS_DONE : SFX_BREAK_DONE);
            }
            // 阶段切换/完成数变了才写一次 NVS,不是每帧写。
            if (s_timer.focus_min != s_saved_focus || s_timer.break_min != s_saved_break) {
                s_saved_focus = s_timer.focus_min;
                s_saved_break = s_timer.break_min;
                store_save();
                ESP_LOGI(TAG, "时长已保存:专注 %u 分 / 休息 %u 分",
                         (unsigned)s_timer.focus_min, (unsigned)s_timer.break_min);
            }
        }

        const bool notifying = s_timer.phase == POMO_PHASE_DONE;
        const bool flash_on = pomo_flash_on(&s_timer, now);

        // 背光目标值必须一次算清,不能分两步设:空闲降亮每帧都会跑,若先降亮
        // 再"闪屏时设回亮",降亮就会把闪屏的暗相位顶掉 —— 闪屏等于没有。
        const bool idle = (uint32_t)(now - s_last_input_ms) > IDLE_MS;
        uint8_t target = (idle && !notifying) ? IDLE_BRIGHT : ACTIVE_BRIGHT;
        if (notifying) target = flash_on ? ACTIVE_BRIGHT : FLASH_BRIGHT;
        set_backlight(target);

        // 只在"画面上真有东西变了"时重画 + 推屏:秒数跳一格、或闪屏相位翻一次。
        // 33FPS 每帧全推 150KB 是白烧 SPI —— 正常倒计时是 1 帧/秒,闪屏是 2.5 帧/秒。
        const uint32_t secs = (s_timer.remaining_ms + 999u) / 1000u;
        if (s_hud_dirty || secs != s_shown_sec || flash_on != s_shown_flash) {
            s_shown_sec = secs;
            s_shown_flash = flash_on;
            draw(&s_timer);
            flush_rect(0, 0, SCREEN_W, SCREEN_H);
            s_hud_dirty = false;
            if (!s_first_frame) {
                s_first_frame = true;
                // 首帧已经在屏上了,这时点背光才是"亮屏即画面"。
                bsp_display_backlight(ACTIVE_BRIGHT);
                s_backlight = ACTIVE_BRIGHT;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t pocket_pomodoro_ui_start(void) {
    if (s_running) return ESP_OK;

    s_keys = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(pomo_key_t));
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
    pomo_init(&s_timer);
    store_load();
    s_last_input_ms = now_ms();

    // 提示音初始化:必须排在画布/推屏缓冲都拿到【之后】—— 音频的 DMA 缓冲会占几十 KB
    // 内部 RAM,先建画布才拿得到连续的 150KB(跑酷那边踩过这个坑)。
    // 音频不可用(例如板上没贴功放)只关提示音,不影响计时和画面。
    if (bsp_audio_init() == ESP_OK && rtttl_player_start() == ESP_OK) {
        s_audio_ok = true;
    } else {
        ESP_LOGW(TAG, "音频不可用,本次运行无提示音");
    }

    s_running = true;
    if (xTaskCreate(frame_task, "pomodoro_frame", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "渲染任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "就绪:番茄闹钟 %dx%d,专注 %u 分 / 休息 %u 分,已完成 %u 个,提示音 %s",
             SCREEN_W, SCREEN_H, (unsigned)s_timer.focus_min, (unsigned)s_timer.break_min,
             (unsigned)s_timer.done_count, s_audio_ok ? "开" : "关");
    return ESP_OK;
}

void pocket_pomodoro_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 只认"按下":调时长可能要连按,等 CLICK(抬起)会让手感变拖。
    if (ev != BSP_BTN_PRESS || !s_running || s_keys == NULL) return;
    const pomo_key_t key = map_key(btn);
    (void)xQueueSend(s_keys, &key, 0);
}
