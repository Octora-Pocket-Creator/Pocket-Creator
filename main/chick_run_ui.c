// main/chick_run_ui.c —— 小鸡快跑的界面与交互(零 LVGL 自绘,240x320 ST7789 SPI 屏)。
//
// 同一份代码只面向一块板:差异全部由 BSP 吸收(见 bsp_pins.h)——
//   * ESP32-S3R8 板:六键 GPIO 直连(UP/DOWN/A/B/LEFT/RIGHT)+ 8MB PSRAM。
// 本文件里的输入处理按 BSP 给出的语义键(UP/DOWN/A/B/LEFT/RIGHT)工作,同时兼容
// 只有三个按键能力时的降级路径(BSP_BTN_HAS_B)。
//
// 这套渲染方案针对下面三点做了适配:
//   1. 竖屏 240x320、SPI 屏:没有成对的"帧缓冲+vsync",渲染进一块内部 RAM 画布,
//      每帧只把【变化过的矩形】通过 SPI 推给面板(全屏推一次要 150KB,SPI 根本跑不动);
//   2. 不依赖 PSRAM:不烘焙整屏背景,天空/草地用"按行算好的颜色表"现场填,远山按精灵贴
//      (本板有 8MB PSRAM,但画布与常驻精灵预算都按内部 RAM 算,换板不用重写渲染);
//   3. 渲染任务不绑核:绑核在单核芯片上会直接创建失败。
//
// 渲染分工:
//   * 世界:chick_scene 的脏矩形计划(纯逻辑,有主机测试)+ 精灵游程 memcpy;
//   * HUD/首页/设置页/弹层:chick_ui_draw 的矩形/文字/精灵原语;
//   * 字形:chick_run_glyphs.c(生成物,来自 assets/fonts 的 20/28px 子集)。
#include "chick_run_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "chick_color.h"
#include "chick_dither.h"
#include "chick_gradient.h"
#include "chick_run_glyphs.h"
#include "chick_run_layout.h"
#include "chick_run_model.h"
#include "chick_run_scene.h"
#include "chick_run_settings.h"
#include "chick_run_sprites.h"
#include "chick_run_store.h"
#include "chick_run_text.h"
#include "chick_ui_draw.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rtttl_player.h"

static const char *TAG = "chick_run_ui";

// --- 字体 -------------------------------------------------------------------
#define FONT_CJK  (&chick_font_28)   // 正文、标题、HUD 大号分数
#define FONT_TINY (&chick_font_20)   // HUD 小字(分数/关卡/米数)
#define FONT_TINYER (&chick_font_14) // 最小号(FPS)

// --- 音效(RTTTL 由 rtttl_player 自己的任务合成)--------------
static const char *const SFX_START  = "start:d=16,o=5,b=200:c,e,g,c6";
static const char *const SFX_JUMP   = "jump:d=16,o=6,b=400:e,g";
static const char *const SFX_SLIDE  = "slide:d=32,o=4,b=500:c";
static const char *const SFX_MILE   = "mile:d=32,o=7,b=500:c6";
static const char *const SFX_RECORD = "record:d=16,o=6,b=220:c,e,g,c7";
static const char *const SFX_OVER   = "over:d=8,o=5,b=150:g,f,e,4c";
static const char *const SFX_HURT   = "hurt:d=32,o=4,b=260:g,f";
static const char *const SFX_LEVEL_UP = "level:d=16,o=5,b=200:e,g,c6";
// 道具音效:金币/宝石是短促的"叮",音符是《欢乐颂》的第一句。
// 音符走高优先级通道(见 sound_play_priority):跑酷里跳跃音效很密,
// 普通音效的话这段旋律一拿出来就会被下一个跳跃切掉。
static const char *const SFX_COIN   = "coin:d=16,o=7,b=520:c7,e7";
static const char *const SFX_GEM    = "gem:d=16,o=6,b=480:c,e,g";
static const char *const SFX_NOTE   = "ode:d=8,o=5,b=80:e,e,f,g,g,f,e,d,c,c,d,e,e.,d,d";
// 音乐道具的另一首:《小星星》(Ah! vous dirai-je, maman)——
// C C G G A A G / F F E E D D C 走两遍,最后收在主音 C。
// 与《欢乐颂》一样约 5.8 秒,所以头顶 🎵 的飘动时间共用 CHICK_RUN_NOTE_FLOAT_MS。
// (之前的卡农一直排不对,已换成这首:音符全在 C 大调自然音阶内,没有升降号。)
static const char *const SFX_TWINKLE =
    "twinkle:d=8,o=5,b=166:c,c,g,g,a,a,4g,f,f,e,e,d,d,4c,"
    "c,c,g,g,a,a,4g,f,f,e,e,d,d,4c";
// 加速护身符的"冲刺"旋律:16 分音符的跑动型琶音,5.5 秒 —— 和 CHICK_RUN_BOOST_MS 等长,
// 走高优先级 + 振幅调制(颤音)音色:RTTTL_VOICE_TREMOLO。
// 刻意比原来低一个八度(而且载波是三角波,见 rtttl_wave.c):原来高八度的方波琶音
// 实机听感"太刺耳",现在是"嗡嗡往前冲"但不扎耳朵。
static const char *const SFX_BOOST =
    "boost:d=16,o=4,b=120:a,c5,e5,a5,e5,c5,a,c5,a,c5,e5,a5,e5,c5,a,c5,a,c5,e5,a5,e5,c5,a,c5,"
    "a,c5,e5,a5,e5,c5,a,c5,a,c5,e5,a5,e5,c5,a,c5,a5,e5,c5,a";
static const char *const BGM_MAIN =
    "bgm:d=8,o=4,b=96:c5,g,e,g,4a,f,c,f,g,e,c,e,4d,g,b,4g";

// --- 节奏 -------------------------------------------------------------------
#define CHICK_MAX_DT_MS          120
#define CHICK_RUN_FRAME_MS       130
#define CHICK_FLASH_MS           420
#define CHICK_IDLE_PAUSE_MS      20000
#define CHICK_IDLE_DIM_MS        45000
#define CHICK_BATTERY_PERIOD_MS  10000
#define CHICK_POWER_LOG_MS       60000
#define CHICK_FRAME_MS           20      // 目标 50 FPS:留给 SPI 传输与绘制
#define CHICK_STATS_MS           5000
#define FRAME_TASK_STACK         4096
#define FRAME_TASK_PRIO          4

// --- 配色(与基线一致)------------------------------------------------------
#define APP_SKY_TOP     0x87CEEB
#define APP_SKY_BOTTOM  0x98D8C8
#define APP_GRASS       0x7CB342
#define APP_GRASS_DARK  0x5A9A2A
#define APP_GRASS_DEEP  0x4E8B2E
#define APP_INK         0x2C3E50
#define APP_ACCENT      0xE74C3C
#define APP_PANEL       0x2C3E50
#define APP_PANEL_INK   0xFFF6D0
#define APP_INK_SOFT    0x4E6E80
#define APP_BAR_BG      0xF2F7FA
#define APP_BAR_FILL    0x3FA9F5
#define APP_BAT_GOOD    0x52C41A
// 草地上的进度条:深墨色半透明轨道 + 暖奶油色填充 —— 直接压在绿草上,
// 比原来的"白底 + 亮蓝"柔和得多(白/蓝在绿草上太突兀,实机反馈)。
#define APP_TRACK_ALPHA 110
#define APP_FILL_ALPHA  235

// --- 版面(竖屏 240x320,1 像素 = 1 逻辑像素)-------------------------------
#define LAYOUT_W         CHICK_RUN_SCREEN_W          // 240
#define LAYOUT_H         CHICK_RUN_SCREEN_H          // 320
#define GROUND_Y         CHICK_RUN_GROUND_Y          // 248

// 圆角屏(BSP_LCD_CORNER_RADIUS > 0)时圆角会切掉四角:HUD 一律内缩,否则文字/控件会被圆角切掉。
// 关卡模式把"第 X 关 + 进度条 + 命数爱心"排在同一行、放在地面线下面的草地上
// (用户要求)。于是两种模式的顶部完全一致:只有距离(米)+ 道具徽章 + FPS/电量。
#define LEVEL_LABEL_X    14
#define LEVEL_LABEL_Y    272   // 与 12px 高的进度条/爱心垂直居中(行中心 y=284,大致在草地带的中间)
#define LEVEL_LABEL_W    84    // 盖住 3 位关卡号(第 100 关 = 83px)
#define LEVEL_LABEL_H    24
#define PROGRESS_X       (LEVEL_LABEL_X + LEVEL_LABEL_W + 6)
#define PROGRESS_Y       278
#define PROGRESS_W       64    // 短一点:只是进度提示,大数字看左边的"米"
#define PROGRESS_H       12
#define PROGRESS_BORDER  2
#define PROGRESS_FILL_W  (PROGRESS_W - 2 * PROGRESS_BORDER)
#define PROGRESS_FILL_H  (PROGRESS_H - 2 * PROGRESS_BORDER)
// 命数爱心右对齐到 226,整行横向范围 14..226,避开屏幕底部圆角。
#define HEARTS_GAP       4
#define HEARTS_X         (LAYOUT_W - 14 - HEARTS_W)
#define HEARTS_Y         278
#define HEART_ART_W      CHICK_HEART_ART_W           // 14
#define HEART_ART_H      CHICK_HEART_ART_H           // 12
#define HEARTS_W         (HEART_ART_W * CHICK_RUN_LEVEL_LIVES + HEARTS_GAP * (CHICK_RUN_LEVEL_LIVES - 1))
// 电量:标签右对齐 + 电池外框 + 填充(坐标沿用基线,避开圆角)。
#define BATTERY_LABEL_X  136
#define BATTERY_LABEL_Y  6
#define BATTERY_LABEL_W  46
#define BATTERY_LABEL_H  24
#define BATTERY_BODY_X   194
#define BATTERY_BODY_Y   8
#define BATTERY_BODY_W   28
#define BATTERY_BODY_H   13
#define BATTERY_BORDER   2
#define BATTERY_FILL_W   (BATTERY_BODY_W - 2 * BATTERY_BORDER)
#define BATTERY_FILL_H   (BATTERY_BODY_H - 2 * BATTERY_BORDER)
// FPS:顶部右侧,和电量同一行、同字号。
#define FPS_LABEL_W      54
#define FPS_LABEL_H      18
#define FPS_LABEL_X      (BATTERY_BODY_X - 8 - FPS_LABEL_W)
#define FPS_LABEL_Y      BATTERY_LABEL_Y

#define SETTINGS_TITLE_Y      26
#define SETTINGS_LABEL_X      22
#define SETTINGS_ROW0_Y       96
#define SETTINGS_ROW1_Y       146
#define SETTINGS_ROW2_Y       196
#define SETTINGS_BAR_X        92
#define SETTINGS_BAR_Y_OFF    3
// 右边留出空当:百分比右对齐到 230,而"100%"在当前字号下宽 51px(左边到 179),
// 所以条子收在 168 才不会和"1"重叠(之前 92px 宽会压到 100% 的第一个字符)。
#define SETTINGS_BAR_W        76
#define SETTINGS_BAR_H        18
#define SETTINGS_BAR_BORDER   2
#define SETTINGS_FILL_W       (SETTINGS_BAR_W - 2 * SETTINGS_BAR_BORDER)
#define SETTINGS_FILL_H       (SETTINGS_BAR_H - 2 * SETTINGS_BAR_BORDER)
#define SETTINGS_VALUE_X      186
#define SETTINGS_VALUE_Y_OFF  0
#define SETTINGS_VALUE_W      44

#define HOME_TITLE_Y     14
#define HOME_MODE_Y      70
#define HOME_BEST_Y      100
#define HOME_START_Y     132
#define HOME_SETTINGS_Y  162
// 新玩法提示:空中的金币/宝石/音符要跳起来吃。竖屏的可用空当在标题与玩法行之间。
#define HOME_PICKUP_Y    50
#define HOME_CHICK_X     40
#define HOME_CACTUS_X    168

#define CHICK_HURT_BLINK_MS   120

// --- 动效节奏(逻辑毫秒)-------------------------------------------------
#define CHICK_AURA_FRAME_MS   130   // 加速光圈两帧交替
#define CHICK_NOTE_GLOW_MS    140   // 音符道具的发光动效(收集物与头顶符号共用)
#define CHICK_NOTEF_BOB_MS    90    // 头顶音符飘动的每格时长
#define CHICK_NOTEF_BOB       4     // 头顶音符上下摆幅(逻辑像素)
#define CHICK_NOTEF_GAP       6     // 头顶音符与小鸡美术顶部的间距(逻辑像素)

// --- 道具计数与 HUD 版面(道具) ------------------------------------------
// 距离(米)行:两种模式都在顶部同一行,字号和 FPS 一样。
// 自由模式是本局距离,关卡模式是本关进度(与底部进度条同一口径);
// 得分不进 HUD,只在结算页合成一次"综合得分"。
#define HUD_LINE_X            14
#define HUD_LINE_W            116
#define HUD_LINE_H            18
#define HUD_LINE_Y            6     // 和 FPS/电量同一行
#define PICKUP_BADGE_X        14
#define PICKUP_BADGE_Y        30
#define PICKUP_BADGE_W        (LAYOUT_W - PICKUP_BADGE_X)
#define PICKUP_BADGE_H        22
#define PICKUP_BADGE_ROW_H    26    // 结算页每行的高度
#define PICKUP_BADGE_PER_ROW  2     // 结算页每行放几个徽章
#define PICKUP_ICON_TEXT_GAP  4
#define PICKUP_BADGE_GAP      10
#define PICKUP_BADGE_COUNT    4    // 金币 / 宝石 / 音符 / 加速符

#define CHICK_ART_W       CHICK_RUN_CHICK_ART_W
#define CHICK_ART_H       CHICK_RUN_CHICK_ART_H
#define CHICK_ART_DUCK_H  CHICK_RUN_CHICK_ART_DUCK_H
#define OBSTACLE_POOL     CHICK_RUN_MAX_OBSTACLES
#define BG_TREE_W         28
#define BG_TREE_H         48
#define BG_FLOWER_W       16
#define BG_FLOWER_H       20
#define FG_DECOR_W        24
#define FG_DECOR_H        16
#define CLOUD_CANVAS_W    52
#define CLOUD_CANVAS_H    22
#define MOUNTAIN_CANVAS_W LAYOUT_W
#define MOUNTAIN_CANVAS_H 44
#define HERO_SCALE_NUM    3
#define HERO_SCALE_DEN    2
#define HERO_W            ((CHICK_ART_W * HERO_SCALE_NUM) / HERO_SCALE_DEN)
#define HERO_H            ((CHICK_ART_H * HERO_SCALE_NUM) / HERO_SCALE_DEN)
#define HERO_CACTUS_W     ((CHICK_CACTUS_ART_W * HERO_SCALE_NUM) / HERO_SCALE_DEN)
#define HERO_CACTUS_H     ((CHICK_CACTUS_ART_H * HERO_SCALE_NUM) / HERO_SCALE_DEN)

#define BG_DECOR_COUNT    4
#define BG_DECOR_SPACING  96
#define FG_DECOR_COUNT    3
#define FG_DECOR_SPACING  76
#define CLOUD_COUNT       2
// 云层必须从 HUD 那条徽章带子【下面】开始:云飘进徽章带时,世界层的
// "擦旧位置 → 补背景"会把徽章一起擦掉一帧,看起来就是一缩一闪(实机反馈)。
// 下面的 _Static_assert 把这个间距钉死,以后改 HUD 位置会直接编译失败。
#define CLOUD_Y_BASE      (PICKUP_BADGE_Y + PICKUP_BADGE_H + 4)
#define CLOUD_Y_STEP      60
_Static_assert(CLOUD_Y_BASE >= PICKUP_BADGE_Y + PICKUP_BADGE_H,
               "cloud band overlaps pickup badges (world erase would flicker the HUD)");

// --- 精灵资产(1x,内部 RAM)-----------------------------------------------
typedef struct {
    uint16_t x, len;
} span_t;

typedef struct {
    uint16_t *rgb;
    uint8_t *mask;
    int w, h;
    span_t *spans;          // 每行的不透明游程
    uint32_t *row_off;      // h+1 个行偏移
} sprite_asset_t;

static sprite_asset_t s_assets[CHICK_SPRITE_COUNT];
static uint16_t s_asset_w[CHICK_SPRITE_COUNT];
static uint16_t s_asset_h[CHICK_SPRITE_COUNT];

// 直接从字符美术查表画(零常驻 RAM;定义见下方,远山/首页大鸡用它)。
static void blit_source_indexed(const chick_canvas_t *c, chick_sprite_id_t id,
                                int ox, int oy, int num, int den, const chick_rect_t *clip);

// --- 画布(内部 RAM,SPI DMA 能直接读)--------------------------------------
static uint16_t *s_fb;           // LAYOUT_W * LAYOUT_H
static chick_canvas_t s_canvas;
#define STAGE_ROWS 64
static uint16_t *s_stage;        // 推送用的连续缓冲(画布有行跨距,面板 API 不接受)
static SemaphoreHandle_t s_flush_done;

// --- 世界槽位(z 序)--------------------------------------------------------
typedef struct {
    uint8_t id;
    int16_t x, y;
    bool drawn;
} scene_item_t;
#define SCENE_SLOT_CLOUD   0
#define SCENE_SLOT_BGDECOR (SCENE_SLOT_CLOUD + CLOUD_COUNT)
#define SCENE_SLOT_FGDECOR (SCENE_SLOT_BGDECOR + BG_DECOR_COUNT)
#define SCENE_SLOT_CHICK   (SCENE_SLOT_FGDECOR + FG_DECOR_COUNT)
#define SCENE_SLOT_AURA    (SCENE_SLOT_CHICK + 1)
#define SCENE_SLOT_NOTEF   (SCENE_SLOT_AURA + 1)
#define SCENE_SLOT_OBS     (SCENE_SLOT_NOTEF + 1)
#define SCENE_SLOT_PICKUP  (SCENE_SLOT_OBS + CHICK_RUN_MAX_OBSTACLES)
#define SCENE_ITEM_MAX     (SCENE_SLOT_PICKUP + CHICK_RUN_MAX_PICKUPS)

// 槽位表按 CHICK_SCENE_SLOTS 分配:池子上限调大而忘了改槽位数,这里直接编译失败。
_Static_assert(SCENE_ITEM_MAX <= CHICK_SCENE_SLOTS, "scene slots < item count");

typedef chick_rect_t rect_t;

static scene_item_t s_cur[SCENE_ITEM_MAX];
static chick_scene_fb_t s_scene;          // 单画布:只需一份记账
static rect_t s_overlay_rect;
static bool s_overlay_repair;

// --- 脏区(推给面板的矩形)-------------------------------------------------
#define DAMAGE_MAX 24
static rect_t s_damage[DAMAGE_MAX];
static int s_damage_n;

// --- 状态 -------------------------------------------------------------------
typedef enum { CHICK_SCREEN_HOME, CHICK_SCREEN_GAME, CHICK_SCREEN_SETTINGS } chick_screen_t;
enum { SETTINGS_ROW_BRIGHTNESS = 0, SETTINGS_ROW_VOLUME = 1, SETTINGS_ROW_WARMTH = 2,
       SETTINGS_ROW_COUNT = 3 };
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } chick_key_t;
typedef struct { int32_t x_milli; } decor_item_t;

static chick_run_t s_run;
static chick_screen_t s_screen;
static QueueHandle_t s_keys;
static TaskHandle_t s_task;
static volatile bool s_running;
static uint32_t s_best_saved;
static uint32_t s_best_level_saved;
static uint32_t s_last_input_ms;
static uint32_t s_flash_until_ms;
static uint32_t s_settings_press_ms;
static bool s_backlight_on;
// 背光策略:面板上电(disp_on_off)到首帧推完之间,屏上是未初始化的内容,
// 所以初始化全程黑着,等渲染任务把【首帧】整块推给面板之后再亮屏 ——
// 用户看到的是"亮屏即首页",不会先闪一下花屏(实机反馈)。
// 另外首帧未完成前不把按键当"唤醒"(否则会提前点亮没画完的屏)。
static bool s_first_frame_pushed;
static chick_run_settings_t s_settings;
static uint8_t s_settings_row;
static chick_run_mode_t s_mode;
static bool s_page_drawn;
static bool s_hud_pending;

static decor_item_t s_bg_decor[BG_DECOR_COUNT];
static decor_item_t s_fg_decor[FG_DECOR_COUNT];
static decor_item_t s_clouds[CLOUD_COUNT];

typedef enum { OVERLAY_NONE = 0, OVERLAY_PAUSE, OVERLAY_GAME_OVER, OVERLAY_LEVEL_CLEAR } overlay_t;
static overlay_t s_overlay;
static char s_overlay_body[96];
static char s_overlay_hint[64];

typedef struct {
    uint32_t score_m;
    uint32_t level;
    uint8_t lives;
    uint16_t progress_w;
    uint32_t fps;
    uint16_t coins;      // 道具计数也要在签名里:徽章内容变了整行都得重画
    uint16_t gems;
    uint16_t notes;
    uint16_t boosts;
    uint8_t boost_s;     // 加速倒计时(秒):按秒变化,不是每帧
    int battery;
} hud_sig_t;
static hud_sig_t s_hud_last;
static bool s_hud_force;

static uint32_t s_fps_count;
static uint32_t s_fps_last_ms;
static uint32_t s_fps_value;
static int s_battery_soc = -1;
// 有没有电量计硬件:开机探测一次。没有硬件时整个电量槽不参与渲染。
static bool s_battery_present;
static uint32_t s_battery_next_ms;
static uint32_t s_power_log_next_ms;

// 遥测
static uint32_t s_frames;
static uint32_t s_world_us_total;
static uint32_t s_world_us_max;
static uint32_t s_flush_px;
static uint32_t s_flush_us_total;
static uint32_t s_flush_calls;
static uint32_t s_stats_next_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

static void sound_play_impl(const char *song, bool priority) {
    if (song == NULL) return;
    const esp_err_t err = priority ? rtttl_player_play_priority(song)
                                   : rtttl_player_play(song);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "音频忙,跳过音效");
    }
}

static void sound_play(const char *song) {
    sound_play_impl(song, false);
}

// 奖励/结算类声音:不被打断。
static void sound_play_priority(const char *song) {
    sound_play_impl(song, true);
}

// 指定音色的高优先级旋律(加速护身符用振幅调制音色)。
static void sound_play_voice(const char *song, rtttl_voice_t voice) {
    if (song == NULL) return;
    if (rtttl_player_play_voice(song, voice) != ESP_OK) {
        ESP_LOGD(TAG, "音频忙,跳过音效");
    }
}

// 世界层的擦除/重贴不能盖到 HUD:云飘过 HUD 那几条带子时,如果只让世界层重画,
// HUD 就会被背景补掉一帧 —— 表现就是一缩一闪。定义见下方 HUD 小节。
static void hud_mark_if_touched(const chick_rect_t *rects, int count);

// --- 精灵资产 ---------------------------------------------------------------
static void asset_build(sprite_asset_t *a, chick_sprite_id_t id, int num, int den) {
    const chick_sprite_t *s = chick_run_sprite(id);
    if (s == NULL) return;
    const int w = chick_run_scale_extent(s->w, num, den);
    const int h = chick_run_scale_extent(s->h, num, den);
    a->w = w;
    a->h = h;
    a->rgb = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    a->mask = heap_caps_malloc((size_t)w * h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (a->rgb == NULL || a->mask == NULL) {
        ESP_LOGE(TAG, "精灵资产分配失败 %dx%d", w, h);
        return;
    }
    for (int y = 0; y < h; y++) {
        const int sy = chick_run_scale_src_index(y, s->h, num, den);
        for (int x = 0; x < w; x++) {
            const int sx = chick_run_scale_src_index(x, s->w, num, den);
            const int idx = chick_run_sprite_index(s->rows[sy][sx]);
            if (idx > 0) {
                a->rgb[(size_t)y * w + x] = chick_rgb565(chick_run_palette[idx]);
                a->mask[(size_t)y * w + x] = 1;
            } else {
                a->mask[(size_t)y * w + x] = 0;
            }
        }
    }
    // 预计算不透明游程:blit 变成按段 memcpy(逐像素判掩码在 240x320 的帧率下太慢)。
    a->row_off = heap_caps_malloc(sizeof(uint32_t) * ((size_t)h + 1), MALLOC_CAP_INTERNAL);
    if (a->row_off == NULL) return;
    uint32_t total = 0;
    for (int y = 0; y < h; y++) {
        a->row_off[y] = total;
        int x = 0;
        while (x < w) {
            if (a->mask[(size_t)y * w + x] == 0) { x++; continue; }
            total++;
            while (x < w && a->mask[(size_t)y * w + x] != 0) x++;
        }
    }
    a->row_off[h] = total;
    if (total == 0) return;
    a->spans = heap_caps_malloc(sizeof(span_t) * total, MALLOC_CAP_INTERNAL);
    if (a->spans == NULL) return;
    uint32_t at = 0;
    for (int y = 0; y < h; y++) {
        int x = 0;
        while (x < w) {
            if (a->mask[(size_t)y * w + x] == 0) { x++; continue; }
            const int start = x;
            while (x < w && a->mask[(size_t)y * w + x] != 0) x++;
            a->spans[at].x = (uint16_t)start;
            a->spans[at].len = (uint16_t)(x - start);
            at++;
        }
    }
}

static bool assets_init(void) {
    const size_t sprite_ram = 0;
    for (int id = 0; id < CHICK_SPRITE_COUNT; id++) {
        if (id == CHICK_SPRITE_MOUNTAINS) continue;   // 作为静态装饰单独贴
        asset_build(&s_assets[id], (chick_sprite_id_t)id, 1, 1);
        if (s_assets[id].rgb == NULL || s_assets[id].mask == NULL) return false;
        s_asset_w[id] = (uint16_t)s_assets[id].w;
        s_asset_h[id] = (uint16_t)s_assets[id].h;
    }
    // 远山与首页大鸡不展开(见 blit_source_indexed):省下约 48KB 常驻 RAM。
    s_asset_w[CHICK_SPRITE_MOUNTAINS] = MOUNTAIN_CANVAS_W;
    s_asset_h[CHICK_SPRITE_MOUNTAINS] = MOUNTAIN_CANVAS_H;
    (void)sprite_ram;
    return true;
}

// --- 背景(无 PSRAM:不烘焙整屏,按行算颜色表 + 远山精灵)------------------
// RGB565 每通道只有 5/6 位,长渐变必须抖动才能不出硬边。每行预先量化成
// "基准档位 + 抖动阈值"(见 main/chick_gradient.c),填充时与 32x32 蓝噪声掩码比较:
// 行平均严格执行理想渐变(所以没有"大条分层"),进位位置无规则(所以没有网格颗粒)。
static chick_gradient_row_t s_sky_row[GROUND_Y];
static chick_gradient_row_t s_grass_row[LAYOUT_H - GROUND_Y];
// 远山基线下压 10 像素后由地面盖住:本板是 1:1,直接用 10;
// 按 2 倍缩放实现时要写 +20 屏幕像素才是同一个位置,写成 20 会把山压掉一大半。
#define MOUNTAIN_Y (GROUND_Y - MOUNTAIN_CANVAS_H + 10)

static void bg_prepare(void) {
    for (int y = 0; y < GROUND_Y; y++) {
        chick_gradient_row(APP_SKY_TOP, APP_SKY_BOTTOM, y, GROUND_Y, &s_sky_row[y]);
    }
    for (int y = 0; y < LAYOUT_H - GROUND_Y; y++) {
        chick_gradient_row(APP_GRASS, APP_GRASS_DEEP, y, LAYOUT_H - GROUND_Y,
                           &s_grass_row[y]);
    }
}

// --- 脏区记账 ---------------------------------------------------------------
static void damage_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LAYOUT_W) x1 = LAYOUT_W;
    if (y1 > LAYOUT_H) y1 = LAYOUT_H;
    if (x0 >= x1 || y0 >= y1) return;
    // 与已有脏区重叠就并成一个包围盒:SPI 传输次数比面积更贵。
    const int MERGE_GAP = 12;      // 间隙 ≤12px 也并起来:一次传输的固定开销 ~1ms 量级
    for (int i = 0; i < s_damage_n; i++) {
        rect_t *r = &s_damage[i];
        if (x0 <= r->x + r->w + MERGE_GAP && r->x <= x1 + MERGE_GAP &&
            y0 <= r->y + r->h + MERGE_GAP && r->y <= y1 + MERGE_GAP) {
            const int nx0 = r->x < x0 ? r->x : x0;
            const int ny0 = r->y < y0 ? r->y : y0;
            const int nx1 = (r->x + r->w) > x1 ? (r->x + r->w) : x1;
            const int ny1 = (r->y + r->h) > y1 ? (r->y + r->h) : y1;
            r->x = nx0;
            r->y = ny0;
            r->w = nx1 - nx0;
            r->h = ny1 - ny0;
            return;
        }
    }
    if (s_damage_n >= DAMAGE_MAX) {          // 满了就退化成整屏,保证不漏
        s_damage[0] = (rect_t){ 0, 0, LAYOUT_W, LAYOUT_H };
        s_damage_n = 1;
        return;
    }
    s_damage[s_damage_n++] = (rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

// --- 画布工具 ---------------------------------------------------------------
static void canvas_fill_rows(const chick_canvas_t *c, int x, int y, int w, int h) {
    for (int row = 0; row < h; row++) {
        const int py = y + row;
        if (py < 0 || py >= LAYOUT_H) continue;
        uint16_t *dst = c->px + (size_t)py * c->stride_px + x;

        // 大多数行是"完全平坦"的(只有量化台阶附近的行才带抖动):
        // 平坦行整行一个颜色,不用逐像素查掩码 —— 全屏重绘时这是最热的循环。
        const chick_gradient_row_t *gr = py < GROUND_Y ? &s_sky_row[py]
                                                       : &s_grass_row[py - GROUND_Y];
        const uint16_t flat = chick_gradient_apply(gr, 0);
        if (flat == chick_gradient_apply(gr, CHICK_DITHER_LEVELS - 1)) {
            for (int i = 0; i < w; i++) dst[i] = flat;
            continue;
        }
        // 抖动行的掩码按【绝对屏幕坐标】取:草坪的 y 用 py(而不是 py-GROUND_Y),
        // 否则草坪与天空会共用同一套行相位,接缝处会出现一条突兀的纹理。
        for (int i = 0; i < w; i++) {
            dst[i] = chick_gradient_apply(gr, chick_dither_mask(x + i, py));
        }
    }
}

// 回补一块世界:背景(天空/草地) + 压在上面的静态远山 + 精灵由调用方重贴。
static void bg_fill_rect(const chick_canvas_t *c, const rect_t *r) {
    int x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LAYOUT_W) x1 = LAYOUT_W;
    if (y1 > LAYOUT_H) y1 = LAYOUT_H;
    if (x0 >= x1 || y0 >= y1) return;
    // ★ 层级顺序:天空 → 远山 → 地面。远山的下沿故意比地面线低 20px,
    //   必须由地面盖住它;反过来(先地面后贴山)山就会压到草地上。
    if (y0 < GROUND_Y) {
        canvas_fill_rows(c, x0, y0, x1 - x0, (y1 < GROUND_Y ? y1 : GROUND_Y) - y0);
    }
    const chick_sprite_t *m = chick_run_sprite(CHICK_SPRITE_MOUNTAINS);
    if (m != NULL && y1 > MOUNTAIN_Y && y0 < MOUNTAIN_Y + (int)m->h && y0 < GROUND_Y) {
        blit_source_indexed(c, CHICK_SPRITE_MOUNTAINS, 0, MOUNTAIN_Y, 1, 1, r);
    }
    if (y1 > GROUND_Y) {
        const int gy = (y0 > GROUND_Y ? y0 : GROUND_Y);
        canvas_fill_rows(c, x0, gy, x1 - x0, y1 - gy);
        if (y0 <= GROUND_Y && y1 > GROUND_Y) {   // 地面线(压在草上沿)
            for (int x = x0; x < x1; x++) {
                c->px[(size_t)GROUND_Y * c->stride_px + x] = chick_rgb565(APP_GRASS_DARK);
            }
        }
    }
}

static void canvas_blit_asset(const chick_canvas_t *c, const sprite_asset_t *a, int x, int y) {
    chick_blit_sprite(c, x, y, a->rgb, a->mask, a->w, a->h);
}

// 常驻 RAM 有限:把每个精灵都预展开成 RGB565+掩码要 ~100KB,加上画布就放不下了。
// 所以“每帧只画一两次”的静态美术(远山、首页大鸡)直接从字符美术查表画 ——
// 每像素多一次查表,但省掉全部常驻缓冲。热点精灵(小鸡/障碍/云/装饰)仍然预展开,
// 因为它们每帧要贴很多次,逐像素查表会被 SP7589 级别的 SPI 帧率拖垮。
static void blit_source_indexed(const chick_canvas_t *c, chick_sprite_id_t id,
                                int ox, int oy, int num, int den, const chick_rect_t *clip) {
    const chick_sprite_t *sprite = chick_run_sprite(id);
    if (sprite == NULL) return;
    const int w = chick_run_scale_extent(sprite->w, num, den);
    const int h = chick_run_scale_extent(sprite->h, num, den);
    // clip:只画这个矩形内的像素(回补一小块背景时不能整张重画,240x44 的远山
    // 在十几个擦除矩形里各画一遍是灾难 —— 实测把世界耗时从 2ms 拉到 118ms)。
    int cx0 = 0, cx1 = c->w, cy0 = 0, cy1 = c->h;
    if (clip != NULL) {
        cx0 = clip->x < 0 ? 0 : clip->x;
        cy0 = clip->y < 0 ? 0 : clip->y;
        cx1 = clip->x + clip->w > c->w ? c->w : clip->x + clip->w;
        cy1 = clip->y + clip->h > c->h ? c->h : clip->y + clip->h;
    }
    for (int y = 0; y < h; y++) {
        const int py = oy + y;
        if (py < cy0 || py >= cy1) continue;
        const int sy = chick_run_scale_src_index(y, sprite->h, num, den);
        for (int x = 0; x < w; x++) {
            const int px = ox + x;
            if (px < cx0 || px >= cx1) continue;
            const int sx = chick_run_scale_src_index(x, sprite->w, num, den);
            const int idx = chick_run_sprite_index(sprite->rows[sy][sx]);
            if (idx > 0) {
                c->px[(size_t)py * c->stride_px + px] = chick_rgb565(chick_run_palette[idx]);
            }
        }
    }
}

static void world_blit(const chick_canvas_t *c, chick_sprite_id_t id, int ox, int oy) {
    const sprite_asset_t *a = &s_assets[id];
    if (a->rgb == NULL || a->spans == NULL) return;
    int x0 = 0, x1 = a->w, y0 = 0, y1 = a->h;
    if (ox < 0) x0 = -ox;
    if (oy < 0) y0 = -oy;
    if (ox + a->w > LAYOUT_W) x1 = LAYOUT_W - ox;
    if (oy + a->h > LAYOUT_H) y1 = LAYOUT_H - oy;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        const uint16_t *src = a->rgb + (size_t)y * a->w;
        uint16_t *dst = c->px + (size_t)(oy + y) * c->stride_px + ox;
        for (uint32_t i = a->row_off[y]; i < a->row_off[y + 1]; i++) {
            int sx = a->spans[i].x;
            int len = a->spans[i].len;
            if (sx >= x1 || sx + len <= x0) continue;
            if (sx < x0) { len -= x0 - sx; sx = x0; }
            if (sx + len > x1) len = x1 - sx;
            if (len > 0) memcpy(dst + sx, src + sx, (size_t)len * 2);
        }
    }
}

static void world_redraw_rect(const chick_canvas_t *c, const rect_t *r) {
    bg_fill_rect(c, r);
    for (int i = 0; i < SCENE_ITEM_MAX; i++) {
        if (!s_cur[i].drawn) continue;
        const rect_t ir = { s_cur[i].x, s_cur[i].y, s_asset_w[s_cur[i].id], s_asset_h[s_cur[i].id] };
        if (chick_rect_overlap(&ir, r)) world_blit(c, (chick_sprite_id_t)s_cur[i].id, s_cur[i].x, s_cur[i].y);
    }
}

// --- 世界采集 ---------------------------------------------------------------
static void scene_collect(void) {
    static const chick_sprite_id_t cloud_sprites[CLOUD_COUNT] = {
        CHICK_SPRITE_CLOUD_BIG, CHICK_SPRITE_CLOUD_SMALL,
    };
    static const chick_sprite_id_t bg_sprites[BG_DECOR_COUNT] = {
        CHICK_SPRITE_TREE, CHICK_SPRITE_FLOWER_RED,
        CHICK_SPRITE_TREE_DARK, CHICK_SPRITE_FLOWER_WHITE,
    };
    static const chick_sprite_id_t chick_poses[4] = {
        CHICK_SPRITE_CHICK_RUN_A, CHICK_SPRITE_CHICK_RUN_B,
        CHICK_SPRITE_CHICK_JUMP, CHICK_SPRITE_CHICK_DUCK,
    };
    for (int i = 0; i < CLOUD_COUNT; i++) {
        s_cur[SCENE_SLOT_CLOUD + i] = (scene_item_t){ (uint8_t)cloud_sprites[i],
            (int16_t)(s_clouds[i].x_milli / 1000),
            (int16_t)(CLOUD_Y_BASE + i * CLOUD_Y_STEP), true };
    }
    for (int i = 0; i < BG_DECOR_COUNT; i++) {
        const chick_sprite_t *s = chick_run_sprite(bg_sprites[i]);
        s_cur[SCENE_SLOT_BGDECOR + i] = (scene_item_t){ (uint8_t)bg_sprites[i],
            (int16_t)(s_bg_decor[i].x_milli / 1000),
            (int16_t)(GROUND_Y + 4 - (int)s->h), true };
    }
    for (int i = 0; i < FG_DECOR_COUNT; i++) {
        s_cur[SCENE_SLOT_FGDECOR + i] = (scene_item_t){ (uint8_t)CHICK_SPRITE_TUFT,
            (int16_t)(s_fg_decor[i].x_milli / 1000), (int16_t)(GROUND_Y + 8 - FG_DECOR_H), true };
    }
    int pose = 0;
    if (!s_run.on_ground) pose = 2;
    else if (s_run.ducking) pose = 3;
    else pose = (int)((s_run.elapsed_ms / CHICK_RUN_FRAME_MS) % 2u);
    const chick_sprite_t *cs = chick_run_sprite(chick_poses[pose]);
    const int drawn_h = (int)cs->h;
    bool chick_visible = true;
    if (s_run.invincible_ms > 0 && ((s_run.invincible_ms / CHICK_HURT_BLINK_MS) % 2u) == 0u) {
        chick_visible = false;
    }
    const int chick_x = CHICK_RUN_CHICK_X - CHICK_RUN_CHICK_ART_MARGIN_X;
    const int chick_y = GROUND_Y - chick_run_chick_y(&s_run) - drawn_h;
    s_cur[SCENE_SLOT_CHICK] = (scene_item_t){ (uint8_t)chick_poses[pose],
        (int16_t)chick_x, (int16_t)chick_y, chick_visible };

    // 加速光圈:画在小鸡背后,两帧交替一大一小 —— 这就是"身上的光圈在呼吸"。
    if (chick_run_is_boosted(&s_run)) {
        const int dx = (CHICK_AURA_ART_W - (int)cs->w) / 2;
        const int dy = (CHICK_AURA_ART_H - drawn_h) / 2;
        const bool tight = ((s_run.boost_ms / CHICK_AURA_FRAME_MS) % 2u) != 0u;
        s_cur[SCENE_SLOT_AURA] = (scene_item_t){
            (uint8_t)(tight ? CHICK_SPRITE_AURA_B : CHICK_SPRITE_AURA_A),
            (int16_t)(chick_x - dx), (int16_t)(chick_y - dy), true };
    } else {
        s_cur[SCENE_SLOT_AURA].drawn = false;
    }

    // 音符道具的"战利品":吃到之后小鸡头上飘一个一闪一闪的 🎵,
    // 上下摆动 0..CHICK_NOTEF_BOB px,和《欢乐颂》同时结束。
    if (chick_run_note_float_ms(&s_run) > 0) {
        int bob = (int)((s_run.elapsed_ms / CHICK_NOTEF_BOB_MS) % (2u * CHICK_NOTEF_BOB));
        if (bob > (int)CHICK_NOTEF_BOB) bob = (int)(2u * CHICK_NOTEF_BOB) - bob;
        const bool glow = ((s_run.elapsed_ms / CHICK_NOTE_GLOW_MS) % 2u) != 0u;
        s_cur[SCENE_SLOT_NOTEF] = (scene_item_t){
            (uint8_t)(glow ? CHICK_SPRITE_NOTE_GLOW : CHICK_SPRITE_NOTE),
            (int16_t)(chick_x + ((int)cs->w - CHICK_NOTE_ART_W) / 2),
            (int16_t)(chick_y - (CHICK_NOTEF_GAP + bob) - CHICK_NOTE_ART_H), true };
    } else {
        s_cur[SCENE_SLOT_NOTEF].drawn = false;
    }

    for (int i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        const chick_obstacle_t *ob = &s_run.obstacles[i];
        scene_item_t *it = &s_cur[SCENE_SLOT_OBS + i];
        if (!ob->active) { it->drawn = false; continue; }
        const chick_obstacle_shape_t *shape = chick_run_obstacle_shape(ob->kind);
        it->id = (uint8_t)(ob->kind == CHICK_OBSTACLE_FENCE ? CHICK_SPRITE_FENCE :
                  ob->kind == CHICK_OBSTACLE_CACTUS ? CHICK_SPRITE_CACTUS : CHICK_SPRITE_FOX);
        it->x = (int16_t)chick_run_obstacle_x(ob);
        it->y = (int16_t)(GROUND_Y - (shape->y_bottom + shape->h));
        it->drawn = true;
    }

    // 收集道具:金币 / 宝石 / 音符 / 加速护身符。位置由模型给(底边离地高度),
    // 这里只换成屏幕像素。音符两帧交替 = 一闪一闪的发光动效。
    const bool note_glow = ((s_run.elapsed_ms / CHICK_NOTE_GLOW_MS) % 2u) != 0u;
    for (int i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        const chick_pickup_t *pickup = &s_run.pickups[i];
        scene_item_t *it = &s_cur[SCENE_SLOT_PICKUP + i];
        if (!pickup->active) { it->drawn = false; continue; }
        switch (pickup->kind) {
            case CHICK_PICKUP_GEM:   it->id = CHICK_SPRITE_GEM; break;
            case CHICK_PICKUP_NOTE:  it->id = note_glow ? CHICK_SPRITE_NOTE_GLOW
                                                        : CHICK_SPRITE_NOTE; break;
            case CHICK_PICKUP_BOOST: it->id = CHICK_SPRITE_BOOST; break;
            case CHICK_PICKUP_COIN:
            default:                 it->id = CHICK_SPRITE_COIN; break;
        }
        const chick_pickup_shape_t *shape = chick_run_pickup_shape(pickup->kind);
        it->x = (int16_t)chick_run_pickup_x(pickup);
        it->y = (int16_t)(GROUND_Y - (chick_run_pickup_y(pickup) + shape->h));
        it->drawn = true;
    }
}

static void world_draw(const chick_canvas_t *c) {
    chick_scene_item_t items[CHICK_SCENE_SLOTS];
    for (int i = 0; i < SCENE_ITEM_MAX; i++) {
        items[i] = (chick_scene_item_t){ s_cur[i].id, s_cur[i].x, s_cur[i].y, s_cur[i].drawn };
    }
    chick_rect_t extra;
    int extra_count = 0;
    if (s_overlay_repair) {
        extra = (chick_rect_t){ (int16_t)s_overlay_rect.x, (int16_t)s_overlay_rect.y,
                                (int16_t)s_overlay_rect.w, (int16_t)s_overlay_rect.h };
        extra_count = 1;
        s_overlay_repair = false;
    }
    chick_scene_plan_t plan;
    chick_scene_plan(&s_scene, items, s_asset_w, s_asset_h, &extra, extra_count, &plan);
    // 世界层碰到的 HUD 带子:把 HUD 标脏,让本帧随后的 hud_draw() 再画一遍
    // (顺序就是 world_draw → hud_draw)。整帧重填背景时 HUD 同样要重画。
    if (plan.refill) {
        s_hud_pending = true;
    } else {
        hud_mark_if_touched(plan.erase, plan.erase_count);
        for (int i = 0; i < SCENE_ITEM_MAX; i++) {
            if (!plan.redraw[i] || !s_cur[i].drawn) continue;
            const chick_rect_t item = { s_cur[i].x, s_cur[i].y,
                                        (int16_t)s_asset_w[s_cur[i].id],
                                        (int16_t)s_asset_h[s_cur[i].id] };
            hud_mark_if_touched(&item, 1);
        }
    }
    if (plan.refill) {
        bg_fill_rect(c, &(rect_t){ 0, 0, LAYOUT_W, LAYOUT_H });
        damage_add(0, 0, LAYOUT_W, LAYOUT_H);
    } else {
        for (int i = 0; i < plan.erase_count; i++) {
            const rect_t r = { plan.erase[i].x, plan.erase[i].y, plan.erase[i].w, plan.erase[i].h };
            bg_fill_rect(c, &r);
            damage_add(r.x, r.y, r.w, r.h);
        }
    }
    for (int i = 0; i < SCENE_ITEM_MAX; i++) {
        if (plan.redraw[i] && s_cur[i].drawn) {
            world_blit(c, (chick_sprite_id_t)s_cur[i].id, s_cur[i].x, s_cur[i].y);
            damage_add(s_cur[i].x, s_cur[i].y, s_asset_w[s_cur[i].id], s_asset_h[s_cur[i].id]);
        }
    }
    chick_scene_commit(&s_scene, items);
}

// --- HUD --------------------------------------------------------------------
// 距离(米)行:两种模式的槽位和位置都一样。
static rect_t hud_rect_meters(void) {
    return (rect_t){ HUD_LINE_X, HUD_LINE_Y, HUD_LINE_W, HUD_LINE_H };
}
static rect_t hud_rect_level(void) { return (rect_t){ LEVEL_LABEL_X, LEVEL_LABEL_Y, LEVEL_LABEL_W, LEVEL_LABEL_H }; }
static rect_t hud_rect_progress(void) { return (rect_t){ PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H }; }
static rect_t hud_rect_hearts(void) { return (rect_t){ HEARTS_X, HEARTS_Y, (int)HEARTS_W, HEART_ART_H }; }
static rect_t hud_rect_fps(void) { return (rect_t){ FPS_LABEL_X, FPS_LABEL_Y, FPS_LABEL_W, FPS_LABEL_H }; }
// 脏区固定盖住整行:徽章会随计数出现/消失,宽度也在变,按最大范围擦才不会留残影。
static rect_t hud_rect_pickups(void) {
    return (rect_t){ PICKUP_BADGE_X, PICKUP_BADGE_Y, PICKUP_BADGE_W, PICKUP_BADGE_H };
}
// 电量只留图标(用户要求去掉百分比文字),所以脏区只覆盖图标本身。
static rect_t hud_rect_battery(void) { return (rect_t){ BATTERY_BODY_X - 2, BATTERY_BODY_Y - 2,
    BATTERY_BODY_W + 8, BATTERY_BODY_H + 4 }; }

static uint16_t progress_width(void) {
    uint32_t m = s_run.score_m;
    if (m > CHICK_RUN_LEVEL_TARGET_M) m = CHICK_RUN_LEVEL_TARGET_M;
    int width = (int)(((uint32_t)PROGRESS_FILL_W * m) / CHICK_RUN_LEVEL_TARGET_M);
    if (m > 0 && width < 1) width = 1;
    return (uint16_t)width;
}

// 距离(米):跑动中唯一的活动数字。
// 得分(距离 + 道具加分)不在 HUD 上滚动显示,只在结算页给一次综合得分 ——
// 数字不会因为吃到道具而跳,孩子也不会去追那个数。
static void hud_draw_meters(const chick_canvas_t *c) {
    char text[24];
    snprintf(text, sizeof(text), CHICK_TEXT_METERS_FMT, (unsigned)s_run.score_m);
    const bool flash = s_flash_until_ms != 0 && now_ms() < s_flash_until_ms;
    chick_draw_text(c, HUD_LINE_X, HUD_LINE_Y, FONT_TINYER,
                    chick_rgb565(flash ? APP_ACCENT : APP_INK), text);
}

// 道具徽章:HUD 图标 + 文本。文本由模型状态决定(加速符在生效时显示倒计时),
// 两处(HUD 徽章行、结算页明细行)共用这一份,不会出现"两处显示不一样"。
static const chick_sprite_id_t PICKUP_BADGE_SPRITES[PICKUP_BADGE_COUNT] = {
    CHICK_SPRITE_COIN, CHICK_SPRITE_GEM, CHICK_SPRITE_NOTE, CHICK_SPRITE_BOOST,
};

// 该徽章本帧要不要显示?要显示就把文本写进 out。
static bool pickup_badge_text(int index, char *out, size_t size) {
    switch (index) {
        case 0:
            if (s_run.coins == 0) return false;
            snprintf(out, size, "x%u", (unsigned)s_run.coins);
            return true;
        case 1:
            if (s_run.gems == 0) return false;
            snprintf(out, size, "x%u", (unsigned)s_run.gems);
            return true;
        case 2:
            if (s_run.notes == 0) return false;
            snprintf(out, size, "x%u", (unsigned)s_run.notes);
            return true;
        case 3:
        default:
            // 加速符:生效期间显示剩余秒数(倒计时),否则显示收集数量。
            if (chick_run_is_boosted(&s_run)) {
                snprintf(out, size, "%us", (unsigned)chick_run_boost_seconds(&s_run));
                return true;
            }
            if (s_run.boosts == 0) return false;
            snprintf(out, size, "x%u", (unsigned)s_run.boosts);
            return true;
    }
}

// 图标 + 数字的垂直对齐:行高比图标矮时靠上,否则居中。
static int pickup_text_y(int icon_h, int y) {
    const int slack = icon_h - (int)FONT_TINYER->line_height;
    return slack > 0 ? y + slack / 2 : y;
}

static void hud_draw_pickups(const chick_canvas_t *c) {
    int x = hud_rect_pickups().x;
    const int y = hud_rect_pickups().y;
    for (int i = 0; i < PICKUP_BADGE_COUNT; i++) {
        char text[16];
        if (!pickup_badge_text(i, text, sizeof(text))) continue;
        const sprite_asset_t *icon = &s_assets[PICKUP_BADGE_SPRITES[i]];
        canvas_blit_asset(c, icon, x, y);
        chick_draw_text(c, x + icon->w + PICKUP_ICON_TEXT_GAP, pickup_text_y(icon->h, y),
                        FONT_TINYER, chick_rgb565(APP_INK), text);
        x += icon->w + PICKUP_ICON_TEXT_GAP + chick_text_measure(FONT_TINYER, text).width +
             PICKUP_BADGE_GAP;
    }
}

static void hud_draw_fps(const chick_canvas_t *c) {
    char text[16];
    snprintf(text, sizeof(text), "FPS %u", (unsigned)s_fps_value);
    chick_draw_text_right(c, FPS_LABEL_X + FPS_LABEL_W, FPS_LABEL_Y, FONT_TINYER,
                          chick_rgb565(APP_INK), text);
}

// 电量:只画图标(用户要求去掉百分比文字)。电量无效时画空槽。
static void hud_draw_battery(const chick_canvas_t *c) {
    chick_fill_rect(c, BATTERY_BODY_X + BATTERY_BODY_W, BATTERY_BODY_Y + 4, 3, 5,
                    chick_rgb565(APP_INK));
    chick_fill_rect(c, BATTERY_BODY_X, BATTERY_BODY_Y, BATTERY_BODY_W, BATTERY_BODY_H,
                    chick_rgb565(APP_BAR_BG));
    chick_stroke_rect(c, BATTERY_BODY_X, BATTERY_BODY_Y, BATTERY_BODY_W, BATTERY_BODY_H,
                      BATTERY_BORDER, chick_rgb565(APP_INK));
    if (s_battery_soc > 0) {
        int w = (BATTERY_FILL_W * s_battery_soc) / 100;
        if (w < 1) w = 1;
        chick_fill_rect(c, BATTERY_BODY_X + BATTERY_BORDER, BATTERY_BODY_Y + BATTERY_BORDER,
                        w, BATTERY_FILL_H, chick_rgb565(APP_BAT_GOOD));
    }
}

static void hud_draw_level(const chick_canvas_t *c) {
    char text[24];
    snprintf(text, sizeof(text), CHICK_TEXT_LEVEL_FMT, (unsigned)s_run.level);
    chick_draw_text(c, LEVEL_LABEL_X, LEVEL_LABEL_Y, FONT_TINY, chick_rgb565(APP_INK), text);
}

// 草地上的进度条:半透明轨道 + 暖色填充。
// 每一帧的底色都是 world_redraw_rect() 刚补好的纯草地,所以 alpha 混合是确定的,
// 不会一帧叠一帧越来越黑。
static void hud_draw_progress(const chick_canvas_t *c) {
    chick_fill_rect_blend(c, PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H,
                          chick_rgb565(APP_INK), APP_TRACK_ALPHA);
    const uint16_t width = progress_width();
    if (width == 0) return;
    chick_fill_rect_blend(c, PROGRESS_X + PROGRESS_BORDER, PROGRESS_Y + PROGRESS_BORDER,
                          width, PROGRESS_FILL_H,
                          chick_rgb565(APP_PANEL_INK), APP_FILL_ALPHA);
}

static void hud_draw_hearts(const chick_canvas_t *c) {
    for (int i = 0; i < CHICK_RUN_LEVEL_LIVES; i++) {
        const bool alive = (int)s_run.lives > i;
        canvas_blit_asset(c, alive ? &s_assets[CHICK_SPRITE_HEART_FULL]
                                   : &s_assets[CHICK_SPRITE_HEART_EMPTY],
                          HEARTS_X + i * (HEART_ART_W + HEARTS_GAP), HEARTS_Y);
    }
}

typedef enum { HUD_SLOT_LEVEL, HUD_SLOT_PROGRESS, HUD_SLOT_METERS, HUD_SLOT_PICKUPS,
               HUD_SLOT_HEARTS, HUD_SLOT_BATTERY, HUD_SLOT_FPS, HUD_SLOT_COUNT } hud_slot_t;

static rect_t hud_slot_rect(hud_slot_t slot) {
    switch (slot) {
        case HUD_SLOT_LEVEL: return hud_rect_level();
        case HUD_SLOT_PROGRESS: return hud_rect_progress();
        case HUD_SLOT_METERS: return hud_rect_meters();
        case HUD_SLOT_PICKUPS: return hud_rect_pickups();
        case HUD_SLOT_HEARTS: return hud_rect_hearts();
        case HUD_SLOT_BATTERY: return hud_rect_battery();
        default: return hud_rect_fps();
    }
}

static void hud_draw_slot(const chick_canvas_t *c, hud_slot_t slot) {
    switch (slot) {
        case HUD_SLOT_LEVEL: hud_draw_level(c); break;
        case HUD_SLOT_PROGRESS: hud_draw_progress(c); break;
        case HUD_SLOT_METERS: hud_draw_meters(c); break;
        case HUD_SLOT_PICKUPS: hud_draw_pickups(c); break;
        case HUD_SLOT_HEARTS: hud_draw_hearts(c); break;
        case HUD_SLOT_BATTERY: hud_draw_battery(c); break;
        default: hud_draw_fps(c); break;
    }
}

// 本帧该画哪些 HUD 槽位(模式决定),把槽位下标写入 out 并返回个数。
// hud_draw() 与世界层的碰撞保护共用这一份判断,不会出现"两处过滤不一样"。
static int hud_visible_slots(hud_slot_t *out, int max) {
    int n = 0;
    for (int slot = 0; slot < HUD_SLOT_COUNT && n < max; slot++) {
        // 首页/设置页只显示电量图标(用户要求:不显示 FPS/分数/进度/血量)。
        if (s_screen != CHICK_SCREEN_GAME && slot != HUD_SLOT_BATTERY) continue;
        // 板上没有电量计时整条电量槽都不出现(S3R8 板):与其画一个永远空的图标,
        // 不如不要占位置。有电量计但偶尔读失败时图标仍在,只是格子空着。
        if (slot == HUD_SLOT_BATTERY && !s_battery_present) continue;
        // 关卡号 / 进度条 / 命数爱心只在关卡模式。
        if (s_mode != CHICK_RUN_MODE_LEVEL &&
            (slot == HUD_SLOT_LEVEL || slot == HUD_SLOT_PROGRESS ||
             slot == HUD_SLOT_HEARTS)) {
            continue;
        }
        out[n++] = (hud_slot_t)slot;
    }
    return n;
}

// 世界层的擦除/重贴矩形碰到 HUD 带子时,把 HUD 标脏(见 world_draw)。
static void hud_mark_if_touched(const chick_rect_t *rects, int count) {
    if (s_screen != CHICK_SCREEN_GAME) return;   // 页面整页重画,不靠这条路径
    hud_slot_t visible[HUD_SLOT_COUNT];
    const int n = hud_visible_slots(visible, HUD_SLOT_COUNT);
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < n; k++) {
            const rect_t r = hud_slot_rect(visible[k]);
            if (chick_rect_overlap(&rects[i], &r)) {
                s_hud_pending = true;
                return;
            }
        }
    }
}

static hud_sig_t hud_signature(void) {
    const hud_sig_t sig = {
        .score_m = s_run.score_m, .level = s_run.level, .lives = s_run.lives,
        .progress_w = progress_width(), .fps = s_fps_value, .battery = s_battery_soc,
        .coins = s_run.coins, .gems = s_run.gems, .notes = s_run.notes,
        .boosts = s_run.boosts, .boost_s = (uint8_t)chick_run_boost_seconds(&s_run),
    };
    return sig;
}

static bool hud_changed(const hud_sig_t *a, const hud_sig_t *b) {
    return a->score_m != b->score_m || a->level != b->level || a->lives != b->lives ||
           a->progress_w != b->progress_w || a->fps != b->fps || a->battery != b->battery ||
           a->coins != b->coins || a->gems != b->gems || a->notes != b->notes ||
           a->boosts != b->boosts || a->boost_s != b->boost_s;
}

static void hud_draw(const chick_canvas_t *c) {
    const hud_sig_t sig = hud_signature();
    const bool content_changed = s_hud_force || hud_changed(&sig, &s_hud_last);
    if (content_changed) s_hud_pending = true;
    if (!s_hud_pending) return;
    hud_slot_t slots[HUD_SLOT_COUNT];
    const int count = hud_visible_slots(slots, HUD_SLOT_COUNT);
    for (int i = 0; i < count; i++) {
        const rect_t r = hud_slot_rect(slots[i]);
        if (s_screen == CHICK_SCREEN_GAME) world_redraw_rect(c, &r);
        else bg_fill_rect(c, &r);
        hud_draw_slot(c, slots[i]);
        damage_add(r.x, r.y, r.w, r.h);
    }
    s_hud_last = sig;
    s_hud_force = false;
    s_hud_pending = false;
}

// 结算页的道具明细:图标 + "xN",整体居中。什么都没吃到时什么都不画。
// 竖屏只有 240px 宽,四个徽章一行放不下,所以每行两个、最多两行。
static void overlay_draw_rewards(const chick_canvas_t *c, int cx, int y) {
    char texts[PICKUP_BADGE_COUNT][16];
    int widths[PICKUP_BADGE_COUNT];
    int index[PICKUP_BADGE_COUNT];
    int count = 0;
    for (int i = 0; i < PICKUP_BADGE_COUNT; i++) {
        widths[i] = 0;
        if (!pickup_badge_text(i, texts[i], sizeof(texts[i]))) continue;
        const int icon_w = s_assets[PICKUP_BADGE_SPRITES[i]].w;
        widths[i] = icon_w + PICKUP_ICON_TEXT_GAP + chick_text_measure(FONT_TINYER, texts[i]).width;
        index[count++] = i;
    }
    if (count == 0) return;
    for (int start = 0; start < count; start += PICKUP_BADGE_PER_ROW) {
        const int row = start / PICKUP_BADGE_PER_ROW;
        const int end = start + PICKUP_BADGE_PER_ROW < count ? start + PICKUP_BADGE_PER_ROW : count;
        int row_w = PICKUP_BADGE_GAP * (end - start - 1);
        for (int k = start; k < end; k++) row_w += widths[index[k]];
        int x = cx - row_w / 2;
        for (int k = start; k < end; k++) {
            const int i = index[k];
            const int ry = y + row * PICKUP_BADGE_ROW_H;
            const sprite_asset_t *icon = &s_assets[PICKUP_BADGE_SPRITES[i]];
            canvas_blit_asset(c, icon, x, ry);
            chick_draw_text(c, x + icon->w + PICKUP_ICON_TEXT_GAP, pickup_text_y(icon->h, ry),
                            FONT_TINYER, chick_rgb565(APP_PANEL_INK), texts[i]);
            x += widths[i] + PICKUP_BADGE_GAP;
        }
    }
}

// --- 弹层 -------------------------------------------------------------------
static void overlay_draw(const chick_canvas_t *c) {
    const rect_t r = s_overlay_rect;
    chick_fill_rect_blend(c, r.x, r.y, r.w, r.h, chick_rgb565(APP_PANEL), 230);
    chick_stroke_rect(c, r.x, r.y, r.w, r.h, 3, chick_rgb565(APP_ACCENT));
    const int cx = r.x + r.w / 2;
    const int y = r.y + 10;
    switch (s_overlay) {
        case OVERLAY_PAUSE:
            chick_draw_text_center(c, cx, y, FONT_TINY, chick_rgb565(APP_PANEL_INK), CHICK_TEXT_PAUSE);
            chick_draw_text_center(c, cx, y + 46, FONT_TINY, chick_rgb565(APP_PANEL_INK), CHICK_TEXT_PAUSE_HINT);
            chick_draw_text_center(c, cx, y + 76, FONT_TINY, chick_rgb565(APP_ACCENT), CHICK_TEXT_BACK_HINT);
            break;
        case OVERLAY_GAME_OVER:
            // 结算比暂停多一行道具明细(本局吃到多少),所以行距重新排一下。
            chick_draw_text_center(c, cx, r.y + 10, FONT_TINY, chick_rgb565(APP_PANEL_INK),
                                   CHICK_TEXT_GAME_OVER);
            chick_draw_text_center(c, cx, r.y + 42, FONT_TINY, chick_rgb565(APP_PANEL_INK),
                                   s_overlay_body);
            overlay_draw_rewards(c, cx, r.y + 98);
            chick_draw_text_center(c, cx, r.y + 156, FONT_TINY, chick_rgb565(APP_ACCENT),
                                   s_overlay_hint);
            break;
        case OVERLAY_LEVEL_CLEAR:
            chick_draw_text_center(c, cx, y, FONT_TINY, chick_rgb565(APP_PANEL_INK), CHICK_TEXT_LEVEL_CLEAR);
            chick_draw_text_center(c, cx, y + 46, FONT_TINY, chick_rgb565(APP_PANEL_INK), s_overlay_body);
            chick_draw_text_center(c, cx, y + 76, FONT_TINY, chick_rgb565(APP_ACCENT), CHICK_TEXT_LEVEL_NEXT_HINT);
            break;
        default:
            break;
    }
    damage_add(r.x, r.y, r.w, r.h);
}

static void overlay_show(overlay_t kind, const char *body) {
    s_overlay = kind;
    // 结算页多了道具明细行,需要更高的面板;暂停/过关维持原来的尺寸。
    if (kind == OVERLAY_GAME_OVER) {
        // 结算面板整块放在草地上的关卡标签/进度条/爱心上方,不遮住它们。
        s_overlay_rect = (rect_t){ (LAYOUT_W - 210) / 2, 42, 210, 212 };
    } else {
        s_overlay_rect = (rect_t){ (LAYOUT_W - 190) / 2, (LAYOUT_H - 140) / 2, 190, 140 };
    }
    s_overlay_repair = true;
    s_hud_force = true;
    if (kind == OVERLAY_PAUSE) {
        snprintf(s_overlay_hint, sizeof(s_overlay_hint), "%s", CHICK_TEXT_BACK_HINT);
        s_overlay_body[0] = '\0';
    } else if (kind == OVERLAY_GAME_OVER) {
        snprintf(s_overlay_hint, sizeof(s_overlay_hint), "%s\n%s", CHICK_TEXT_AGAIN_HINT,
                 CHICK_TEXT_BACK_HINT);
    } else {
        s_overlay_hint[0] = '\0';
    }
    if (body != NULL) snprintf(s_overlay_body, sizeof(s_overlay_body), "%s", body);
}

// --- 页面 -------------------------------------------------------------------
static void decor_reset(void) {
    for (size_t i = 0; i < CLOUD_COUNT; i++) s_clouds[i].x_milli = (int32_t)(20 + i * 140) * 1000;
    for (size_t i = 0; i < BG_DECOR_COUNT; i++) {
        s_bg_decor[i].x_milli = (int32_t)(20 + i * BG_DECOR_SPACING) * 1000;
    }
    for (size_t i = 0; i < FG_DECOR_COUNT; i++) {
        s_fg_decor[i].x_milli = (int32_t)(i * FG_DECOR_SPACING) * 1000;
    }
}

static void draw_static_scenery(const chick_canvas_t *c) {
    bg_fill_rect(c, &(rect_t){ 0, 0, LAYOUT_W, LAYOUT_H });
    blit_source_indexed(c, CHICK_SPRITE_CHICK_RUN_A, HOME_CHICK_X, GROUND_Y + 10 - HERO_H,
                        HERO_SCALE_NUM, HERO_SCALE_DEN, NULL);
    blit_source_indexed(c, CHICK_SPRITE_CACTUS, HOME_CACTUS_X, GROUND_Y + 10 - HERO_CACTUS_H,
                        HERO_SCALE_NUM, HERO_SCALE_DEN, NULL);
    damage_add(0, 0, LAYOUT_W, LAYOUT_H);
}

static void draw_home(const chick_canvas_t *c) {
    draw_static_scenery(c);
    chick_draw_text_center(c, LAYOUT_W / 2, HOME_TITLE_Y, FONT_CJK, chick_rgb565(APP_INK), CHICK_TEXT_TITLE);
    char text[40];
    snprintf(text, sizeof(text), CHICK_TEXT_MODE_FMT,
             s_mode == CHICK_RUN_MODE_LEVEL ? CHICK_TEXT_MODE_LEVEL : CHICK_TEXT_MODE_FREE);
    chick_draw_text_center(c, LAYOUT_W / 2, HOME_MODE_Y, FONT_TINY, chick_rgb565(APP_INK), text);
    if (s_mode == CHICK_RUN_MODE_LEVEL) {
        snprintf(text, sizeof(text), CHICK_TEXT_LEVEL_BEST_FMT, (unsigned)s_run.best_level);
    } else {
        snprintf(text, sizeof(text), CHICK_TEXT_BEST_FMT, (unsigned)s_run.best_m);
    }
    chick_draw_text_center(c, LAYOUT_W / 2, HOME_BEST_Y, FONT_TINY, chick_rgb565(APP_INK_SOFT), text);
    chick_draw_text_center(c, LAYOUT_W / 2, HOME_START_Y, FONT_TINY, chick_rgb565(APP_ACCENT), CHICK_TEXT_START_HINT);
    chick_draw_text_center(c, LAYOUT_W / 2, HOME_SETTINGS_Y, FONT_TINY, chick_rgb565(APP_INK), CHICK_TEXT_SETTINGS_HINT);
    // 新玩法提示:空中的金币/宝石/音符要跳起来吃。
    chick_draw_text_center(c, LAYOUT_W / 2, HOME_PICKUP_Y, FONT_TINYER, chick_rgb565(APP_INK_SOFT),
                           CHICK_TEXT_PICKUP_HINT);
    chick_draw_text_center(c, LAYOUT_W / 2, GROUND_Y + 22, FONT_TINY, chick_rgb565(APP_INK), CHICK_TEXT_CONTROLS);
    hud_draw(c);
}

static uint8_t settings_value_of(uint8_t row) {
    switch (row) {
        case SETTINGS_ROW_VOLUME: return s_settings.volume;
        case SETTINGS_ROW_WARMTH: return s_settings.warmth;
        default: return s_settings.brightness;
    }
}

static void draw_settings_row(const chick_canvas_t *c, uint8_t row, const char *label, int y) {
    const bool active = row == s_settings_row;
    const uint32_t ink = active ? APP_ACCENT : APP_INK;
    chick_draw_text(c, SETTINGS_LABEL_X, y, FONT_TINY, chick_rgb565(ink), label);
    chick_fill_rect(c, SETTINGS_BAR_X, y + SETTINGS_BAR_Y_OFF, SETTINGS_BAR_W, SETTINGS_BAR_H,
                    chick_rgb565(APP_BAR_BG));
    chick_stroke_rect(c, SETTINGS_BAR_X, y + SETTINGS_BAR_Y_OFF, SETTINGS_BAR_W, SETTINGS_BAR_H,
                      SETTINGS_BAR_BORDER, chick_rgb565(ink));
    const uint8_t value = settings_value_of(row);
    int width = (SETTINGS_FILL_W * value) / 100;
    if (value > 0 && width < 1) width = 1;
    chick_fill_rect(c, SETTINGS_BAR_X + SETTINGS_BAR_BORDER,
                    y + SETTINGS_BAR_Y_OFF + SETTINGS_BAR_BORDER, width, SETTINGS_FILL_H,
                    chick_rgb565(APP_BAR_FILL));
    char text[16];
    snprintf(text, sizeof(text), CHICK_TEXT_PERCENT_FMT, (unsigned)value);
    chick_draw_text_right(c, SETTINGS_VALUE_X + SETTINGS_VALUE_W, y + SETTINGS_VALUE_Y_OFF,
                          FONT_TINY, chick_rgb565(APP_INK), text);
}

static void draw_settings(const chick_canvas_t *c) {
    draw_static_scenery(c);
    chick_draw_text_center(c, LAYOUT_W / 2, SETTINGS_TITLE_Y, FONT_CJK, chick_rgb565(APP_INK), CHICK_TEXT_SETTINGS);
    draw_settings_row(c, SETTINGS_ROW_BRIGHTNESS, CHICK_TEXT_SETTINGS_BRIGHT, SETTINGS_ROW0_Y);
    draw_settings_row(c, SETTINGS_ROW_VOLUME, CHICK_TEXT_SETTINGS_VOLUME, SETTINGS_ROW1_Y);
    draw_settings_row(c, SETTINGS_ROW_WARMTH, CHICK_TEXT_SETTINGS_WARM, SETTINGS_ROW2_Y);
    chick_draw_text_center(c, LAYOUT_W / 2, GROUND_Y + 22, FONT_TINY, chick_rgb565(APP_INK), CHICK_TEXT_SETTINGS_HELP);
    hud_draw(c);
}

// --- 设置应用 ---------------------------------------------------------------
// 屏幕色温补偿:设置一变就重建查表;推屏时每像素查一次(见表很小,代价可忽略)。
static chick_color_lut_t s_tint_lut;
static bool s_tint_ready;

static void tint_rebuild(void) {
    const chick_color_gain_t gain = chick_color_gain_from_warmth(s_settings.warmth);
    chick_color_lut_build(&gain, &s_tint_lut);
    s_tint_ready = true;
    ESP_LOGI(TAG, "色温补偿 暖度 %u%%,增益 R%d G%d B%d (Q8,256=1.0)",
             (unsigned)s_settings.warmth, gain.r, gain.g, gain.b);
}

static void settings_apply(void) {
    if (s_backlight_on) bsp_display_backlight(s_settings.brightness);
    bsp_audio_set_volume(s_settings.volume);
    tint_rebuild();
}

// --- 页面切换 ---------------------------------------------------------------
static void scene_state_reset(void) {
    memset(&s_scene, 0, sizeof(s_scene));
    s_scene.bg_filled = false;
    s_overlay_repair = false;
}

static void page_invalidate(void) {
    s_page_drawn = false;
    s_hud_pending = true;
}

static void start_run(void) {
    chick_run_start(&s_run, s_mode);
    sound_play(SFX_START);
    decor_reset();
    scene_state_reset();
    s_overlay = OVERLAY_NONE;
    s_hud_force = true;
    s_flash_until_ms = 0;
    s_screen = CHICK_SCREEN_GAME;
    memset(&s_hud_last, 0xFF, sizeof(s_hud_last));
}

static void back_to_title(void) {
    chick_run_abort(&s_run);
    s_overlay = OVERLAY_NONE;
    s_screen = CHICK_SCREEN_HOME;
    s_flash_until_ms = 0;
    page_invalidate();
}

static void settings_open(void) {
    page_invalidate();
    s_settings_row = SETTINGS_ROW_BRIGHTNESS;
    s_screen = CHICK_SCREEN_SETTINGS;
}

static void settings_close(void) {
    if (chick_run_store_save_settings(&s_settings) != ESP_OK) {
        ESP_LOGW(TAG, "设置未保存,重启后恢复旧值");
    }
    back_to_title();
}

static void game_over_show(void) {
    char body[96];
    if (s_mode == CHICK_RUN_MODE_LEVEL) {
        // 关卡模式:第几关 + 整局总得分(道具加分含在内)。
        snprintf(body, sizeof(body), CHICK_TEXT_LEVEL_RESULT_FMT, (unsigned)s_run.level,
                 (unsigned)chick_run_total_score(&s_run));
        if (s_run.best_level > s_best_level_saved &&
            chick_run_store_save_best_level(s_run.best_level) == ESP_OK) {
            s_best_level_saved = s_run.best_level;
        }
    } else if (chick_run_is_new_best(&s_run)) {
        snprintf(body, sizeof(body), CHICK_TEXT_RECORD_FMT,
                 (unsigned)chick_run_total_score(&s_run));
    } else {
        // 综合得分只在结算页出现一次:距离 + 道具加分都在里面。
        snprintf(body, sizeof(body), CHICK_TEXT_THIS_RUN_FMT,
                 (unsigned)chick_run_total_score(&s_run));
    }
    if (s_mode == CHICK_RUN_MODE_FREE && s_run.best_m > s_best_saved &&
        chick_run_store_save_best(s_run.best_m) == ESP_OK) {
        s_best_saved = s_run.best_m;
    }
    overlay_show(OVERLAY_GAME_OVER, body);
    ESP_LOGI(TAG, "本局 %u 分(%u 米 + 道具 %u),最高 %u 分,金币 %u 宝石 %u 音符 %u",
             (unsigned)chick_run_total_score(&s_run), (unsigned)s_run.score_m,
             (unsigned)chick_run_bonus_points(&s_run), (unsigned)s_run.best_m,
             (unsigned)s_run.coins, (unsigned)s_run.gems, (unsigned)s_run.notes);
}

static void title_mode_switch(int32_t delta) {
    page_invalidate();
    int32_t next = (int32_t)s_mode + delta;
    if (next < 0) next = CHICK_RUN_MODE_COUNT - 1;
    if (next >= CHICK_RUN_MODE_COUNT) next = 0;
    s_mode = (chick_run_mode_t)next;
    (void)chick_run_store_save_mode((uint8_t)s_mode);
}

static void settings_adjust(int32_t steps) {
    page_invalidate();
    switch (s_settings_row) {
        case SETTINGS_ROW_VOLUME:
            s_settings.volume = chick_run_settings_step(s_settings.volume, steps,
                                                       CHICK_RUN_VOLUME_MIN, CHICK_RUN_VOLUME_MAX);
            break;
        case SETTINGS_ROW_WARMTH:
            s_settings.warmth = chick_run_settings_step(s_settings.warmth, steps,
                                                       CHICK_RUN_WARMTH_MIN, CHICK_RUN_WARMTH_MAX);
            break;
        default:
            s_settings.brightness = chick_run_settings_step(s_settings.brightness, steps,
                                                            CHICK_RUN_BRIGHTNESS_MIN,
                                                            CHICK_RUN_BRIGHTNESS_MAX);
            break;
    }
    settings_apply();
}

// --- 输入 -------------------------------------------------------------------
static void handle_key(const chick_key_t *key) {
    s_last_input_ms = now_ms();
    if (!s_backlight_on) {
        if (!s_first_frame_pushed) return;   // 还没亮屏:这期间的按键不提前点亮
        bsp_display_backlight(s_settings.brightness);
        s_backlight_on = true;
        rtttl_player_set_bgm(BGM_MAIN);
        return;
    }
    if (s_screen == CHICK_SCREEN_SETTINGS) {
        // 按键库按住期间会重复上报 PRESS，而设置页按 PRESS 即时生效 → 必须去抖。
        const bool debounced = (uint32_t)(now_ms() - s_settings_press_ms) < 150;
#if BSP_BTN_HAS_B
        // 六键板(上/下/左/右/A/B):上下切换项目,左右调整数值,A/B 都是返回。
        if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_PRESS) {
            s_settings_row = (uint8_t)((s_settings_row + SETTINGS_ROW_COUNT - 1U) %
                                      SETTINGS_ROW_COUNT);
            page_invalidate();
        } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_PRESS) {
            s_settings_row = (uint8_t)((s_settings_row + 1U) % SETTINGS_ROW_COUNT);
            page_invalidate();
        } else if (key->btn == BSP_BTN_LEFT && key->ev == BSP_BTN_PRESS && !debounced) {
            s_settings_press_ms = now_ms();
            settings_adjust(-1);
        } else if (key->btn == BSP_BTN_RIGHT && key->ev == BSP_BTN_PRESS && !debounced) {
            s_settings_press_ms = now_ms();
            settings_adjust(+1);
        } else if (key->btn == BSP_BTN_LEFT && key->ev == BSP_BTN_LONG) {
            settings_adjust(-CHICK_RUN_SETTINGS_FAST_STEPS);
        } else if (key->btn == BSP_BTN_RIGHT && key->ev == BSP_BTN_LONG) {
            settings_adjust(+CHICK_RUN_SETTINGS_FAST_STEPS);
        } else if ((key->btn == BSP_BTN_A || key->btn == BSP_BTN_B) &&
                   key->ev == BSP_BTN_CLICK) {
            settings_close();          // A/B 都是返回(设置已经即时生效,不需要单独保存键)
        }
#else
        // 没有左右键时:BSP_BTN_HAS_B 为 0,上下调整数值,确定切换项目,长按确定返回。
        if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_PRESS && !debounced) {
            s_settings_press_ms = now_ms();
            settings_adjust(+1);
        } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_PRESS && !debounced) {
            s_settings_press_ms = now_ms();
            settings_adjust(-1);
        } else if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_LONG) {
            settings_adjust(+CHICK_RUN_SETTINGS_FAST_STEPS);
        } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_LONG) {
            settings_adjust(-CHICK_RUN_SETTINGS_FAST_STEPS);
        } else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_PRESS) {
            s_settings_row = (uint8_t)((s_settings_row + 1U) % SETTINGS_ROW_COUNT);
            page_invalidate();
        } else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_LONG) {
            settings_close();
        }
#endif
        return;
    }
    if (s_screen == CHICK_SCREEN_HOME) {
        if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_PRESS) title_mode_switch(-1);
        else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_PRESS) title_mode_switch(+1);
        else if (key->btn == BSP_BTN_LEFT && key->ev == BSP_BTN_PRESS) title_mode_switch(-1);
        else if (key->btn == BSP_BTN_RIGHT && key->ev == BSP_BTN_PRESS) title_mode_switch(+1);
        else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_CLICK) start_run();
        else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_LONG) settings_open();
        else if (key->btn == BSP_BTN_B && key->ev == BSP_BTN_CLICK) settings_open();
        return;
    }
    switch (s_run.state) {
        case CHICK_STATE_RUNNING:
            if (key->btn == BSP_BTN_UP && key->ev == BSP_BTN_PRESS) {
                if (chick_run_jump(&s_run)) sound_play(SFX_JUMP);
            } else if (key->btn == BSP_BTN_UP &&
                       (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
                chick_run_jump_release(&s_run);
            } else if (key->btn == BSP_BTN_DOWN && key->ev == BSP_BTN_PRESS) {
                chick_run_slide(&s_run);
                sound_play(SFX_SLIDE);
#if BSP_BTN_HAS_B
            } else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_PRESS) {
                // 六键板:A 与上键同为跳跃,暂停交给 B。
                if (chick_run_jump(&s_run)) sound_play(SFX_JUMP);
            } else if (key->btn == BSP_BTN_A &&
                       (key->ev == BSP_BTN_CLICK || key->ev == BSP_BTN_LONG)) {
                chick_run_jump_release(&s_run);
            } else if (key->btn == BSP_BTN_B && key->ev == BSP_BTN_CLICK) {
                if (chick_run_toggle_pause(&s_run)) overlay_show(OVERLAY_PAUSE, NULL);
#else
            } else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_CLICK) {
                // 没有 B 键时的降级:确定键兼任暂停。
                if (chick_run_toggle_pause(&s_run)) overlay_show(OVERLAY_PAUSE, NULL);
#endif
            }
            break;
        case CHICK_STATE_PAUSED:
            if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_CLICK) {
                if (chick_run_toggle_pause(&s_run)) {
                    s_overlay = OVERLAY_NONE;
                    s_overlay_repair = true;
                }
            } else if (key->btn == BSP_BTN_B && key->ev == BSP_BTN_CLICK) {
                back_to_title();           // 六键板:B 键直接从暂停回到首页
            } else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_LONG) {
                back_to_title();           // 没有 B 键时只能长按确定回首页
            }
            break;
        case CHICK_STATE_GAME_OVER:
            if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_CLICK) start_run();
            else if (key->btn == BSP_BTN_A && key->ev == BSP_BTN_LONG) back_to_title();
            else if (key->btn == BSP_BTN_B && key->ev == BSP_BTN_CLICK) back_to_title();
            break;
        default:
            break;
    }
}

// --- 帧循环 -----------------------------------------------------------------
static void decor_view_update(decor_item_t *items, size_t count, int32_t spacing,
                              int32_t advance_milli, int32_t wrap_milli) {
    for (size_t i = 0; i < count; i++) {
        items[i].x_milli -= advance_milli;
        if (items[i].x_milli < -(int32_t)spacing * 1000) items[i].x_milli += wrap_milli;
    }
}

static void clouds_view_update(int32_t advance_milli) {
    for (size_t i = 0; i < CLOUD_COUNT; i++) {
        s_clouds[i].x_milli -= advance_milli;
        if (s_clouds[i].x_milli < -CLOUD_CANVAS_W * 1000) {
            s_clouds[i].x_milli += (LAYOUT_W + 100) * 1000;
        }
    }
}

static void housekeeping(uint32_t time_ms) {
    int32_t idle_delta = (int32_t)(time_ms - s_last_input_ms);
    if (idle_delta < 0) idle_delta = 0;
    const uint32_t idle_ms = (uint32_t)idle_delta;
    if (s_run.state == CHICK_STATE_RUNNING && idle_ms > CHICK_IDLE_PAUSE_MS) {
        if (chick_run_toggle_pause(&s_run)) {
            overlay_show(OVERLAY_PAUSE, NULL);
            ESP_LOGI(TAG, "空闲 %u ms,自动暂停", (unsigned)idle_ms);
        }
    }
    if (s_backlight_on && idle_ms > CHICK_IDLE_DIM_MS) {
        rtttl_player_stop();
        bsp_display_backlight(0);
        s_backlight_on = false;
        ESP_LOGI(TAG, "空闲 %u ms,关闭背光", (unsigned)idle_ms);
    }
    if (s_battery_present &&
        (s_battery_next_ms == 0 || (int32_t)(time_ms - s_battery_next_ms) >= 0)) {
        const int soc = bsp_battery_soc();
        if (soc != s_battery_soc) {
            s_battery_soc = soc;
            s_hud_pending = true;
        }
        s_battery_next_ms = time_ms + CHICK_BATTERY_PERIOD_MS;
    }
    if (s_battery_present &&
        (s_power_log_next_ms == 0 || (int32_t)(time_ms - s_power_log_next_ms) >= 0)) {
        ESP_LOGI(TAG, "电量 %d%%,电压 %d mV", bsp_battery_soc(), bsp_battery_mv());
        s_power_log_next_ms = time_ms + CHICK_POWER_LOG_MS;
    }
}

static void render_frame(uint32_t dt_ms, uint32_t time_ms) {
    const chick_canvas_t c = s_canvas;
    if (s_screen == CHICK_SCREEN_GAME) {
        const bool was_running = s_run.state == CHICK_STATE_RUNNING;
        const uint32_t events = chick_run_tick(&s_run, dt_ms);
        if (was_running) {
            const int32_t advance = s_run.speed * (int32_t)dt_ms;
            decor_view_update(s_bg_decor, BG_DECOR_COUNT, BG_DECOR_SPACING,
                              advance / 3, BG_DECOR_COUNT * BG_DECOR_SPACING * 1000);
            decor_view_update(s_fg_decor, FG_DECOR_COUNT, FG_DECOR_SPACING,
                              advance, FG_DECOR_COUNT * FG_DECOR_SPACING * 1000);
            clouds_view_update(advance / 5);
        }
        scene_collect();
        const uint32_t t0 = now_us();
        world_draw(&c);
        const uint32_t world_us = now_us() - t0;
        s_world_us_total += world_us;
        if (world_us > s_world_us_max) s_world_us_max = world_us;
        hud_draw(&c);
        if (s_overlay != OVERLAY_NONE) {
            world_redraw_rect(&c, &s_overlay_rect);
            overlay_draw(&c);
        }
        // 道具:先出声再处理结算 —— 同一帧里"吃到金币"和"游戏结束"同时发生时,
        // 结束音(高优先级)会把金币声顶掉,这是对的。
        if (events & CHICK_EV_COIN) sound_play(SFX_COIN);
        if (events & CHICK_EV_GEM) sound_play(SFX_GEM);
        if (events & CHICK_EV_NOTE) {
            // 音乐道具:两首世界名曲随机二选一(《小星星》/《欢乐颂》),都约 5.8 秒。
            sound_play_priority((esp_random() & 1u) != 0u ? SFX_TWINKLE : SFX_NOTE);
        }
        if (events & CHICK_EV_BOOST) sound_play_voice(SFX_BOOST, RTTTL_VOICE_TREMOLO);
        if (events & CHICK_EV_SMASH) {
            // 不另发撞击声:加速期间"冲刺"旋律走高优先级,再插一个短音只会把它切碎。
            ESP_LOGD(TAG, "撞碎第 %u 个障碍", (unsigned)chick_run_smashed(&s_run));
        }
        if (events & CHICK_EV_MILESTONE) {
            s_flash_until_ms = time_ms + CHICK_FLASH_MS;
            s_hud_force = true;
            sound_play(SFX_MILE);
        }
        if (events & CHICK_EV_NEW_BEST) sound_play_priority(SFX_RECORD);
        if (events & CHICK_EV_HURT) {
            sound_play(SFX_HURT);
            ESP_LOGI(TAG, "掉一条命,剩余 %u", (unsigned)s_run.lives);
        }
        if (events & CHICK_EV_LEVEL_UP) {
            sound_play_priority(SFX_LEVEL_UP);
            (void)chick_run_toggle_pause(&s_run);
            char body[24];
            snprintf(body, sizeof(body), CHICK_TEXT_LEVEL_FMT, (unsigned)s_run.level);
            overlay_show(OVERLAY_LEVEL_CLEAR, body);
        }
        if (events & CHICK_EV_GAME_OVER) {
            sound_play_priority(SFX_OVER);
            game_over_show();
        }
        if (s_flash_until_ms != 0 && (int32_t)(time_ms - s_flash_until_ms) >= 0) {
            s_flash_until_ms = 0;
            s_hud_force = true;
        }
    } else if (!s_page_drawn || s_hud_pending) {
        if (s_screen == CHICK_SCREEN_SETTINGS) draw_settings(&c);
        else draw_home(&c);
        s_page_drawn = true;
    }
}

// --- 推送:只把脏区通过 SPI 发给面板 ---------------------------------------
static bool IRAM_ATTR on_flush_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata, void *ctx) {
    (void)io; (void)edata; (void)ctx;
    BaseType_t need_yield = pdFALSE;
    if (s_flush_done != NULL) xSemaphoreGiveFromISR(s_flush_done, &need_yield);
    return need_yield == pdTRUE;
}

static void flush_rect(int x, int y, int w, int h) {
    const uint32_t t0 = now_us();
    for (int row = 0; row < h; row += STAGE_ROWS) {
        const int rows = (h - row) > STAGE_ROWS ? STAGE_ROWS : (h - row);
        uint16_t *dst = s_stage;
        // ST7789 走 SPI 要【大端】RGB565,而画布是小端 → 拷贝时交换高低字节
        // (LVGL 那条路径是靠 esp_lvgl_port 的 swap_bytes 做的,这里自己做)。
        for (int r = 0; r < rows; r++) {
            const uint16_t *src = s_fb + (size_t)(y + row + r) * LAYOUT_W + x;
            for (int i = 0; i < w; i++) {
                // 先做色温补偿(纠正面板偏冷),再交换高低字节给 ST7789。
                uint16_t px = src[i];
                if (s_tint_ready) px = chick_color_apply_lut(px, &s_tint_lut);
                dst[i] = (uint16_t)((px >> 8) | (px << 8));
            }
            dst += w;
        }
        esp_lcd_panel_draw_bitmap(bsp_display_panel(), x, y + row, x + w, y + row + rows, s_stage);
        (void)xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
        s_flush_calls++;
    }
    s_flush_px += (uint32_t)w * (uint32_t)h;
    s_flush_us_total += now_us() - t0;
}

// 屏幕四角遮罩:只有圆角屏(半径 ≥ 1)需要把圆角外涂黑。
// 本板 BSP_LCD_CORNER_RADIUS = 0(直角屏),直接跳过 —— 用户要求"屏幕四周
// 不要黑色圆角",所以这里不做任何遮罩,四角就是应用自己画的画面。
static void mask_corners(void) {
    const int radius = BSP_LVGL_SCREEN_RADIUS;
    if (radius <= 0) return;
    for (int y = 0; y < LAYOUT_H; y++) {
        int32_t x1 = 0, x2 = LAYOUT_W - 1;
        if (!bsp_display_rounded_row_span(y, LAYOUT_W, LAYOUT_H, radius, &x1, &x2)) continue;
        if (x1 == 0 && x2 == LAYOUT_W - 1) continue;          // 中间行不受圆角影响
        for (int x = 0; x < x1; x++) s_fb[(size_t)y * LAYOUT_W + x] = 0;
        for (int x = x2 + 1; x < LAYOUT_W; x++) s_fb[(size_t)y * LAYOUT_W + x] = 0;
    }
}

static void damage_flush(void) {
    if (s_damage_n == 0) return;
    for (int i = 0; i < s_damage_n; i++) {
        const rect_t r = s_damage[i];
        flush_rect(r.x, r.y, r.w, r.h);
    }
    s_damage_n = 0;
}

static void stats_maybe_log(uint32_t time_ms) {
    if (s_stats_next_ms != 0 && (int32_t)(time_ms - s_stats_next_ms) < 0) return;
    const uint32_t frames = s_frames ? s_frames : 1u;
    ESP_LOGI(TAG, "帧统计 %u 帧 %.1f FPS | 世界 平均 %u.%u 最大 %u.%u ms | 推送 %u px/帧 "
                  "%u us/帧 %u 次/帧 | 堆 %u",
             (unsigned)s_frames, (double)s_frames * 1000.0 / (double)CHICK_STATS_MS,
             (unsigned)(s_world_us_total / frames / 1000),
             (unsigned)((s_world_us_total / frames / 100) % 10),
             (unsigned)(s_world_us_max / 1000), (unsigned)((s_world_us_max / 100) % 10),
             (unsigned)(s_flush_px / frames), (unsigned)(s_flush_us_total / frames),
             (unsigned)(s_flush_calls / frames),
             (unsigned)esp_get_free_heap_size());
    s_frames = 0;
    s_world_us_total = 0;
    s_world_us_max = 0;
    s_flush_px = 0;
    s_flush_us_total = 0;
    s_flush_calls = 0;
    s_stats_next_ms = time_ms + CHICK_STATS_MS;
}

#ifdef CHICK_RUN_SHOT
// 开发自检:自动走一遍页面,并把画布 4 倍降采样后 RLE 从串口回传(主机
// tools/fb_shot.py 解码成图片)。只用于开发看画面,不参与发布固件。
enum { SHOT_CMD_KEY = 1, SHOT_CMD_SHOT, SHOT_CMD_MODE, SHOT_CMD_MAX };
typedef struct { uint32_t at_ms; uint8_t kind; uint8_t a; uint8_t b; const char *label; } shot_cmd_t;
static const shot_cmd_t SHOT_CMDS[] = {
    {  2500, SHOT_CMD_MODE, CHICK_RUN_MODE_LEVEL, 0, NULL },
    {  3000, SHOT_CMD_SHOT, 0, 0, "01-home-level" },
    {  3500, SHOT_CMD_MODE, CHICK_RUN_MODE_FREE, 0, NULL },
    {  4000, SHOT_CMD_SHOT, 0, 0, "02-home-free" },
    {  4500, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_PRESS, NULL },
    {  4800, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_CLICK, NULL },
    // 自由模式跑动中跳几次:跳过障碍的那条抛物线正好扫过金币弧线,
    // 于是截图里能看到 HUD 的道具计数行(吃不到也不影响其他截图)。
    {  5200, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_PRESS, NULL },
    {  5500, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_CLICK, NULL },
    {  6300, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_PRESS, NULL },
    {  6600, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_CLICK, NULL },
    {  7400, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_PRESS, NULL },
    {  7700, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_CLICK, NULL },
    {  8000, SHOT_CMD_SHOT, 0, 0, "03-game" },
    {  9500, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_CLICK, NULL },
    { 11000, SHOT_CMD_SHOT, 0, 0, "04-pause" },
    { 12000, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_CLICK, NULL },
    { 13000, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_LONG, NULL },
    { 15000, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_LONG, NULL },
    { 16500, SHOT_CMD_MAX, 0, 0, NULL },
    { 17000, SHOT_CMD_SHOT, 0, 0, "05-settings" },
    { 18000, SHOT_CMD_KEY, BSP_BTN_DOWN, BSP_BTN_PRESS, NULL },
    { 19500, SHOT_CMD_SHOT, 0, 0, "06-settings-adjusted" },
    { 21000, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_LONG, NULL },
    { 23000, SHOT_CMD_SHOT, 0, 0, "07-home-again" },
    // 关卡模式的游戏内画面:关卡号 + 得分/米数行 + 底部草地上的进度条与爱心。
    { 23500, SHOT_CMD_MODE, CHICK_RUN_MODE_LEVEL, 0, NULL },
    { 24500, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_PRESS, NULL },
    { 25000, SHOT_CMD_KEY, BSP_BTN_OK, BSP_BTN_CLICK, NULL },
    { 26500, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_PRESS, NULL },
    { 26800, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_CLICK, NULL },
    { 28500, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_PRESS, NULL },
    { 28800, SHOT_CMD_KEY, BSP_BTN_UP, BSP_BTN_CLICK, NULL },
    { 30000, SHOT_CMD_SHOT, 0, 0, "08-game-level" },
    { 31400, SHOT_CMD_SHOT, 0, 0, "09-game-level-later" },
};
#define SHOT_CMDS_N (sizeof(SHOT_CMDS) / sizeof(SHOT_CMDS[0]))
static size_t s_shot_next;

static void shot_dump(const chick_canvas_t *c, const char *label) {
    enum { STEP = 2 };
    const int w = LAYOUT_W / STEP, h = LAYOUT_H / STEP;
    ESP_LOGW(TAG, "SHOT_BEGIN %s %d %d", label, w, h);
    char line[200];
    int n = 0;
    uint16_t prev = 0;
    uint32_t run = 0;
    for (int y = 0; y < LAYOUT_H; y += STEP) {
        for (int x = 0; x < LAYOUT_W; x += STEP) {
            uint32_t r = 0, g = 0, b = 0;
            for (int dy = 0; dy < STEP; dy++) {
                const uint16_t *rowp = c->px + (size_t)(y + dy) * c->stride_px + x;
                for (int dx = 0; dx < STEP; dx++) {
                    const uint16_t p = rowp[dx];
                    r += (p >> 11) & 0x1F; g += (p >> 5) & 0x3F; b += p & 0x1F;
                }
            }
            const uint32_t cnt = (uint32_t)(STEP * STEP);
            const uint16_t cur = (uint16_t)((((r / cnt) & 0x1F) << 11) |
                                            (((g / cnt) & 0x3F) << 5) | ((b / cnt) & 0x1F));
            if (run == 0) { prev = cur; run = 1; continue; }
            const int dr = (int)((cur >> 11) & 0x1F) - (int)((prev >> 11) & 0x1F);
            const int dg = (int)((cur >> 5) & 0x3F) - (int)((prev >> 5) & 0x3F);
            const int db = (int)(cur & 0x1F) - (int)(prev & 0x1F);
            if (dr >= -1 && dr <= 1 && dg >= -1 && dg <= 1 && db >= -1 && db <= 1) { run++; continue; }
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%X:%04X ", (unsigned)run, (unsigned)prev);
            if (n > (int)sizeof(line) - 12) { ESP_LOGW(TAG, "SHOT_DATA %s", line); n = 0; }
            prev = cur; run = 1;
        }
    }
    n += snprintf(line + n, sizeof(line) - (size_t)n, "%X:%04X ", (unsigned)run, (unsigned)prev);
    ESP_LOGW(TAG, "SHOT_DATA %s", line);
    ESP_LOGW(TAG, "SHOT_END %s", label);
}

static void shot_step(uint32_t time_ms) {
    if (s_shot_next >= SHOT_CMDS_N) return;
    const shot_cmd_t *cmd = &SHOT_CMDS[s_shot_next];
    if ((int32_t)(time_ms - cmd->at_ms) < 0) return;
    s_shot_next++;
    if (cmd->kind == SHOT_CMD_KEY) {
        const chick_key_t key = { (bsp_btn_t)cmd->a, (bsp_btn_ev_t)cmd->b };
        ESP_LOGW(TAG, "SHOT key %u %u", cmd->a, cmd->b);
        handle_key(&key);
    } else if (cmd->kind == SHOT_CMD_MODE) {
        s_mode = (chick_run_mode_t)cmd->a;
        page_invalidate();
        s_hud_force = true;
    } else if (cmd->kind == SHOT_CMD_MAX) {
        // 开发自检:把亮度和音量拉到 100%,专门截图验证"100%"不会压到进度条右边界
        // (右对齐的百分比是最宽的那一档,只有 100% 才暴露这个间距问题)。
        s_settings.brightness = CHICK_RUN_BRIGHTNESS_MAX;
        s_settings.volume = CHICK_RUN_VOLUME_MAX;
        settings_apply();
        page_invalidate();
    } else {
        shot_dump(&s_canvas, cmd->label);
    }
}
#endif

static void frame_task(void *arg) {
    (void)arg;
    uint32_t last_ms = now_ms();
    uint32_t next_frame_ms = last_ms;
    while (s_running) {
        chick_key_t key;
        while (s_keys != NULL && xQueueReceive(s_keys, &key, 0) == pdTRUE) handle_key(&key);

        const uint32_t time_ms = now_ms();
        uint32_t dt = time_ms - last_ms;
        last_ms = time_ms;
        if (dt > CHICK_MAX_DT_MS) dt = CHICK_MAX_DT_MS;

        if (s_screen != CHICK_SCREEN_GAME) scene_state_reset();
        render_frame(dt, time_ms);

        s_frames++;
        s_fps_count++;
        if ((int32_t)(time_ms - s_fps_last_ms) >= 1000) {
            s_fps_value = s_fps_count * 1000u / (uint32_t)(time_ms - s_fps_last_ms);
            s_fps_count = 0;
            s_fps_last_ms = time_ms;
            s_hud_pending = true;      // FPS 变了,重画右上角
            if (s_screen != CHICK_SCREEN_GAME) s_page_drawn = false;
        }

        mask_corners();
        damage_flush();
        // 首帧已经画完并推给面板 → 现在亮屏。
        if (!s_first_frame_pushed) {
            s_first_frame_pushed = true;
            s_backlight_on = true;
            bsp_display_backlight(s_settings.brightness);
            ESP_LOGI(TAG, "首帧已推送,开屏(面板就绪到首帧全程黑屏,避免开机花屏)");
        }
#ifdef CHICK_RUN_SHOT
        if (s_screen == CHICK_SCREEN_GAME && s_run.state == CHICK_STATE_RUNNING) {
            s_run.invincible_ms = CHICK_RUN_HURT_INVINCIBLE_MS;   // 截图期间撞不死
        }
        shot_step(time_ms);
#endif
        housekeeping(time_ms);
        stats_maybe_log(time_ms);

        // 按目标帧长补足等待(SPI 传输本身也会阻塞,这里是兜底)。
        next_frame_ms += CHICK_FRAME_MS;
        const uint32_t now = now_ms();
        const int32_t wait = (int32_t)(next_frame_ms - now);
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)wait));
        else next_frame_ms = now;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// --- 字形覆盖自检 -----------------------------------------------------------
static void font_check_coverage(void) {
    static const char *const inventory[] = { CHICK_TEXT_INVENTORY };
    const chick_font_t *const fonts[] = { FONT_CJK, FONT_TINY };
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

// --- 对外接口 ---------------------------------------------------------------
esp_err_t chick_run_ui_start(void) {
    if (s_running) return ESP_OK;

    s_keys = xQueueCreate(8, sizeof(chick_key_t));
    if (s_keys == NULL) return ESP_ERR_NO_MEM;

    // ★ 画布必须在音频/I2S 初始化【之前】分配:音频初始化会吃掉几十 KB,
    //   晚分配时最大连续块只剩 136KB,150KB 的画布就分配不到了(真机踩过)。
    ESP_LOGI(TAG, "堆:最大块 default=%u internal=%u int8=%u dma=%u iram=%u 总空闲=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_IRAM_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    // 画布:内部 RAM(SPI 推送会先拷进 staging,所以画布本身不必是 DMA 内存)。
    // 依次尝试:默认堆 → 内部 8bit → 内部。哪条能过用哪条(不同 IDF/配置下
    // 可用的 cap 组合不一样,硬编码一种就可能整块画布分配不到)。
    const size_t canvas_bytes = (size_t)LAYOUT_W * LAYOUT_H * 2;
    s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_DEFAULT);
    if (s_fb == NULL) s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_fb == NULL) s_fb = heap_caps_malloc(canvas_bytes, MALLOC_CAP_INTERNAL);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "画布分配失败:最大可用块 %u(DMA %u),剩余堆 %u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    s_stage = heap_caps_malloc((size_t)LAYOUT_W * STAGE_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_stage == NULL) {
        ESP_LOGE(TAG, "推送缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    chick_run_settings_defaults(&s_settings);
    if (chick_run_store_init() == ESP_OK) (void)chick_run_store_load_settings(&s_settings);
    chick_run_settings_sanitize(&s_settings);
    s_settings_row = SETTINGS_ROW_BRIGHTNESS;

    if (bsp_audio_init() == ESP_OK) {
        if (rtttl_player_start() == ESP_OK) rtttl_player_set_bgm(BGM_MAIN);
        else ESP_LOGW(TAG, "音频任务创建失败,静音运行");
    } else {
        ESP_LOGW(TAG, "音频初始化失败,静音运行");
    }
    bsp_audio_set_volume(s_settings.volume);
    // 色温补偿要在首帧推屏前建好表,否则第一帧会是未补偿的颜色。
    tint_rebuild();
    // 电池采样是可选能力:探测一次决定要不要画电量槽。
    s_battery_present = (bsp_battery_init() == ESP_OK);
    if (!s_battery_present) ESP_LOGI(TAG, "本板无电量计,电量槽不显示");

    s_mode = CHICK_RUN_MODE_FREE;
    uint8_t stored = 0;
    if (chick_run_store_load_mode(&stored) == ESP_OK && stored < (uint8_t)CHICK_RUN_MODE_COUNT) {
        s_mode = (chick_run_mode_t)stored;
    }
    chick_run_init(&s_run, (uint32_t)esp_timer_get_time());
    s_best_saved = chick_run_store_load_best();
    s_best_level_saved = chick_run_store_load_best_level();
    chick_run_set_best_level(&s_run, s_best_level_saved);
    chick_run_set_best(&s_run, s_best_saved);

    // 画布:内部 RAM(SPI DMA 必须能读)。240x320x2 = 150KB。
    // 画布不需要是 DMA 内存:推送时会先按行拷进 staging 缓冲(spi 面板 API 只接受
    // 连续缓冲,画布有行跨距),所以这里只要普通内部 RAM —— 加上 MALLOC_CAP_DMA
    // 会把可选块砍掉一大半,150KB 直接分配失败。
    if (s_stage == NULL) {
        ESP_LOGE(TAG, "推送缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }
    s_flush_done = xSemaphoreCreateCounting(1, 0);
    if (s_flush_done == NULL) {
        ESP_LOGE(TAG, "推送信号量创建失败,剩余堆 %u", (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "步骤 画布+推送缓冲+信号量 就绪,剩余堆 %u 最大块 %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_flush_done };
    (void)esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &cbs, NULL);

    s_canvas = (chick_canvas_t){ s_fb, LAYOUT_W, LAYOUT_W, LAYOUT_H };
    bg_prepare();
    if (!assets_init()) {
        ESP_LOGE(TAG, "精灵资产初始化失败(内部 RAM 不足?),剩余堆 %u",
                 (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "步骤 精灵资产 就绪,剩余堆 %u", (unsigned)esp_get_free_heap_size());
    scene_state_reset();
    page_invalidate();
    decor_reset();
    s_screen = CHICK_SCREEN_HOME;
    s_last_input_ms = now_ms();
    s_fps_last_ms = s_last_input_ms;
    // 背光先不开:面板已经上电但屏上还不是我们的画面,这一段黑着,
    // 等渲染任务把首帧推完再亮(见 frame_task 与 s_first_frame_pushed 的说明)。
    s_backlight_on = false;
    s_first_frame_pushed = false;
    bsp_display_backlight(0);
    settings_apply();
    font_check_coverage();

    s_running = true;
    // 单核芯片:不要用 xTaskCreatePinnedToCore(传 core 1 会直接失败)。
    if (xTaskCreate(frame_task, "chick_frame", FRAME_TASK_STACK, NULL, FRAME_TASK_PRIO,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "渲染任务创建失败,剩余堆 %u 最大块 %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "就绪:零 LVGL 自绘 %dx%d,SPI 脏区推送,目标 %d FPS",
             LAYOUT_W, LAYOUT_H, 1000 / CHICK_FRAME_MS);
    return ESP_OK;
}

void chick_run_ui_stop(void) {
    if (!s_running) return;
    s_running = false;
    (void)chick_run_store_save_settings(&s_settings);
    rtttl_player_stop();
    for (int i = 0; i < 100 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_keys) {
        vQueueDelete(s_keys);
        s_keys = NULL;
    }
}

void chick_run_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
#ifdef CHICK_RUN_SHOT
    // 开发自检:自动导航期间忽略真实按键 —— 否则手上随便按几下就会把固定时间表带偏,
    // 截图对不上页面(自检用的是 handle_key 直接注入,不走这条队列)。
    (void)btn;
    (void)ev;
    return;
#endif
    if (!s_running || s_keys == NULL) return;
    const chick_key_t key = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_keys, &key, 0);
}
