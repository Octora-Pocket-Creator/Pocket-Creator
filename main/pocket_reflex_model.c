// main/pocket_reflex_model.c —— 《方向快手》规则实现。见 .h 的说明。
#include "pocket_reflex_model.h"

// 自带 LCG(Numerical Recipes 的常数)。不用 esp_random():这一层要能在主机上
// 编译测试,而且出题只要"够乱",不需要密码学强度。
static uint32_t rng_next(pocket_reflex_t *g) {
    g->rng = g->rng * 1664525u + 1013904223u;
    return g->rng >> 16;
}

// 出下一题。避开和上一题同方向:连出两个"上"时,玩家分不清自己是不是按重了。
static pocket_key_t next_arrow(pocket_reflex_t *g) {
    pocket_key_t arrow = (pocket_key_t)(rng_next(g) % 4u);
    if (arrow == g->arrow) arrow = (pocket_key_t)((arrow + 1u + rng_next(g) % 3u) % 4u);
    return arrow;
}

// 本题限时:答对越多越短,到底就停在 POCKET_WINDOW_MIN_MS。
// 先比分数再减,避免无符号回绕(score 很大时 1400 - score*70 会变成一个巨大的数)。
static uint32_t window_for(uint16_t score) {
    const uint16_t steps = (POCKET_WINDOW_START_MS - POCKET_WINDOW_MIN_MS) / POCKET_WINDOW_STEP_MS;
    if (score >= steps) return POCKET_WINDOW_MIN_MS;
    return POCKET_WINDOW_START_MS - (uint32_t)score * POCKET_WINDOW_STEP_MS;
}

static void prompt(pocket_reflex_t *g, uint32_t now_ms) {
    g->arrow = next_arrow(g);
    g->window_ms = window_for(g->score);
    g->prompt_ms = now_ms;
    g->deadline_ms = now_ms + g->window_ms;
    g->phase = POCKET_PHASE_WAIT;
}

void pocket_reflex_init(pocket_reflex_t *g, uint32_t seed) {
    g->phase = POCKET_PHASE_TITLE;
    g->judge = POCKET_JUDGE_NONE;
    g->arrow = POCKET_KEY_UP;
    g->last_key = POCKET_KEY_UP;
    g->score = 0;
    g->best = 0;
    g->last_rt_ms = 0;
    g->lives = POCKET_LIVES;
    g->window_ms = POCKET_WINDOW_START_MS;
    g->prompt_ms = seed;
    g->deadline_ms = seed;
    g->flash_until_ms = 0;
    // 种子为 0 时 LCG 会一直吐 0 → 每题都是"上",所以强制非零。
    g->rng = seed | 1u;
}

void pocket_reflex_start(pocket_reflex_t *g, uint32_t now_ms) {
    g->score = 0;
    g->lives = POCKET_LIVES;
    g->judge = POCKET_JUDGE_NONE;
    g->last_rt_ms = 0;
    prompt(g, now_ms);
}

bool pocket_reflex_press(pocket_reflex_t *g, pocket_key_t key, uint32_t now_ms) {
    if (g->phase == POCKET_PHASE_TITLE || g->phase == POCKET_PHASE_OVER) {
        if (key != POCKET_KEY_A && key != POCKET_KEY_B) return false;
        pocket_reflex_start(g, now_ms);
        return true;
    }
    // 判定闪烁期间不吃按键:否则玩家连按会穿到下一题,变成"没看图也算对"。
    if (g->phase != POCKET_PHASE_WAIT) return false;
    // 答题中 A/B 无效,也不该被算成按错。
    if (key >= POCKET_KEY_A) return false;

    g->last_key = key;
    if (key == g->arrow) {
        g->last_rt_ms = (uint16_t)(now_ms - g->prompt_ms);
        g->score++;
        if (g->score > g->best) g->best = g->score;
        g->judge = POCKET_JUDGE_HIT;
    } else {
        if (g->lives > 0) g->lives--;
        g->judge = POCKET_JUDGE_WRONG;
    }
    g->flash_until_ms = now_ms + POCKET_JUDGE_FLASH_MS;
    g->phase = POCKET_PHASE_JUDGE;
    return true;
}

bool pocket_reflex_tick(pocket_reflex_t *g, uint32_t now_ms) {
    if (g->phase == POCKET_PHASE_WAIT && (int32_t)(now_ms - g->deadline_ms) >= 0) {
        if (g->lives > 0) g->lives--;
        g->judge = POCKET_JUDGE_SLOW;
        g->flash_until_ms = now_ms + POCKET_JUDGE_FLASH_MS;
        g->phase = POCKET_PHASE_JUDGE;
        return true;
    }
    if (g->phase == POCKET_PHASE_JUDGE && (int32_t)(now_ms - g->flash_until_ms) >= 0) {
        if (g->lives == 0) {
            if (g->score > g->best) g->best = g->score;
            g->phase = POCKET_PHASE_OVER;
        } else {
            prompt(g, now_ms);
        }
        return true;
    }
    return false;
}

int pocket_reflex_time_left_pct(const pocket_reflex_t *g, uint32_t now_ms) {
    if (g->phase != POCKET_PHASE_WAIT || g->window_ms == 0) return 0;
    const int32_t left = (int32_t)(g->deadline_ms - now_ms);
    if (left <= 0) return 0;
    const int32_t pct = left * 100 / (int32_t)g->window_ms;
    return pct > 100 ? 100 : pct;
}
