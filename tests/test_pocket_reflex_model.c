// tests/test_pocket_reflex_model.c —— 《方向快手》规则的主机测试。
//
// 这些规则在设备上出错只表现为"分数不对/命数不对",靠肉眼很难定位,
// 所以把边界(超时、按错、限时缩短到下限、重开局)在这里钉死。
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pocket_reflex_model.h"

// 按当前题目方向按一次,返回用掉的毫秒数。
static void answer_correct(pocket_reflex_t *g, uint32_t *now_ms, uint32_t rt) {
    assert(g->phase == POCKET_PHASE_WAIT);
    assert(pocket_reflex_press(g, g->arrow, *now_ms + rt));
    assert(g->phase == POCKET_PHASE_JUDGE);
}

// 判定闪烁结束 → 下一题。
static void next_prompt(pocket_reflex_t *g, uint32_t *now_ms) {
    assert(g->phase == POCKET_PHASE_JUDGE);
    *now_ms = g->flash_until_ms;
    assert(pocket_reflex_tick(g, *now_ms));
    assert(g->phase == POCKET_PHASE_WAIT);
}

static void test_title_and_start(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 1234);
    assert(g.phase == POCKET_PHASE_TITLE);
    assert(g.lives == POCKET_LIVES);

    // 标题页只认 A/B:方向键不该开局。
    for (pocket_key_t k = POCKET_KEY_UP; k <= POCKET_KEY_RIGHT; k = (pocket_key_t)(k + 1)) {
        assert(!pocket_reflex_press(&g, k, 0));
    }
    assert(g.phase == POCKET_PHASE_TITLE);

    assert(pocket_reflex_press(&g, POCKET_KEY_A, 500));
    assert(g.phase == POCKET_PHASE_WAIT);
    assert(g.score == 0 && g.lives == POCKET_LIVES);
    assert(g.window_ms == POCKET_WINDOW_START_MS);
    assert(g.deadline_ms == 500 + POCKET_WINDOW_START_MS);
    assert(pocket_reflex_time_left_pct(&g, 500) == 100);
}

static void test_hit_scores_and_shrinks_window(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 7);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);

    uint32_t now = 0;
    uint32_t last_window = g.window_ms;
    for (int i = 0; i < 12; i++) {
        answer_correct(&g, &now, 120);
        assert(g.judge == POCKET_JUDGE_HIT);
        assert(g.last_rt_ms == 120);
        assert((int)g.score == i + 1);
        assert(g.lives == POCKET_LIVES);          // 按对不掉命
        assert(g.best == g.score);
        next_prompt(&g, &now);
        assert(g.window_ms <= last_window);       // 限时单调不增
        assert(g.window_ms >= POCKET_WINDOW_MIN_MS);
        last_window = g.window_ms;
    }
    assert(last_window < POCKET_WINDOW_START_MS);
}

static void test_window_has_a_floor(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 99);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);
    uint32_t now = 0;
    // 一路答对到远超"缩短到下限"的题数:限时必须停在下限,不能回绕成大数。
    for (int i = 0; i < 60; i++) {
        answer_correct(&g, &now, 50);
        next_prompt(&g, &now);
        assert(g.window_ms >= POCKET_WINDOW_MIN_MS);
        assert(g.window_ms <= POCKET_WINDOW_START_MS);
    }
    assert(g.window_ms == POCKET_WINDOW_MIN_MS);
    assert(g.score == 60);
}

static void test_wrong_key_costs_a_life(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 3);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);

    uint32_t now = 0;
    for (int i = 0; i < POCKET_LIVES; i++) {
        assert(g.phase == POCKET_PHASE_WAIT);
        const pocket_key_t wrong = (pocket_key_t)((g.arrow + 1) % 4);
        assert(pocket_reflex_press(&g, wrong, now));
        assert(g.judge == POCKET_JUDGE_WRONG);
        assert(g.score == 0);
        assert(g.lives == POCKET_LIVES - 1 - i);
        if (g.lives > 0) next_prompt(&g, &now);
    }
    // 命掉光:闪烁结束后进结算页,不再是新题。
    const uint32_t after = g.flash_until_ms;
    assert(pocket_reflex_tick(&g, after));
    assert(g.phase == POCKET_PHASE_OVER);
}

static void test_timeout_costs_a_life(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 11);
    pocket_reflex_press(&g, POCKET_KEY_A, 1000);

    // 差一毫秒不算超时。
    assert(!pocket_reflex_tick(&g, g.deadline_ms - 1));
    assert(g.phase == POCKET_PHASE_WAIT);
    assert(pocket_reflex_time_left_pct(&g, g.deadline_ms) == 0);

    assert(pocket_reflex_tick(&g, g.deadline_ms));
    assert(g.judge == POCKET_JUDGE_SLOW);
    assert(g.lives == POCKET_LIVES - 1);
    assert(g.phase == POCKET_PHASE_JUDGE);
}

static void test_keys_are_ignored_while_flashing(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 5);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);
    const pocket_key_t arrow = g.arrow;
    assert(pocket_reflex_press(&g, arrow, 10));
    assert(g.score == 1);

    // 判定闪烁期间的任何按键(包括再按一次对的方向)都不该被算进去。
    assert(!pocket_reflex_press(&g, arrow, 20));
    assert(!pocket_reflex_press(&g, POCKET_KEY_UP, 21));
    assert(g.score == 1 && g.lives == POCKET_LIVES);
    assert(g.phase == POCKET_PHASE_JUDGE);
}

static void test_ab_does_not_answer(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 17);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);
    assert(g.phase == POCKET_PHASE_WAIT);

    assert(!pocket_reflex_press(&g, POCKET_KEY_A, 10));
    assert(!pocket_reflex_press(&g, POCKET_KEY_B, 11));
    assert(g.lives == POCKET_LIVES && g.phase == POCKET_PHASE_WAIT);
}

static void test_best_survives_restart(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 21);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);

    uint32_t now = 0;
    answer_correct(&g, &now, 100);
    next_prompt(&g, &now);
    answer_correct(&g, &now, 100);
    next_prompt(&g, &now);
    assert(g.best == 2);

    // 连按错三次结束这一局。
    for (int i = 0; i < POCKET_LIVES; i++) {
        if (g.phase == POCKET_PHASE_OVER) break;
        assert(g.phase == POCKET_PHASE_WAIT);
        pocket_reflex_press(&g, (pocket_key_t)((g.arrow + 1) % 4), now);
        now = g.flash_until_ms;
        pocket_reflex_tick(&g, now);
    }
    assert(g.phase == POCKET_PHASE_OVER);
    assert(g.score == 2 && g.best == 2);

    // 重开:分数清零,最好成绩留着。
    assert(pocket_reflex_press(&g, POCKET_KEY_A, now));
    assert(g.phase == POCKET_PHASE_WAIT);
    assert(g.score == 0 && g.lives == POCKET_LIVES && g.best == 2);
}

static void test_arrows_do_not_repeat_back_to_back(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 1);
    pocket_reflex_press(&g, POCKET_KEY_A, 0);

    uint32_t now = 0;
    bool seen[4] = { false, false, false, false };
    for (int i = 0; i < 200; i++) {
        const pocket_key_t asked = g.arrow;
        assert(asked <= POCKET_KEY_RIGHT);
        seen[asked] = true;
        answer_correct(&g, &now, 1);
        next_prompt(&g, &now);
        assert(g.arrow != asked);        // 不连着出同一个方向
    }
    // 四个方向都要出得到,否则"不重复"只是变成了轮流。
    for (int i = 0; i < 4; i++) assert(seen[i]);
}

static void test_time_left_pct_is_bounded_and_monotonic(void) {
    pocket_reflex_t g;
    pocket_reflex_init(&g, 33);
    pocket_reflex_press(&g, POCKET_KEY_A, 100);

    int last = 101;
    for (uint32_t t = 100; t < g.deadline_ms; t += 25) {
        const int pct = pocket_reflex_time_left_pct(&g, t);
        assert(pct >= 0 && pct <= 100);
        assert(pct <= last);
        last = pct;
    }
    assert(last >= 0);
    // 非等待阶段没有"剩余时间"这回事。
    assert(pocket_reflex_press(&g, g.arrow, g.prompt_ms + 30));
    assert(pocket_reflex_time_left_pct(&g, g.prompt_ms + 30) == 0);
}

int main(void) {
    test_title_and_start();
    test_hit_scores_and_shrinks_window();
    test_window_has_a_floor();
    test_wrong_key_costs_a_life();
    test_timeout_costs_a_life();
    test_keys_are_ignored_while_flashing();
    test_ab_does_not_answer();
    test_best_survives_restart();
    test_arrows_do_not_repeat_back_to_back();
    test_time_left_pct_is_bounded_and_monotonic();
    puts("test_pocket_reflex_model: PASS");
    return 0;
}
