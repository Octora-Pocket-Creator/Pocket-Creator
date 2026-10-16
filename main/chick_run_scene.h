// main/chick_run_scene.h —— 世界渲染的“纯逻辑”:槽位清单 + 每块帧缓冲记账 + 脏矩形判定。
//
// 为什么单独成一层:这套判断是这个渲染器最容易写错的地方(已经踩过两次 ——
// ①和 LVGL 共用帧缓冲时被它盖掉;②HUD 只记一份全局账导致三缓冲轮转时新旧值狂闪)。
// 抽出来就能在主机上直接测,不用真机试错。
//
// 约定:
//   * 固定槽位,槽位顺序就是 z 序;同一精灵每帧占同一槽;
//   * 每块帧缓冲各自记一份“上次画进去的槽位”,因为帧缓冲是轮转复用的;
//   * 本模块只算“擦哪里、重画哪些槽”,不碰像素。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 槽位数 = 世界固定的装饰槽 + 光圈 1 + 小鸡 1 + 头顶音符 1 + 障碍池 + 道具池(见 main/chick_run_ui.c)。
// 槽位数组是"每块帧缓冲各记一份"的,所以这个数要盖住所有同时可见的精灵;
// 障碍池/道具池的上限变了,这里必须跟着变(chick_run_ui.c 有静态断言把关)。
#define CHICK_SCENE_SLOTS 48

typedef struct {
    uint8_t id;        // 精灵下标(只用于查尺寸与交给调用方贴图)
    int16_t x, y;      // 左上角(屏幕像素)
    bool drawn;        // 本帧是否可见
} chick_scene_item_t;

typedef struct {
    int16_t x, y, w, h;
} chick_rect_t;

typedef struct {
    chick_scene_item_t items[CHICK_SCENE_SLOTS];   // 上次画进这块缓冲的槽位
    bool bg_filled;                                // 是否已用烘焙背景整帧填过
} chick_scene_fb_t;

typedef struct {
    bool refill;                                     // 需要整帧填背景(并把所有可见精灵重画)
    chick_rect_t erase[CHICK_SCENE_SLOTS + 1];       // 需要回补背景的矩形
    int erase_count;
    bool redraw[CHICK_SCENE_SLOTS];                  // 需要重新贴图的槽位
} chick_scene_plan_t;

// 生成绘制计划。
//   sprite_w/sprite_h:按精灵下标查位图尺寸(旧位置的矩形要用【旧的】尺寸算);
//   extra/extra_count:额外要回补的矩形(例如弹层刚收起),可以为 NULL。
void chick_scene_plan(const chick_scene_fb_t *fb, const chick_scene_item_t *cur,
                      const uint16_t *sprite_w, const uint16_t *sprite_h,
                      const chick_rect_t *extra, int extra_count,
                      chick_scene_plan_t *plan);

// 计划执行完之后调用:把本帧的槽位清单记到这块缓冲上。
void chick_scene_commit(chick_scene_fb_t *fb, const chick_scene_item_t *cur);

// 矩形是否相交(调用方执行计划时也要用)。
bool chick_rect_overlap(const chick_rect_t *a, const chick_rect_t *b);
