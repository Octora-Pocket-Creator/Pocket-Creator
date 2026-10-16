// main/chick_run_model.c —— 小鸡快跑的物理、生成、碰撞与计分。
//
// 单位约定(见 chick_run_model.h):
//   位置/速度用"毫"单位累加,只有真正要用像素时才除以 1000,
//   这样 16ms 子步下低速度也不会被整数截断吃掉位移。
#include "chick_run_model.h"

#include <string.h>

// 障碍尺寸表:必须与美术尺寸一一相等(主机测试会核对),所见即所撞。
// 宽度比高度更要紧:障碍越宽,小鸡"脚底高于障碍顶部"的时间就越不够用。
// 最低速度(105px/s)下,32px 宽的栅栏横穿小鸡要 610ms,而当时跳跃只有 566ms
// 高于它 —— 那就是"怎么跳都过不去"。现在栅栏收到 20px、跳跃顶点 137px,
// 三种障碍都留出 130ms 以上余量,tests/test_chick_run_model.c 有不变量测试守住。
static const chick_obstacle_shape_t OBSTACLE_SHAPES[CHICK_OBSTACLE_KIND_COUNT] = {
    [CHICK_OBSTACLE_FENCE]  = { .w = 20, .h = 32, .y_bottom = 0 },
    [CHICK_OBSTACLE_CACTUS] = { .w = 20, .h = 40, .y_bottom = 0 },
    [CHICK_OBSTACLE_FOX]    = { .w = 26, .h = 26, .y_bottom = 0 },
};

// 生成权重:三种障碍难度接近(仙人掌最高、狐狸最宽),数量上基本均分。
static const uint8_t OBSTACLE_WEIGHTS[CHICK_OBSTACLE_KIND_COUNT] = { 35, 30, 35 };
#define OBSTACLE_WEIGHT_TOTAL 100

// 道具尺寸表:必须与美术尺寸一一相等(主机测试核对)。
// 16/16/20 写在这里而不是引用 chick_run_sprites.h —— 模型不依赖美术层,
// 两边一致靠 tests/test_chick_run_sprites.c 守。
static const chick_pickup_shape_t PICKUP_SHAPES[CHICK_PICKUP_KIND_COUNT] = {
    [CHICK_PICKUP_COIN] = { .w = 16, .h = 16 },
    [CHICK_PICKUP_GEM]  = { .w = 16, .h = 16 },
    [CHICK_PICKUP_NOTE] = { .w = 20, .h = 20 },
    [CHICK_PICKUP_BOOST] = { .w = 20, .h = 20 },
};

// 每个道具的分值(与 chick_run_model.h 的说明、HUD/结算显示保持一致)。
static const uint32_t PICKUP_POINTS[CHICK_PICKUP_KIND_COUNT] = {
    [CHICK_PICKUP_COIN] = CHICK_RUN_COIN_POINTS,
    [CHICK_PICKUP_GEM]  = CHICK_RUN_GEM_POINTS,
    [CHICK_PICKUP_NOTE] = CHICK_RUN_NOTE_POINTS,
    [CHICK_PICKUP_BOOST] = CHICK_RUN_BOOST_POINTS,
};

static uint32_t rng_next(chick_run_t *run) {
    uint32_t x = run->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    run->rng = x ? x : 0x9E3779B9u;   // xorshift 不允许退化为 0
    return run->rng;
}

static uint32_t rng_range(chick_run_t *run, uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + rng_next(run) % (hi - lo + 1u);
}

static chick_obstacle_kind_t rng_obstacle_kind(chick_run_t *run) {
    uint32_t roll = rng_next(run) % OBSTACLE_WEIGHT_TOTAL;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < CHICK_OBSTACLE_KIND_COUNT; i++) {
        acc += OBSTACLE_WEIGHTS[i];
        if (roll < acc) return (chick_obstacle_kind_t)i;
    }
    return CHICK_OBSTACLE_FENCE;
}

int32_t chick_run_level_speed_base(uint32_t level) {
    const uint32_t steps = level > 0 ? level - 1u : 0u;
    int32_t base = (int32_t)CHICK_RUN_SPEED_START + (int32_t)(steps * CHICK_RUN_LEVEL_SPEED_STEP);
    if (base > CHICK_RUN_SPEED_MAX) base = CHICK_RUN_SPEED_MAX;
    return base;
}

uint32_t chick_run_level_gap_min(uint32_t level) {
    const uint32_t steps = level > 0 ? level - 1u : 0u;
    const uint32_t cut = steps * CHICK_RUN_LEVEL_GAP_MIN_STEP;
    const uint32_t gap = cut < CHICK_RUN_GAP_MIN_MS ? CHICK_RUN_GAP_MIN_MS - cut : 0u;
    return gap > CHICK_RUN_LEVEL_GAP_MIN_FLOOR ? gap : CHICK_RUN_LEVEL_GAP_MIN_FLOOR;
}

uint32_t chick_run_level_gap_max(uint32_t level) {
    const uint32_t steps = level > 0 ? level - 1u : 0u;
    const uint32_t cut = steps * CHICK_RUN_LEVEL_GAP_MAX_STEP;
    const uint32_t gap = cut < CHICK_RUN_GAP_MAX_MS ? CHICK_RUN_GAP_MAX_MS - cut : 0u;
    return gap > CHICK_RUN_LEVEL_GAP_MAX_FLOOR ? gap : CHICK_RUN_LEVEL_GAP_MAX_FLOOR;
}

static void clear_obstacles(chick_run_t *run) {
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        run->obstacles[i].kind = CHICK_OBSTACLE_FENCE;
        run->obstacles[i].active = false;
        run->obstacles[i].x_milli = 0;
    }
}

static void clear_pickups(chick_run_t *run) {
    for (size_t i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        run->pickups[i].kind = CHICK_PICKUP_COIN;
        run->pickups[i].active = false;
        run->pickups[i].x_milli = 0;
        run->pickups[i].y_milli = 0;
    }
}

// 往道具池里放一个道具。池满只记账不静默丢弃:道具是加分项,
// 少一个不会让玩家送命,但"生成被吃掉"的手感问题要能在回归测试里看见。
static bool spawn_pickup_at(chick_run_t *run, chick_pickup_kind_t kind,
                            int32_t x_milli, int32_t y_milli) {
    // 兜底:贴地的道具会变成"必须撞障碍才能吃"的陷阱,这不是难度是 bug。
    if (y_milli < CHICK_RUN_PICKUP_MIN_Y * 1000) y_milli = CHICK_RUN_PICKUP_MIN_Y * 1000;
    for (size_t i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        chick_pickup_t *p = &run->pickups[i];
        if (p->active) continue;
        p->kind = kind;
        p->active = true;
        p->x_milli = x_milli;
        p->y_milli = y_milli;
        return true;
    }
    run->dropped_pickups++;
    return false;
}

// 道具排布:和障碍一起生成,所以"跳过障碍的那条抛物线"正好扫过道具 ——
// 玩家不需要额外的操作,跳起来就顺手吃到了(这正是需求里的玩法)。
static void spawn_pickup_pattern(chick_run_t *run, int32_t ob_x_milli, int32_t ob_w) {
    const int32_t center = ob_x_milli + ob_w * 500;   // 障碍中心(1/1000 px)
    // 权重:金币弧 39 / 后方三连 26 / 宝石 20 / 音符 10 / 加速护身符 5。
    // 音乐道具与加速符刻意做得稀疏(合计 15%):这两个是"大奖",吃到才有分量;
    // 音符比加速符高一点(10% vs 5%) —— 音乐是"好听一下",加速是直接改玩法。
    // 换算成体感:障碍大约 1.5 秒一个,所以音符大约每 15 秒出现一次。
    const uint32_t roll = rng_next(run) % 100u;
    if (roll < 39u) {
        // 三金币弧线:两边低、中间高,和跳跃轨迹同形状。
        static const int32_t dx[3] = { -18, 0, 18 };
        static const int32_t dy[3] = { 52, 80, 52 };
        const int32_t half = chick_run_pickup_shape(CHICK_PICKUP_COIN)->w * 500;
        for (int i = 0; i < 3; i++) {
            spawn_pickup_at(run, CHICK_PICKUP_COIN, center + dx[i] * 1000 - half,
                            dy[i] * 1000);
        }
    } else if (roll < 65u) {
        // 障碍后方的低空三连:跳过障碍、下落的过程中横穿过去。
        const int32_t first = ob_x_milli + (ob_w + 34) * 1000;
        const int32_t half = chick_run_pickup_shape(CHICK_PICKUP_COIN)->w * 500;
        for (int i = 0; i < 3; i++) {
            spawn_pickup_at(run, CHICK_PICKUP_COIN, first + (i * 24) * 1000 - half,
                            56 * 1000);
        }
    } else if (roll < 85u) {
        // 宝石:单个挂在跳跃顶点附近。比金币难拿,所以更值钱。
        const int32_t half = chick_run_pickup_shape(CHICK_PICKUP_GEM)->w * 500;
        spawn_pickup_at(run, CHICK_PICKUP_GEM, center - half, 88 * 1000);
    } else if (roll < 95u) {
        // 音符(音乐道具):稀有 —— 播一首世界名曲(《小星星》/《欢乐颂》),
        // 小鸡头顶还会飘一个 🎵。
        const int32_t half = chick_run_pickup_shape(CHICK_PICKUP_NOTE)->w * 500;
        spawn_pickup_at(run, CHICK_PICKUP_NOTE, center - half, 62 * 1000);
    } else {
        // 加速护身符:最稀有 —— 5.5 秒自动加速 + 撞碎障碍,很值钱。
        const int32_t half = chick_run_pickup_shape(CHICK_PICKUP_BOOST)->w * 500;
        spawn_pickup_at(run, CHICK_PICKUP_BOOST, center - half, 58 * 1000);
    }
}

static void spawn_obstacle(chick_run_t *run) {
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        chick_obstacle_t *ob = &run->obstacles[i];
        if (ob->active) continue;
        ob->kind = rng_obstacle_kind(run);
        ob->active = true;
        // 从屏幕右边缘外进场,保证玩家总能看到完整的滑入过程。
        ob->x_milli = (CHICK_RUN_SCREEN_W + 4) * 1000;
        spawn_pickup_pattern(run, ob->x_milli, chick_run_obstacle_shape(ob->kind)->w);
        return;
    }
    // 池满:记录而不是静默跳过,便于回归测试发现"障碍被吃掉"的手感问题。
    run->dropped_spawns++;
}

static int32_t current_speed(const chick_run_t *run) {
    uint32_t speed = (uint32_t)run->speed_base +
                     (run->elapsed_ms / 1000u) * (uint32_t)CHICK_RUN_SPEED_GAIN;
    if (speed > (uint32_t)CHICK_RUN_SPEED_MAX) speed = CHICK_RUN_SPEED_MAX;
    // 加速护身符:在普通封顶【之后】再乘倍率,所以普通速度上限不会把加速吃掉。
    if (run->boost_ms > 0) {
        speed = speed * (uint32_t)CHICK_RUN_BOOST_SPEED_PERMILLE / 1000u;
    }
    return (int32_t)speed;
}

const chick_obstacle_shape_t *chick_run_obstacle_shape(chick_obstacle_kind_t kind) {
    if (kind < 0 || kind >= CHICK_OBSTACLE_KIND_COUNT) return &OBSTACLE_SHAPES[0];
    return &OBSTACLE_SHAPES[kind];
}

const chick_pickup_shape_t *chick_run_pickup_shape(chick_pickup_kind_t kind) {
    if (kind < 0 || kind >= CHICK_PICKUP_KIND_COUNT) return &PICKUP_SHAPES[0];
    return &PICKUP_SHAPES[kind];
}

uint32_t chick_run_pickup_points(chick_pickup_kind_t kind) {
    if (kind < 0 || kind >= CHICK_PICKUP_KIND_COUNT) return 0;
    return PICKUP_POINTS[kind];
}

int32_t chick_run_pickup_x(const chick_pickup_t *pickup) {
    return pickup->x_milli / 1000;
}

int32_t chick_run_pickup_y(const chick_pickup_t *pickup) {
    return pickup->y_milli / 1000;
}

void chick_run_pickup_box(const chick_pickup_t *pickup, int32_t *left, int32_t *bottom,
                          int32_t *right, int32_t *top) {
    const chick_pickup_shape_t *shape = chick_run_pickup_shape(pickup->kind);
    *left = chick_run_pickup_x(pickup);
    *right = *left + shape->w;
    *bottom = chick_run_pickup_y(pickup);
    *top = *bottom + shape->h;
}

uint32_t chick_run_bonus_points(const chick_run_t *run) {
    return run->bonus_points;
}

bool chick_run_is_boosted(const chick_run_t *run) {
    return run->boost_ms > 0;
}

uint32_t chick_run_boost_seconds(const chick_run_t *run) {
    // 向上取整:吃到的那一瞬间 HUD 显示 6s,然后 5、4……而不是从 5 开始。
    return (run->boost_ms + 999u) / 1000u;
}

uint32_t chick_run_note_float_ms(const chick_run_t *run) {
    return run->note_ms;
}

uint32_t chick_run_smashed(const chick_run_t *run) {
    return run->smashed;
}

uint32_t chick_run_total_score(const chick_run_t *run) {
    return run->score_m + run->bonus_points;
}

void chick_run_init(chick_run_t *run, uint32_t seed) {
    memset(run, 0, sizeof(*run));
    run->state = CHICK_STATE_READY;
    run->seed = seed ? seed : 1u;
    run->rng = run->seed;
    run->on_ground = true;
    run->level = 1;
    run->speed_base = chick_run_level_speed_base(1);
    run->speed = run->speed_base;
    clear_obstacles(run);
}

void chick_run_set_best(chick_run_t *run, uint32_t best_m) {
    run->best_m = best_m;
}

void chick_run_set_best_level(chick_run_t *run, uint32_t best_level) {
    run->best_level = best_level;
}

uint32_t chick_run_level_progress_m(const chick_run_t *run) {
    return run->score_m;
}

bool chick_run_start(chick_run_t *run, chick_run_mode_t mode) {
    if (run->state == CHICK_STATE_RUNNING || run->state == CHICK_STATE_PAUSED) return false;
    if (mode >= CHICK_RUN_MODE_COUNT) mode = CHICK_RUN_MODE_FREE;

    const uint32_t seed = run->seed;
    const uint32_t best = run->best_m;
    const uint32_t best_level = run->best_level;
    chick_run_init(run, seed);
    run->best_m = best;
    run->best_level = best_level;
    run->mode = mode;
    run->new_best = false;
    run->state = CHICK_STATE_RUNNING;
    run->speed = run->speed_base;
    run->lives = mode == CHICK_RUN_MODE_LEVEL ? CHICK_RUN_LEVEL_LIVES : 1;
    run->next_gap_ms = 1600;             // 开局留足反应时间
    run->milestone_m = CHICK_RUN_MILESTONE_M;
    return true;
}

bool chick_run_jump(chick_run_t *run) {
    if (run->state != CHICK_STATE_RUNNING || !run->on_ground) return false;
    run->on_ground = false;
    run->jump_held = true;
    run->jump_hold_ms = 0;
    run->vy_milli = CHICK_RUN_JUMP_V * 1000;
    run->chick_y_milli = 1;              // 立刻离地,避免同一步又判定落地
    return true;
}

void chick_run_jump_release(chick_run_t *run) {
    run->jump_held = false;
}

void chick_run_slide(chick_run_t *run) {
    if (run->state != CHICK_STATE_RUNNING) return;
    run->slide_ms = CHICK_RUN_SLIDE_MS;
}

bool chick_run_toggle_pause(chick_run_t *run) {
    if (run->state == CHICK_STATE_RUNNING) {
        run->state = CHICK_STATE_PAUSED;
        return true;
    }
    if (run->state == CHICK_STATE_PAUSED) {
        run->state = CHICK_STATE_RUNNING;
        return true;
    }
    return false;
}

void chick_run_abort(chick_run_t *run) {
    uint32_t best = run->best_m;
    uint32_t best_level = run->best_level;
    uint32_t seed = run->seed;
    chick_run_init(run, seed);
    run->best_m = best;
    run->best_level = best_level;
    clear_obstacles(run);
}

int32_t chick_run_chick_y(const chick_run_t *run) {
    return run->chick_y_milli / 1000;
}

int32_t chick_run_chick_height(const chick_run_t *run) {
    if (run->ducking && run->on_ground) return CHICK_RUN_CHICK_DUCK_H;
    return CHICK_RUN_CHICK_H;
}

int32_t chick_run_obstacle_x(const chick_obstacle_t *ob) {
    return ob->x_milli / 1000;
}

bool chick_run_obstacle_box(const chick_obstacle_t *ob, int32_t *left, int32_t *top,
                            int32_t *right, int32_t *bottom) {
    if (!ob->active) return false;
    const chick_obstacle_shape_t *shape = chick_run_obstacle_shape(ob->kind);
    *left = chick_run_obstacle_x(ob);
    *right = *left + shape->w;
    *bottom = (int32_t)CHICK_RUN_GROUND_Y - shape->y_bottom;
    *top = *bottom - shape->h;
    return true;
}

bool chick_run_is_new_best(const chick_run_t *run) {
    return run->new_best;
}

// 小鸡与单个障碍的 AABB 相交判定(半开区间,边缘相切不算碰撞)。
static bool chick_hits_one(const chick_run_t *run, const chick_obstacle_t *ob) {
    const int32_t chick_left = CHICK_RUN_CHICK_X;
    const int32_t chick_right = chick_left + CHICK_RUN_CHICK_W;
    const int32_t chick_bottom = (int32_t)CHICK_RUN_GROUND_Y - chick_run_chick_y(run);
    const int32_t chick_top = chick_bottom - chick_run_chick_height(run);

    int32_t left, top, right, bottom;
    if (!chick_run_obstacle_box(ob, &left, &top, &right, &bottom)) return false;
    return chick_left < right && left < chick_right &&
           chick_top < bottom && top < chick_bottom;
}

static bool chick_hits_obstacle(const chick_run_t *run) {
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        if (chick_hits_one(run, &run->obstacles[i])) return true;
    }
    return false;
}

// 加速状态下的冲撞:把这一子步里碰到的障碍全部撞碎(可能一下撞碎好几个),
// 障碍直接消失、小鸡不掉命也不减速 —— 这就是用户要的"冲破栅栏"。
static uint32_t smash_obstacles(chick_run_t *run) {
    uint32_t events = CHICK_EV_NONE;
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        chick_obstacle_t *ob = &run->obstacles[i];
        if (!ob->active || !chick_hits_one(run, ob)) continue;
        ob->active = false;
        run->smashed++;
        events |= CHICK_EV_SMASH;
    }
    return events;
}

// 吃到道具：AABB 相交（半开区间，边缘相切不算）。坐标系用"离地高度"，
// 和 chick_y_milli 同一基准，避免地面线上下各算一次。
static bool chick_touches_pickup(const chick_run_t *run, const chick_pickup_t *pickup) {
    // 统一用 1/1000 px：道具位置本来就是毫单位，横向这一侧也要跟上，
    // 否则就会拿"像素"和"毫"相比 —— 那个 bug 的表现是"道具穿身而过却吃不到"。
    const int32_t chick_left = CHICK_RUN_CHICK_X * 1000;
    const int32_t chick_right = (CHICK_RUN_CHICK_X + CHICK_RUN_CHICK_W) * 1000;
    const int32_t chick_low = run->chick_y_milli;
    const int32_t chick_high = chick_low + chick_run_chick_height(run) * 1000;

    const chick_pickup_shape_t *shape = chick_run_pickup_shape(pickup->kind);
    const int32_t p_left = pickup->x_milli;
    const int32_t p_right = p_left + shape->w * 1000;
    const int32_t p_low = pickup->y_milli;
    const int32_t p_high = p_low + shape->h * 1000;

    return chick_left < p_right && p_left < chick_right &&
           chick_low < p_high && p_low < chick_high;
}

// 本子步吃到的东西：加分 + 计数 + 交给 UI 的事件位。
static uint32_t collect_pickups(chick_run_t *run) {
    uint32_t events = CHICK_EV_NONE;
    for (size_t i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        chick_pickup_t *pickup = &run->pickups[i];
        if (!pickup->active || !chick_touches_pickup(run, pickup)) continue;
        pickup->active = false;
        run->bonus_points += chick_run_pickup_points(pickup->kind);
        switch (pickup->kind) {
            case CHICK_PICKUP_GEM:
                run->gems++;
                events |= CHICK_EV_GEM;
                break;
            case CHICK_PICKUP_NOTE:
                run->notes++;
                run->note_ms = CHICK_RUN_NOTE_FLOAT_MS;   // 头顶飘 🎵,与旋律同长
                events |= CHICK_EV_NOTE;
                break;
            case CHICK_PICKUP_BOOST:
                run->boosts++;
                run->boost_ms = CHICK_RUN_BOOST_MS;       // 再吃一个直接刷新时长
                // 同一步就把速度刷新:UI 的装饰滚动读的就是 run->speed。
                run->speed = current_speed(run);
                events |= CHICK_EV_BOOST;
                break;
            case CHICK_PICKUP_COIN:
            default:
                run->coins++;
                events |= CHICK_EV_COIN;
                break;
        }
    }
    return events;
}

// 自由模式的纪录以【总得分】（距离 + 道具加分）为准：金币/宝石/音符都能推进纪录。
// best_m 会被抬到当前总得分，所以 "total > best_m" 一旦破纪录就每加 1 分都成立；
// 事件必须只在"本局第一次超过开局时的最高分"发一次，否则纪录音效会被以每分一次的
// 频率反复重放 —— 听感就是一段连续的、类似马里奥顶砖块的碎音。
// 关卡模式的最高成绩是"第几关"，不走这里。
static uint32_t refresh_best(chick_run_t *run) {
    if (run->mode != CHICK_RUN_MODE_FREE) return CHICK_EV_NONE;
    const uint32_t total = chick_run_total_score(run);
    if (total <= run->best_m) return CHICK_EV_NONE;
    uint32_t events = CHICK_EV_NONE;
    if (!run->new_best) {
        run->new_best = true;
        events |= CHICK_EV_NEW_BEST;
    }
    run->best_m = total;
    return events;
}

// 推进一个子步。返回该子步产生的事件位。
static uint32_t step_once(chick_run_t *run, uint32_t step_ms) {
    uint32_t events = CHICK_EV_NONE;

    // 计时先走:本帧的速度倍率、无敌判定都由"扣完时间"的状态决定。
    // 顺序反过来的话,加速结束的那一子步还会多冲一帧,加速的无敌也会多留一帧。
    if (run->invincible_ms > step_ms) run->invincible_ms -= step_ms;
    else run->invincible_ms = 0;
    if (run->boost_ms > step_ms) run->boost_ms -= step_ms;
    else run->boost_ms = 0;
    if (run->note_ms > step_ms) run->note_ms -= step_ms;
    else run->note_ms = 0;

    run->elapsed_ms += step_ms;
    run->speed = current_speed(run);

    // 滑铲计时(先于物理与碰撞:本帧的碰撞盒高度由它决定)。
    if (run->slide_ms > step_ms) {
        run->slide_ms -= step_ms;
    } else {
        run->slide_ms = 0;
    }
    run->ducking = run->slide_ms > 0;

    // 垂直运动:水平位移与生成节奏无关,先算清楚再统一移动障碍。
    if (!run->on_ground) {
        int32_t gravity = CHICK_RUN_GRAVITY;
        if (run->jump_held && run->vy_milli > 0 && run->jump_hold_ms < CHICK_RUN_HOLD_MS) {
            gravity = CHICK_RUN_HOLD_GRAVITY;
            run->jump_hold_ms += step_ms;
        }
        if (run->ducking) gravity *= CHICK_RUN_FALL_MULT;

        run->vy_milli -= gravity * (int32_t)step_ms;
        run->chick_y_milli += run->vy_milli * (int32_t)step_ms / 1000;
        if (run->chick_y_milli <= 0) {
            run->chick_y_milli = 0;
            run->vy_milli = 0;
            run->on_ground = true;
            run->jump_held = false;
            events |= CHICK_EV_LAND;
        }
    } else {
        run->chick_y_milli = 0;
        run->vy_milli = 0;
    }

    // 水平推进:障碍左移,距离累加。
    const int32_t advance_milli = run->speed * (int32_t)step_ms;   // px/s * ms = 1/1000 px
    run->distance_milli += advance_milli;
    for (size_t i = 0; i < CHICK_RUN_MAX_OBSTACLES; i++) {
        chick_obstacle_t *ob = &run->obstacles[i];
        if (!ob->active) continue;
        ob->x_milli -= advance_milli;
        if (ob->x_milli < -(int32_t)(chick_run_obstacle_shape(ob->kind)->w + 4) * 1000) {
            ob->active = false;
        }
    }
    // 道具和障碍是同一个世界速度，一起左移、一起出场。
    for (size_t i = 0; i < CHICK_RUN_MAX_PICKUPS; i++) {
        chick_pickup_t *pickup = &run->pickups[i];
        if (!pickup->active) continue;
        pickup->x_milli -= advance_milli;
        if (pickup->x_milli < -(int32_t)(chick_run_pickup_shape(pickup->kind)->w + 4) * 1000) {
            pickup->active = false;
        }
    }

    // 生成节奏按"时间"而不是"距离":速度越快离得越密,但反应窗口始终够用。
    if (run->next_gap_ms <= step_ms) {
        spawn_obstacle(run);
        run->next_gap_ms = rng_range(run, chick_run_level_gap_min(run->level),
                                     chick_run_level_gap_max(run->level));
    } else {
        run->next_gap_ms -= step_ms;
    }

    // 计分与里程碑。关卡模式的进度以"本关起点"为基准,所以过关时把基准前移即可,
    // 不用清空累计距离(那会吃掉不足 1 米的余量,进度条会一格一格跳)。
    const uint32_t distance_m =
        (uint32_t)((run->distance_milli - run->level_base_milli) / 1000) / CHICK_RUN_PX_PER_M;
    if (distance_m != run->score_m) {
        run->score_m = distance_m;
        if (run->score_m >= run->milestone_m) {
            events |= CHICK_EV_MILESTONE;
            run->milestone_m += CHICK_RUN_MILESTONE_M;
        }
        events |= refresh_best(run);
    }

    // 吃道具：跳过障碍的那条抛物线正好扫过金币弧线，所以玩家不用额外操作。
    // 放在碰撞判定之前：同一子步里"吃到最后一枚金币"和"撞上障碍"应该都算。
    events |= collect_pickups(run);
    events |= refresh_best(run);

    // 过关:达到每关目标就切下一关(先于碰撞判定 —— 踩线过关不该被障碍吃掉)。
    if (run->mode == CHICK_RUN_MODE_LEVEL && run->score_m >= CHICK_RUN_LEVEL_TARGET_M) {
        run->level++;
        run->level_base_milli +=
            (int32_t)CHICK_RUN_LEVEL_TARGET_M * CHICK_RUN_PX_PER_M * 1000;
        run->score_m =
            (uint32_t)((run->distance_milli - run->level_base_milli) / 1000) / CHICK_RUN_PX_PER_M;
        run->speed_base = chick_run_level_speed_base(run->level);
        run->speed = run->speed_base;
        run->elapsed_ms = 0;                       // 每关重新开始提速
        run->milestone_m = CHICK_RUN_MILESTONE_M;
        // 命数是整局共用的,过关不补:3 条命撑完每一关才算过关。
        run->invincible_ms = 0;                    // 上一关的无敌不带进新关
        if (run->level > run->best_level) run->best_level = run->level;
        clear_obstacles(run);
        clear_pickups(run);                      // 新关不继承上一关剩下的道具
        run->next_gap_ms = 1600;                   // 新关开头同样留反应时间
        events |= CHICK_EV_LEVEL_UP;
    }

    if (run->boost_ms > 0) {
        // 加速中:碰到的障碍被撞碎(而不是撞死);不掉命、不停下。
        events |= smash_obstacles(run);
    } else if (run->invincible_ms == 0 && chick_hits_obstacle(run)) {
        if (run->mode == CHICK_RUN_MODE_LEVEL && run->lives > 1) {
            // 还有多余备命:掉一条命,跑动继续,靠无敌时间穿过去。
            run->lives--;
            run->invincible_ms = CHICK_RUN_HURT_INVINCIBLE_MS;
            events |= CHICK_EV_HIT | CHICK_EV_HURT;
        } else {
            run->state = CHICK_STATE_GAME_OVER;
            run->jump_held = false;
            if (run->mode == CHICK_RUN_MODE_LEVEL) {
                run->lives = 0;
                if (run->level > run->best_level) run->best_level = run->level;
            }
            events |= CHICK_EV_HIT | CHICK_EV_GAME_OVER;
        }
    }
    return events;
}

uint32_t chick_run_tick(chick_run_t *run, uint32_t dt_ms) {
    if (run->state != CHICK_STATE_RUNNING) return CHICK_EV_NONE;

    uint32_t events = CHICK_EV_NONE;
    uint32_t remaining = dt_ms;
    while (remaining > 0) {
        uint32_t step = remaining > CHICK_RUN_STEP_MS ? CHICK_RUN_STEP_MS : remaining;
        remaining -= step;
        events |= step_once(run, step);
        if (run->state != CHICK_STATE_RUNNING) break;
    }
    return events;
}
