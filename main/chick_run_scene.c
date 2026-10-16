// main/chick_run_scene.c —— 见 chick_run_scene.h。
#include "chick_run_scene.h"

#include <string.h>

bool chick_rect_overlap(const chick_rect_t *a, const chick_rect_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static chick_rect_t item_rect(const chick_scene_item_t *item, const uint16_t *sprite_w,
                              const uint16_t *sprite_h) {
    const chick_rect_t r = {
        item->x, item->y,
        (int16_t)sprite_w[item->id], (int16_t)sprite_h[item->id]
    };
    return r;
}

static bool item_same(const chick_scene_item_t *a, const chick_scene_item_t *b) {
    if (a->drawn != b->drawn) return false;
    if (!b->drawn) return true;                     // 两个都不可见 = 一样
    return a->id == b->id && a->x == b->x && a->y == b->y;
}

void chick_scene_plan(const chick_scene_fb_t *fb, const chick_scene_item_t *cur,
                      const uint16_t *sprite_w, const uint16_t *sprite_h,
                      const chick_rect_t *extra, int extra_count,
                      chick_scene_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));

    if (!fb->bg_filled) {
        // 这块缓冲第一次轮到(或被显式作废,例如调光需要每帧重画):
        // 整帧填背景 + 所有可见精灵都要重画。
        plan->refill = true;
        for (int i = 0; i < CHICK_SCENE_SLOTS; i++) {
            plan->redraw[i] = cur[i].drawn;
        }
        return;
    }

    // 1) 额外要回补的矩形(弹层收起等)。
    for (int i = 0; i < extra_count && plan->erase_count < CHICK_SCENE_SLOTS; i++) {
        plan->erase[plan->erase_count++] = extra[i];
    }

    // 2) 变化的槽位:擦掉它上次画在这块缓冲上的矩形,并标记重画。
    //    注意是整块擦,不是“旧位置减新位置”:精灵有透明像素,
    //    只擦差集的话交集里的透明像素会留下旧图,变成残影。
    for (int i = 0; i < CHICK_SCENE_SLOTS; i++) {
        if (item_same(&fb->items[i], &cur[i])) continue;
        // 本帧不可见(闪烁/离场)时只需要擦掉旧矩形,没有东西要重画。
        plan->redraw[i] = cur[i].drawn;
        if (fb->items[i].drawn && plan->erase_count < CHICK_SCENE_SLOTS + 1) {
            plan->erase[plan->erase_count++] = item_rect(&fb->items[i], sprite_w, sprite_h);
        }
    }

    // 3) 被擦区域压到的、本来不用重画的精灵要跟着重画,否则会被背景抹掉一块
    //    (小鸡跑过树前面就是这么个情况)。
    for (int i = 0; i < CHICK_SCENE_SLOTS; i++) {
        if (plan->redraw[i] || !cur[i].drawn) continue;
        const chick_rect_t r = item_rect(&cur[i], sprite_w, sprite_h);
        for (int e = 0; e < plan->erase_count; e++) {
            if (chick_rect_overlap(&r, &plan->erase[e])) {
                plan->redraw[i] = true;
                break;
            }
        }
    }
}

void chick_scene_commit(chick_scene_fb_t *fb, const chick_scene_item_t *cur) {
    memcpy(fb->items, cur, sizeof(fb->items));
    fb->bg_filled = true;
}
