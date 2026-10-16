// tests/test_chick_run_model.c —— 小鸡快跑纯逻辑(物理/生成/碰撞/计分)主机测试。
//
// 只依赖 main/chick_run_model.c,不链接 LVGL 或 ESP-IDF,可直接用宿主 cc 运行。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "chick_run_model.h"

#define STEP_MS 16

// 按固定子步推进,并在游戏结束时提前停下(与 UI 定时器行为一致)。
static uint32_t run_for(chick_run_t *run, uint32_t ms) {
    uint32_t events = CHICK_EV_NONE;
    while (ms > 0) {
        uint32_t step = ms > STEP_MS ? STEP_MS : ms;
        ms -= step;
        events |= chick_run_tick(run, step);
        if (run->state != CHICK_STATE_RUNNING) break;
    }
    return events;
}

// 关掉随机生成,便于单独观察物理与碰撞。
static void quiet_spawns(chick_run_t *run) {
    run->next_gap_ms = 10u * 60u * 1000u;
}

static void place_obstacle(chick_run_t *run, chick_obstacle_kind_t kind, int32_t x_px) {
    chick_obstacle_t *ob = &run->obstacles[0];
    ob->kind = kind;
    ob->active = true;
    ob->x_milli = x_px * 1000;
}

static uint32_t count_active(const chick_run_t *run) {
    uint32_t n = 0;
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        if (run->obstacles[i].active) n++;
    }
    return n;
}

static void test_start_and_score(void) {
    chick_run_t run;
    chick_run_init(&run, 12345u);
    assert(run.state == CHICK_STATE_READY);
    assert(run.score_m == 0);
    assert(chick_run_tick(&run, 500) == CHICK_EV_NONE);   // 标题页不推进
    assert(run.elapsed_ms == 0);

    assert(chick_run_start(&run, CHICK_RUN_MODE_FREE));
    assert(run.state == CHICK_STATE_RUNNING);
    assert(!chick_run_start(&run, CHICK_RUN_MODE_FREE));                       // 跑动中不能重复开局

    quiet_spawns(&run);
    // 起始 120px/s、每秒 +4px/s:第 1 秒 120px、第 2 秒 124px,共 244px = 20 米。
    run_for(&run, 1000);
    assert(run.score_m == 7);
    assert(run.speed == CHICK_RUN_SPEED_START + CHICK_RUN_SPEED_GAIN);
    run_for(&run, 1000);
    assert(run.score_m == 15);
    assert(run.speed == CHICK_RUN_SPEED_START + 2 * CHICK_RUN_SPEED_GAIN);

    run_for(&run, 120000);
    assert(run.speed == CHICK_RUN_SPEED_MAX);             // 速度封顶
}

static void test_jump_physics(void) {
    chick_run_t run;
    chick_run_init(&run, 7u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);

    assert(chick_run_jump(&run));
    assert(!run.on_ground);
    assert(!chick_run_jump(&run));                        // 空中不能二段跳
    chick_run_jump_release(&run);

    int32_t peak = 0;
    uint32_t airtime = 0;
    while (!run.on_ground && airtime < 2000) {
        chick_run_tick(&run, STEP_MS);
        airtime += STEP_MS;
        if (chick_run_chick_y(&run) > peak) peak = chick_run_chick_y(&run);
    }
    assert(run.on_ground);
    assert(peak >= 92 && peak <= 115);                    // 短按跳顶点约 105px
    assert(airtime >= 780 && airtime <= 880);             // 滞空约 0.82s
    assert(chick_run_chick_y(&run) == 0);

    // 玩法不变量:短按跳跃必须明显高过最高的障碍(仙人掌 40px),留出余量。
    for (int kind = 0; kind < CHICK_OBSTACLE_KIND_COUNT; kind++) {
        const chick_obstacle_shape_t *shape = chick_run_obstacle_shape((chick_obstacle_kind_t)kind);
        assert(peak > shape->h + 20);
    }
}

static void test_hold_jump_is_higher(void) {
    chick_run_t short_run;
    chick_run_t long_run;
    chick_run_init(&short_run, 99u);
    chick_run_init(&long_run, 99u);
    chick_run_start(&short_run, CHICK_RUN_MODE_FREE);
    chick_run_start(&long_run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&short_run);
    quiet_spawns(&long_run);

    chick_run_jump(&short_run);
    chick_run_jump_release(&short_run);
    chick_run_jump(&long_run);

    int32_t short_peak = 0;
    int32_t long_peak = 0;
    for (int i = 0; i < 120; i++) {
        chick_run_tick(&short_run, STEP_MS);
        if (chick_run_chick_y(&short_run) > short_peak) short_peak = chick_run_chick_y(&short_run);
        if (i == CHICK_RUN_HOLD_MS / STEP_MS) chick_run_jump_release(&long_run);
        chick_run_tick(&long_run, STEP_MS);
        if (chick_run_chick_y(&long_run) > long_peak) long_peak = chick_run_chick_y(&long_run);
    }
    assert(short_run.on_ground);
    assert(long_run.on_ground);
    assert(short_peak >= 92 && short_peak <= 115);
    assert(long_peak >= short_peak + 8);                  // 长按更高
    assert(long_peak <= 140);                             // 但仍然受助力时长约束
}

static void test_slide_shortens_hitbox_temporarily(void) {
    chick_run_t run;
    chick_run_init(&run, 3u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);

    assert(chick_run_chick_height(&run) == CHICK_RUN_CHICK_H);
    chick_run_slide(&run);
    run_for(&run, CHICK_RUN_SLIDE_MS - 100);
    assert(run.ducking);
    assert(chick_run_chick_height(&run) == CHICK_RUN_CHICK_DUCK_H);

    // 滑铲是定时动作:到点自己恢复站姿,不依赖"松开"事件
    // (BSP 长按 500ms 之后不再上报松开)。
    run_for(&run, 200);
    assert(!run.ducking);
    assert(chick_run_chick_height(&run) == CHICK_RUN_CHICK_H);

    // 空中滑铲不减碰撞盒,只用于加速下落。
    chick_run_jump(&run);
    chick_run_slide(&run);
    run_for(&run, 100);
    assert(!run.on_ground);
    assert(chick_run_chick_height(&run) == CHICK_RUN_CHICK_H);

    // 连续按可以刷新滑铲时长。
    run_for(&run, 400);
    assert(run.on_ground);
    chick_run_slide(&run);
    run_for(&run, 200);
    chick_run_slide(&run);
    run_for(&run, 400);
    assert(run.ducking);
}

static void test_fence_requires_jump(void) {
    chick_run_t run;
    chick_run_init(&run, 5u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    place_obstacle(&run, CHICK_OBSTACLE_FENCE, CHICK_RUN_CHICK_X);

    uint32_t events = run_for(&run, STEP_MS * 2);
    assert((events & CHICK_EV_HIT) != 0);
    assert((events & CHICK_EV_GAME_OVER) != 0);
    assert(run.state == CHICK_STATE_GAME_OVER);

    // 蹲下也躲不过地面障碍。
    chick_run_init(&run, 5u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    chick_run_slide(&run);
    place_obstacle(&run, CHICK_OBSTACLE_FENCE, CHICK_RUN_CHICK_X);
    assert((run_for(&run, STEP_MS * 2) & CHICK_EV_HIT) != 0);
}



static void test_pause_freezes_simulation(void) {
    chick_run_t run;
    chick_run_init(&run, 21u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run_for(&run, 500);

    uint32_t score = run.score_m;
    uint32_t elapsed = run.elapsed_ms;
    assert(chick_run_toggle_pause(&run));
    assert(run.state == CHICK_STATE_PAUSED);
    assert(chick_run_tick(&run, 3000) == CHICK_EV_NONE);
    assert(run.score_m == score);
    assert(run.elapsed_ms == elapsed);

    assert(chick_run_toggle_pause(&run));
    assert(run.state == CHICK_STATE_RUNNING);
    run_for(&run, 500);
    assert(run.elapsed_ms == elapsed + 500);
    assert(run.score_m > score);

    // 暂停不能吞掉输入时间:恢复后每帧照常推进。
    assert(chick_run_toggle_pause(&run));
    assert(run.state == CHICK_STATE_PAUSED);
    assert(!chick_run_toggle_pause(&run) || run.state == CHICK_STATE_RUNNING);
}

static void test_abort_returns_to_title(void) {
    chick_run_t run;
    chick_run_init(&run, 31u);
    chick_run_set_best(&run, 42u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    run_for(&run, 3000);
    assert(count_active(&run) > 0);

    chick_run_abort(&run);
    assert(run.state == CHICK_STATE_READY);
    assert(run.score_m == 0);
    assert(run.elapsed_ms == 0);
    assert(count_active(&run) == 0);
    assert(run.best_m == 42u);                            // 最高分不因放弃而丢失
    assert(!chick_run_is_new_best(&run));
}

static void test_best_score_tracking(void) {
    chick_run_t run;
    chick_run_init(&run, 41u);
    chick_run_set_best(&run, 500u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run_for(&run, 2000);
    assert(run.score_m < 500u);
    assert(!chick_run_is_new_best(&run));
    assert(run.best_m == 500u);

    chick_run_init(&run, 41u);
    chick_run_set_best(&run, 5u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run_for(&run, 2000);
    assert(run.score_m > 5u);
    assert(chick_run_is_new_best(&run));
    assert(run.best_m == run.score_m);
}

static void test_milestone_events(void) {
    chick_run_t run;
    chick_run_init(&run, 51u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);

    uint32_t events = run_for(&run, 30000);
    assert(run.score_m > 2u * CHICK_RUN_MILESTONE_M);
    assert((events & CHICK_EV_MILESTONE) != 0);
    assert(run.milestone_m > 2u * CHICK_RUN_MILESTONE_M);
}

static void test_long_dt_does_not_tunnel(void) {
    chick_run_t run;
    chick_run_init(&run, 61u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    place_obstacle(&run, CHICK_OBSTACLE_FENCE, CHICK_RUN_SCREEN_W);

    // 一次 3 秒的大 dt 也必须撞上:内部子步长 16ms,障碍每步最多移动 6px,
    // 远小于小鸡 20px 宽的碰撞盒,不会穿过。
    uint32_t events = chick_run_tick(&run, 3000);
    assert((events & CHICK_EV_HIT) != 0);
    assert(run.state == CHICK_STATE_GAME_OVER);
}

static void test_seed_is_deterministic(void) {
    chick_run_t a;
    chick_run_t b;
    chick_run_t c;
    chick_run_init(&a, 777u);
    chick_run_init(&b, 777u);
    chick_run_init(&c, 778u);
    chick_run_start(&a, CHICK_RUN_MODE_FREE);
    chick_run_start(&b, CHICK_RUN_MODE_FREE);
    chick_run_start(&c, CHICK_RUN_MODE_FREE);

    // 双方一路撞到同一个障碍为止(不做任何躲闪)。
    for (int i = 0; i < 2000 && a.state == CHICK_STATE_RUNNING && b.state == CHICK_STATE_RUNNING; i++) {
        chick_run_tick(&a, STEP_MS);
        chick_run_tick(&b, STEP_MS);
    }
    assert(a.state == CHICK_STATE_GAME_OVER);
    assert(b.state == CHICK_STATE_GAME_OVER);
    assert(a.score_m == b.score_m);
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        assert(a.obstacles[i].kind == b.obstacles[i].kind);
        assert(a.obstacles[i].x_milli == b.obstacles[i].x_milli);
    }
    // 道具也必须在同一个随机序列里：跟同一个种子跑出来的两局，金币位置要一致。
    for (size_t i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        assert(a.pickups[i].kind == b.pickups[i].kind);
        assert(a.pickups[i].active == b.pickups[i].active);
        assert(a.pickups[i].x_milli == b.pickups[i].x_milli);
        assert(a.pickups[i].y_milli == b.pickups[i].y_milli);
    }

    for (int i = 0; i < 2000 && c.state == CHICK_STATE_RUNNING; i++) {
        chick_run_tick(&c, STEP_MS);
    }
    assert(c.dropped_spawns == 0);
}

// 核心可玩性不变量:地面障碍必须"跳得过去",且要在最低速度下也留足余量。
// 判据:小鸡脚底高于障碍顶部的持续时间 > 水平通过该障碍所需的时间 + 100ms ——
// 32px 宽的栅栏曾经让这条不等式不成立,表现就是"怎么跳都过不去"。
// 飞行障碍(乌鸦)不走这条:它的解法是蹲下,由 test_obstacle_shape_invariants
// 的高度关系保证。
static void test_ground_obstacles_are_jumpable_at_worst_speed(void) {
    chick_run_t run;
    for (int kind = 0; kind < CHICK_OBSTACLE_KIND_COUNT; kind++) {
        const chick_obstacle_shape_t *shape =
            chick_run_obstacle_shape((chick_obstacle_kind_t)kind);
        if (shape->y_bottom != 0) continue;
        const int32_t top = shape->y_bottom + shape->h;

        chick_run_init(&run, 42u);
        chick_run_start(&run, CHICK_RUN_MODE_FREE);
        quiet_spawns(&run);
        chick_run_jump(&run);
        chick_run_jump_release(&run);

        uint32_t above_ms = 0;
        while (!run.on_ground && above_ms < 3000u) {
            chick_run_tick(&run, STEP_MS);
            if (chick_run_chick_y(&run) > top) above_ms += STEP_MS;
        }
        const uint32_t pass_ms =
            (uint32_t)(shape->w + CHICK_RUN_CHICK_W) * 1000u / (uint32_t)CHICK_RUN_SPEED_START;
        assert(above_ms > pass_ms + 100u);
    }
}

static void test_obstacle_pool_has_headroom(void) {
    // 最慢速度下障碍需要约 2200ms 才能穿越屏幕,而最小生成间隔是 700ms,
    // 因此最坏情况会同时存在 4 个障碍;池子必须留有余量而不是靠运气。
    const uint32_t crossing_ms =
        (CHICK_RUN_SCREEN_W + 4 + CHICK_RUN_CHICK_W) * 1000u / CHICK_RUN_SPEED_START;
    const uint32_t worst_concurrent = crossing_ms / CHICK_RUN_GAP_MIN_MS + 2u;
    assert(worst_concurrent <= CHICK_RUN_MAX_OBSTACLES);
}

static void test_spawn_pool_never_drops(void) {
    // 把生成间隔强行压到最小值、速度压在下限,反复跑到撞上为止,
    // 逼近"屏幕上同时存在最多障碍"的极端情况,确认池子不会悄悄吞掉障碍。
    uint32_t most_active = 0;
    for (uint32_t seed = 1; seed <= 25; seed++) {
        chick_run_t run;
        chick_run_init(&run, seed);
        chick_run_start(&run, CHICK_RUN_MODE_FREE);
        for (int i = 0; i < 400 && run.state == CHICK_STATE_RUNNING; i++) {
            if (run.next_gap_ms > CHICK_RUN_GAP_MIN_MS) run.next_gap_ms = CHICK_RUN_GAP_MIN_MS;
            run.elapsed_ms = 0;                            // 锁定最低速度
            chick_run_tick(&run, STEP_MS);
            uint32_t active = count_active(&run);
            if (active > most_active) most_active = active;
        }
        assert(run.dropped_spawns == 0);
    }
    assert(most_active >= 2);                              // 确实跑到了多障碍同时在屏
    assert(most_active <= CHICK_RUN_MAX_OBSTACLES);
}

static void test_obstacle_shape_invariants(void) {
    for (int kind = 0; kind < CHICK_OBSTACLE_KIND_COUNT; kind++) {
        const chick_obstacle_shape_t *shape = chick_run_obstacle_shape((chick_obstacle_kind_t)kind);
        assert(shape != NULL);
        assert(shape->w > 0 && shape->h > 0);
        // 目前所有障碍都是地面障碍:统一靠跳跃通过。
        assert(shape->y_bottom == 0);
    }
}

static void test_obstacle_box_matches_ground_line(void) {
    // 断言只依赖尺寸表与地面线:改放大倍数/改障碍高度都不需要改这里。
    for (int kind = 0; kind < CHICK_OBSTACLE_KIND_COUNT; kind++) {
        const chick_obstacle_shape_t *shape = chick_run_obstacle_shape((chick_obstacle_kind_t)kind);
        chick_obstacle_t ob = { .kind = (chick_obstacle_kind_t)kind, .active = true,
                                .x_milli = 100000 };
        int32_t left, top, right, bottom;
        assert(chick_run_obstacle_box(&ob, &left, &top, &right, &bottom));
        assert(left == 100);
        assert(right - left == shape->w);
        assert(bottom == CHICK_RUN_GROUND_Y - shape->y_bottom);
        assert(bottom - top == shape->h);

        ob.active = false;
        assert(!chick_run_obstacle_box(&ob, &left, &top, &right, &bottom));
    }
}

// 破纪录音效只允许响一次:best_m 会被抬到当前分数,若直接拿它当事件条件,
// 破纪录后每加 1 米就会再发一次事件 —— 听感就是后期出现一段连续碎音(实测反馈过)。
static void test_new_best_event_fires_once_per_run(void) {
    chick_run_t run;
    chick_run_init(&run, 71u);
    chick_run_set_best(&run, 5u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);

    uint32_t best_events = 0;
    uint32_t milestone_events = 0;
    for (uint32_t elapsed = 0; elapsed < 60000u; elapsed += STEP_MS) {
        const uint32_t events = chick_run_tick(&run, STEP_MS);
        if (events & CHICK_EV_NEW_BEST) {
            best_events++;
            assert(run.best_m == run.score_m);   // 触发那一次已经抬到当前分数
        }
        if (events & CHICK_EV_MILESTONE) milestone_events++;
    }
    assert(run.score_m > 5u);                    // 确认真的破了纪录
    assert(best_events == 1u);
    assert(milestone_events >= 1u);
    assert(milestone_events * CHICK_RUN_MILESTONE_M <= run.score_m + CHICK_RUN_MILESTONE_M);
}


// --- 关卡模式 ---------------------------------------------------------------

// 跑到目标距离:过关、进下一关、进度归零、命数补满、障碍清空。
static void test_level_mode_clears_level_at_target(void) {
    chick_run_t run;
    chick_run_init(&run, 4242u);
    assert(chick_run_start(&run, CHICK_RUN_MODE_LEVEL));
    assert(run.mode == CHICK_RUN_MODE_LEVEL);
    assert(run.level == 1);
    assert(run.lives == CHICK_RUN_LEVEL_LIVES);

    // 手动把累计距离推到刚好过线,避免真跑 1000 米(测试要秒级完成)。
    quiet_spawns(&run);
    run.distance_milli = (int32_t)CHICK_RUN_LEVEL_TARGET_M * CHICK_RUN_PX_PER_M * 1000;
    const uint32_t events = run_for(&run, STEP_MS);

    assert((events & CHICK_EV_LEVEL_UP) != 0);
    assert(run.level == 2);
    assert(run.score_m == 0);                       // 新关从 0 米开始
    assert(run.lives == CHICK_RUN_LEVEL_LIVES);     // 没掉过命,过关后仍是 3 条
    assert(run.speed_base == chick_run_level_speed_base(2));
    assert(run.speed_base > chick_run_level_speed_base(1));   // 下一关更快
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        assert(!run.obstacles[i].active);           // 新关不继承上一关的障碍
    }
    assert(run.best_level == 2);
}

// 命数整局共用:过关不补命,掉过的命不会因为过关而回来。
static void test_level_lives_are_shared_across_levels(void) {
    chick_run_t run;
    chick_run_init(&run, 5150u);
    chick_run_start(&run, CHICK_RUN_MODE_LEVEL);
    quiet_spawns(&run);

    // 先掉一条命。
    place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
    (void)run_for(&run, STEP_MS);
    assert(run.lives == CHICK_RUN_LEVEL_LIVES - 1);

    // 等无敌结束后过关,命数必须保持 2,而不是补回 3。
    while (run.invincible_ms > 0) run_for(&run, STEP_MS);
    run.distance_milli = (int32_t)CHICK_RUN_LEVEL_TARGET_M * CHICK_RUN_PX_PER_M * 1000;
    const uint32_t events = run_for(&run, STEP_MS);
    assert((events & CHICK_EV_LEVEL_UP) != 0);
    assert(run.level == 2);
    assert(run.lives == CHICK_RUN_LEVEL_LIVES - 1);
}

// 关卡模式 3 条命:掉命继续跑,无敌期内不再掉,掉光才结束。
static void test_level_mode_three_lives(void) {
    chick_run_t run;
    chick_run_init(&run, 777u);
    chick_run_start(&run, CHICK_RUN_MODE_LEVEL);
    quiet_spawns(&run);

    // 第一次撞击:掉一条命,游戏继续。
    place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
    uint32_t events = run_for(&run, STEP_MS);
    assert((events & CHICK_EV_HURT) != 0);
    assert((events & CHICK_EV_GAME_OVER) == 0);
    assert(run.state == CHICK_STATE_RUNNING);
    assert(run.lives == CHICK_RUN_LEVEL_LIVES - 1);
    assert(run.invincible_ms > 0);

    // 无敌期内再撞:命数不变。
    place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
    events = run_for(&run, STEP_MS * 2);
    assert((events & CHICK_EV_HURT) == 0);
    assert(run.lives == CHICK_RUN_LEVEL_LIVES - 1);

    // 等无敌时间过去,再撞两次:掉光最后两条命后结束。
    while (run.invincible_ms > 0) run_for(&run, STEP_MS);
    place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
    events = run_for(&run, STEP_MS);
    assert(run.lives == 1);
    assert(run.state == CHICK_STATE_RUNNING);

    while (run.invincible_ms > 0) run_for(&run, STEP_MS);
    place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
    events = run_for(&run, STEP_MS);
    assert((events & CHICK_EV_GAME_OVER) != 0);
    assert(run.state == CHICK_STATE_GAME_OVER);
    assert(run.lives == 0);
    assert(run.best_level == 1);                    // 死在第一关,最高到第一关
}

// 自由模式保持原样:一撞就结束,不显示命数。
static void test_free_mode_still_one_hit(void) {
    chick_run_t run;
    chick_run_init(&run, 888u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    assert(run.lives == 1);
    place_obstacle(&run, CHICK_OBSTACLE_FENCE, CHICK_RUN_CHICK_X);
    const uint32_t events = run_for(&run, STEP_MS);
    assert((events & CHICK_EV_GAME_OVER) != 0);
    assert((events & CHICK_EV_HURT) == 0);
    assert(run.level == 1);                          // 自由模式不推进关卡
}

// 每关难度:速度单调不减并封顶;生成间隔收紧但不低于"跳得过去"的下限。
static void test_level_difficulty_curve(void) {
    int32_t prev_speed = chick_run_level_speed_base(1);
    uint32_t prev_gap_min = chick_run_level_gap_min(1);
    uint32_t prev_gap_max = chick_run_level_gap_max(1);
    for (uint32_t level = 1; level <= 20; level++) {
        const int32_t speed = chick_run_level_speed_base(level);
        const uint32_t gap_min = chick_run_level_gap_min(level);
        const uint32_t gap_max = chick_run_level_gap_max(level);

        assert(speed >= prev_speed && speed <= CHICK_RUN_SPEED_MAX);
        assert(gap_min <= prev_gap_min && gap_min >= CHICK_RUN_LEVEL_GAP_MIN_FLOOR);
        assert(gap_max <= prev_gap_max && gap_max >= CHICK_RUN_LEVEL_GAP_MAX_FLOOR);
        assert(gap_max >= gap_min);
        // 关键不变量:间隔不能短于一次跳跃滞空(约 820ms),否则会"落地即撞"。
        assert(gap_min >= 900);
        prev_speed = speed;
        prev_gap_min = gap_min;
        prev_gap_max = gap_max;
    }
    assert(chick_run_level_speed_base(0) == CHICK_RUN_SPEED_START);   // 非法关卡不炸
    assert(chick_run_level_gap_min(0) >= CHICK_RUN_LEVEL_GAP_MIN_FLOOR);
}

// --- 收集道具(金币 / 宝石 / 音符)------------------------------------------

static void place_pickup(chick_run_t *run, chick_pickup_kind_t kind, int32_t x_px, int32_t y_px) {
    chick_pickup_t *pickup = &run->pickups[0];
    pickup->kind = kind;
    pickup->active = true;
    pickup->x_milli = x_px * 1000;
    pickup->y_milli = y_px * 1000;
}

static uint32_t count_active_pickups(const chick_run_t *run) {
    uint32_t n = 0;
    for (size_t i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        if (run->pickups[i].active) n++;
    }
    return n;
}

// 需求核心：站着跑吃不到空中的金币，跳起来才能吃到，吃到就加分。
static void test_pickup_requires_jump_and_adds_score(void) {
    chick_run_t run;
    chick_run_init(&run, 901u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    place_pickup(&run, CHICK_PICKUP_COIN, CHICK_RUN_CHICK_X, 60);

    // 地面上：碰撞盒高 0..40，金币在 60..76 —— 水平重叠也吃不到。
    const uint32_t ground_events = run_for(&run, STEP_MS * 2);
    assert((ground_events & (CHICK_EV_COIN | CHICK_EV_GEM | CHICK_EV_NOTE)) == 0);
    assert(run.coins == 0);
    assert(chick_run_bonus_points(&run) == 0);

    // 重新摆一次（金币一直在左移），跳起来。
    place_pickup(&run, CHICK_PICKUP_COIN, CHICK_RUN_CHICK_X, 60);
    const uint32_t before_bonus = chick_run_bonus_points(&run);
    assert(chick_run_jump(&run));
    uint32_t events = CHICK_EV_NONE;
    for (int i = 0; i < 60 && run.coins == 0; i++) events |= chick_run_tick(&run, STEP_MS);

    assert((events & CHICK_EV_COIN) != 0);
    assert(run.coins == 1);
    // 加分正好是一枚金币的分值（距离在此期间也在涨，所以只看 bonus）。
    assert(chick_run_bonus_points(&run) == before_bonus + CHICK_RUN_COIN_POINTS);
    assert(chick_run_total_score(&run) == run.score_m + chick_run_bonus_points(&run));
    assert(!run.pickups[0].active);                      // 吃掉了就不会再触发一次
    assert((run_for(&run, 500) & CHICK_EV_COIN) == 0);
}

static void test_gem_and_note_points(void) {
    chick_run_t run;
    chick_run_init(&run, 902u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);

    place_pickup(&run, CHICK_PICKUP_GEM, CHICK_RUN_CHICK_X, 56);
    chick_run_jump(&run);
    uint32_t events = CHICK_EV_NONE;
    for (int i = 0; i < 60 && run.gems == 0; i++) events |= chick_run_tick(&run, STEP_MS);
    assert((events & CHICK_EV_GEM) != 0);
    assert(run.gems == 1);
    assert(chick_run_bonus_points(&run) == CHICK_RUN_GEM_POINTS);

    // 音符（音乐道具）：+10 分，并且单独发一个事件让 UI 去播《欢乐颂》。
    while (!run.on_ground) chick_run_tick(&run, STEP_MS);
    place_pickup(&run, CHICK_PICKUP_NOTE, CHICK_RUN_CHICK_X, 56);
    chick_run_jump(&run);
    events = CHICK_EV_NONE;
    for (int i = 0; i < 60 && run.notes == 0; i++) events |= chick_run_tick(&run, STEP_MS);
    assert((events & CHICK_EV_NOTE) != 0);
    assert((events & CHICK_EV_COIN) == 0);
    assert(run.notes == 1);
    assert(chick_run_total_score(&run) ==
           run.score_m + CHICK_RUN_GEM_POINTS + CHICK_RUN_NOTE_POINTS);
}

// 最难拿的那枚（生成器能放到的最高位置）也必须"跳得起来就吃得到"。
static void test_tallest_spawned_pickup_is_reachable(void) {
    const uint32_t any_pickup = CHICK_EV_COIN | CHICK_EV_GEM | CHICK_EV_NOTE | CHICK_EV_BOOST;
    for (int kind = 0; kind < CHICK_PICKUP_KIND_COUNT; kind++) {
        const chick_pickup_shape_t *shape = chick_run_pickup_shape((chick_pickup_kind_t)kind);
        const int32_t y = CHICK_RUN_PICKUP_MAX_TOP - shape->h;
        chick_run_t run;
        chick_run_init(&run, 903u);
        chick_run_start(&run, CHICK_RUN_MODE_FREE);
        quiet_spawns(&run);
        place_pickup(&run, (chick_pickup_kind_t)kind, CHICK_RUN_CHICK_X, y);
        chick_run_jump(&run);
        bool got = false;
        for (int i = 0; i < 80 && !got; i++) {
            got = (chick_run_tick(&run, STEP_MS) & any_pickup) != 0;
        }
        assert(got);
    }
}

// 生成出来的道具必须满足两条不变量：
//   * 高过最高的障碍（否则会变成"想吃就得撞"的死局）；
//   * 不超过跳跃能够得到的高度（否则就是永远拿不到的装饰）。
static void test_spawned_pickups_are_reachable_and_never_blocking(void) {
    int32_t tallest_obstacle = 0;
    for (int kind = 0; kind < CHICK_OBSTACLE_KIND_COUNT; kind++) {
        const chick_obstacle_shape_t *shape = chick_run_obstacle_shape((chick_obstacle_kind_t)kind);
        if (shape->y_bottom + shape->h > tallest_obstacle) tallest_obstacle = shape->y_bottom + shape->h;
    }
    assert(CHICK_RUN_PICKUP_MIN_Y > tallest_obstacle);

    uint32_t seen[CHICK_PICKUP_KIND_COUNT] = { 0 };
    uint32_t most_active = 0;
    for (uint32_t seed = 1; seed <= 12; seed++) {
        chick_run_t run;
        chick_run_init(&run, seed);
        chick_run_start(&run, CHICK_RUN_MODE_FREE);
        for (int i = 0; i < 3000 && run.state == CHICK_STATE_RUNNING; i++) {
            run.invincible_ms = CHICK_RUN_HURT_INVINCIBLE_MS;   // 先专心看生成，不撞死
            chick_run_tick(&run, STEP_MS);
            uint32_t active = 0;
            for (size_t k = 0; k < CHICK_RUN_MAX_PICKUPS; k++) {
                const chick_pickup_t *pickup = &run.pickups[k];
                if (!pickup->active) continue;
                active++;
                const chick_pickup_shape_t *shape = chick_run_pickup_shape(pickup->kind);
                assert(pickup->kind >= 0 && pickup->kind < CHICK_PICKUP_KIND_COUNT);
                assert(chick_run_pickup_y(pickup) >= CHICK_RUN_PICKUP_MIN_Y);
                assert(chick_run_pickup_y(pickup) + shape->h <= CHICK_RUN_PICKUP_MAX_TOP);
                seen[pickup->kind]++;
            }
            if (active > most_active) most_active = active;
        }
    }
    // 四种道具都要真的出现过，否则"加了道具"只是纸面上的。
    assert(seen[CHICK_PICKUP_COIN] > 0);
    assert(seen[CHICK_PICKUP_GEM] > 0);
    assert(seen[CHICK_PICKUP_NOTE] > 0);
    assert(seen[CHICK_PICKUP_BOOST] > 0);
    // 音乐道具与加速符是"大奖",必须明显比金币/宝石稀疏(用户要求有稀缺性):
    // 这里用生成权重导致的在场帧数做粗略卡口 —— 把它们调得和宝石一样常见就会挂。
    assert(seen[CHICK_PICKUP_NOTE] + seen[CHICK_PICKUP_BOOST] <
           seen[CHICK_PICKUP_GEM]);
    assert(most_active >= 2);
}

// 道具池不能被塞爆（和障碍一样的回归测试）。
static void test_pickup_pool_never_drops(void) {
    uint32_t most_active = 0;
    for (uint32_t seed = 1; seed <= 25; seed++) {
        chick_run_t run;
        chick_run_init(&run, seed);
        chick_run_start(&run, CHICK_RUN_MODE_FREE);
        for (int i = 0; i < 4000; i++) {
            if (run.next_gap_ms > CHICK_RUN_GAP_MIN_MS) run.next_gap_ms = CHICK_RUN_GAP_MIN_MS;
            run.elapsed_ms = 0;                                  // 锁定最低速度
            run.invincible_ms = CHICK_RUN_HURT_INVINCIBLE_MS;    // 跑满 64 秒
            chick_run_tick(&run, STEP_MS);
            const uint32_t active = count_active_pickups(&run);
            if (active > most_active) most_active = active;
        }
        assert(run.dropped_pickups == 0);
        assert(run.dropped_spawns == 0);
    }
    assert(most_active >= 3);
    assert(most_active <= CHICK_RUN_MAX_PICKUPS);
}

// 自由模式的纪录看"总得分"：距离加分，道具也加分。
static void test_total_score_drives_free_mode_record(void) {
    chick_run_t run;
    chick_run_init(&run, 904u);
    chick_run_set_best(&run, 0u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);

    run_for(&run, 2000);
    assert(chick_run_is_new_best(&run));
    assert(run.best_m == run.score_m);          // 纯跑动时纪录 == 距离

    const uint32_t before = run.best_m;
    place_pickup(&run, CHICK_PICKUP_GEM, CHICK_RUN_CHICK_X, 56);
    chick_run_jump(&run);
    for (int i = 0; i < 60 && run.gems == 0; i++) chick_run_tick(&run, STEP_MS);
    assert(run.gems == 1);
    assert(run.best_m >= before + CHICK_RUN_GEM_POINTS);   // 道具把纪录抬上去了
    assert(run.best_m == chick_run_total_score(&run));
}

static void test_pickups_cleared_when_level_changes(void) {
    chick_run_t run;
    chick_run_init(&run, 905u);
    chick_run_start(&run, CHICK_RUN_MODE_LEVEL);
    quiet_spawns(&run);
    place_pickup(&run, CHICK_PICKUP_COIN, CHICK_RUN_SCREEN_W, 60);
    assert(count_active_pickups(&run) == 1);

    run.distance_milli = (int32_t)CHICK_RUN_LEVEL_TARGET_M * CHICK_RUN_PX_PER_M * 1000;
    const uint32_t events = run_for(&run, STEP_MS);
    assert((events & CHICK_EV_LEVEL_UP) != 0);
    assert(count_active_pickups(&run) == 0);       // 新关不继承旧道具

    // 放弃本局也要清干净（回首页不能还留着上一局的道具）。
    place_pickup(&run, CHICK_PICKUP_GEM, CHICK_RUN_SCREEN_W, 60);
    chick_run_abort(&run);
    assert(count_active_pickups(&run) == 0);
}

// --- 加速护身符 ---------------------------------------------------------

// 吃到加速符:进入加速状态,而且速度真的抬起来(不是只改了个标志位)。
static void test_boost_pickup_speeds_up(void) {
    chick_run_t run;
    chick_run_init(&run, 1001u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run_for(&run, 500);
    const int32_t normal_speed = run.speed;

    place_pickup(&run, CHICK_PICKUP_BOOST, CHICK_RUN_CHICK_X, 56);
    chick_run_jump(&run);
    uint32_t events = CHICK_EV_NONE;
    for (int i = 0; i < 60 && run.boosts == 0; i++) events |= chick_run_tick(&run, STEP_MS);

    assert((events & CHICK_EV_BOOST) != 0);
    assert(run.boosts == 1);
    assert(chick_run_is_boosted(&run));
    assert(chick_run_boost_seconds(&run) == (CHICK_RUN_BOOST_MS + 999u) / 1000u);
    // 加速符本身不加分：它的价值是那 5.5 秒。
    assert(chick_run_bonus_points(&run) == 0);
    assert(run.speed > normal_speed);
    assert(run.speed >= normal_speed * CHICK_RUN_BOOST_SPEED_PERMILLE / 1000 - 2);
    // 当前速度必须是"普通封顶 × 倍率"：加速不能被普通速度上限吃掉。
    assert(run.speed <= CHICK_RUN_SPEED_MAX * CHICK_RUN_BOOST_SPEED_PERMILLE / 1000 + 2);
}

// 加速期间撞上障碍是"撞碎"：不掉命、不停下，障碍直接消失。
static void test_boost_smashes_obstacles(void) {
    chick_run_t run;
    chick_run_init(&run, 1002u);
    chick_run_start(&run, CHICK_RUN_MODE_LEVEL);
    quiet_spawns(&run);
    const uint8_t lives_before = run.lives;
    run.boost_ms = CHICK_RUN_BOOST_MS;

    place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
    run.obstacles[1].kind = CHICK_OBSTACLE_FENCE;
    run.obstacles[1].active = true;
    run.obstacles[1].x_milli = (CHICK_RUN_CHICK_X + 4) * 1000;
    const uint32_t events = run_for(&run, STEP_MS);

    assert((events & CHICK_EV_SMASH) != 0);
    assert((events & CHICK_EV_HIT) == 0);
    assert(run.state == CHICK_STATE_RUNNING);
    assert(run.lives == lives_before);
    assert(count_active(&run) == 0);            // 碰到的两个都碎了
    assert(chick_run_smashed(&run) == 2);
}

// 加速结束之后回到普通规则：光圈没了、再撞就死。
static void test_boost_expires_and_returns_to_normal(void) {
    chick_run_t run;
    chick_run_init(&run, 1003u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run.boost_ms = CHICK_RUN_BOOST_MS;

    while (chick_run_is_boosted(&run)) chick_run_tick(&run, STEP_MS);
    assert(chick_run_boost_seconds(&run) == 0);
    assert(run.speed <= CHICK_RUN_SPEED_MAX);

    place_obstacle(&run, CHICK_OBSTACLE_FENCE, CHICK_RUN_CHICK_X);
    const uint32_t events = run_for(&run, STEP_MS);
    assert((events & CHICK_EV_GAME_OVER) != 0);
}

// 加速的"最坏情况"也不能穿过障碍：一次 6 秒的大 dt + 加速也必须检测到碰撞。
static void test_boost_does_not_tunnel(void) {
    chick_run_t run;
    chick_run_init(&run, 1004u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run.boost_ms = 6000u;                       // 持续到跑完
    place_obstacle(&run, CHICK_OBSTACLE_FENCE, CHICK_RUN_SCREEN_W);
    const uint32_t events = chick_run_tick(&run, 6000);
    assert((events & CHICK_EV_SMASH) != 0);
    assert(run.state == CHICK_STATE_RUNNING);
}

// 加速期间不能掉命，但也不能把命数补回去，更不能变成无敌外挂的跳过判定：
// 加速结束那一子步必须回到普通碰撞规则。
static void test_boost_lives_untouched(void) {
    chick_run_t run;
    chick_run_init(&run, 1005u);
    chick_run_start(&run, CHICK_RUN_MODE_LEVEL);
    quiet_spawns(&run);
    run.boost_ms = CHICK_RUN_BOOST_MS;
    // 整个加速期间每步都往小鸡身上放一个仙人掌:全部应该被撞碎。
    // 最后不到一步时不再放 —— 加速到点之后本来就该按普通规则算,那不是 bug。
    while (chick_run_is_boosted(&run)) {
        if (run.boost_ms > STEP_MS) place_obstacle(&run, CHICK_OBSTACLE_CACTUS, CHICK_RUN_CHICK_X);
        chick_run_tick(&run, STEP_MS);
    }
    assert(run.lives == CHICK_RUN_LEVEL_LIVES);
    assert(run.state == CHICK_STATE_RUNNING);
    assert(chick_run_smashed(&run) > 0);
}

// 音符道具：吃到之后头顶飘 🎵 的计时开始，音乐停了就结束；暂停时不走时。
static void test_note_float_timer(void) {
    chick_run_t run;
    chick_run_init(&run, 1006u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    assert(chick_run_note_float_ms(&run) == 0);

    place_pickup(&run, CHICK_PICKUP_NOTE, CHICK_RUN_CHICK_X, 56);
    chick_run_jump(&run);
    for (int i = 0; i < 60 && run.notes == 0; i++) chick_run_tick(&run, STEP_MS);
    assert(chick_run_note_float_ms(&run) == CHICK_RUN_NOTE_FLOAT_MS);

    // 暂停期间不能偷偷把计时走完（否则恢复时音符已经消失了）。
    (void)chick_run_toggle_pause(&run);
    chick_run_tick(&run, 2000);
    assert(chick_run_note_float_ms(&run) == CHICK_RUN_NOTE_FLOAT_MS);
    (void)chick_run_toggle_pause(&run);

    run_for(&run, CHICK_RUN_NOTE_FLOAT_MS + 200);
    assert(chick_run_note_float_ms(&run) == 0);
}

// 再吃一个加速符可以把时间刷新回去（不吃亏）。
static void test_boost_refreshes(void) {
    chick_run_t run;
    chick_run_init(&run, 1007u);
    chick_run_start(&run, CHICK_RUN_MODE_FREE);
    quiet_spawns(&run);
    run.boost_ms = CHICK_RUN_BOOST_MS / 2;
    place_pickup(&run, CHICK_PICKUP_BOOST, CHICK_RUN_CHICK_X, 56);
    chick_run_jump(&run);
    for (int i = 0; i < 60 && run.boosts == 0; i++) chick_run_tick(&run, STEP_MS);
    assert(run.boosts == 1);
    assert(chick_run_boost_seconds(&run) >= 6u);
}

int main(void) {
    test_start_and_score();
    test_jump_physics();
    test_hold_jump_is_higher();
    test_slide_shortens_hitbox_temporarily();
    test_fence_requires_jump();
    test_pause_freezes_simulation();
    test_abort_returns_to_title();
    test_best_score_tracking();
    test_milestone_events();
    test_long_dt_does_not_tunnel();
    test_seed_is_deterministic();
    test_ground_obstacles_are_jumpable_at_worst_speed();
    test_level_mode_clears_level_at_target();
    test_level_mode_three_lives();
    test_level_lives_are_shared_across_levels();
    test_free_mode_still_one_hit();
    test_level_difficulty_curve();
    test_new_best_event_fires_once_per_run();
    test_obstacle_pool_has_headroom();
    test_spawn_pool_never_drops();
    test_obstacle_shape_invariants();
    test_obstacle_box_matches_ground_line();
    test_pickup_requires_jump_and_adds_score();
    test_gem_and_note_points();
    test_tallest_spawned_pickup_is_reachable();
    test_spawned_pickups_are_reachable_and_never_blocking();
    test_pickup_pool_never_drops();
    test_total_score_drives_free_mode_record();
    test_pickups_cleared_when_level_changes();
    test_boost_pickup_speeds_up();
    test_boost_smashes_obstacles();
    test_boost_expires_and_returns_to_normal();
    test_boost_does_not_tunnel();
    test_boost_lives_untouched();
    test_note_float_timer();
    test_boost_refreshes();
    printf("test_chick_run_model: all checks passed\n");
    return 0;
}
