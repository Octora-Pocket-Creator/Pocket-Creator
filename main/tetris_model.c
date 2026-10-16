// main/tetris_model.c —— 俄罗斯方块规则实现(纯逻辑,主机可测)。
//
// 形状表是 4x4 包围盒的位掩码:bit (y*4+x)。四行分别对应 4 个旋转态,
// 不做矩阵旋转而是直接查表 —— 形状永远只可能是这 28 个,查表既快又不会算错。
#include "tetris_model.h"

#include <string.h>

// 标准 7 种方块(SRS 的形状,不含 SRS 的精确踢墙表 —— 见 rotate 的说明)。
static const uint16_t SHAPES[TETRIS_PIECE_COUNT][4] = {
    { 0x00F0, 0x4444, 0x0F00, 0x2222 },   // I
    { 0x0066, 0x0066, 0x0066, 0x0066 },   // O
    { 0x0072, 0x0262, 0x0270, 0x0232 },   // T
    { 0x0036, 0x0462, 0x0360, 0x0231 },   // S
    { 0x0063, 0x0264, 0x0630, 0x0132 },   // Z
    { 0x0071, 0x0226, 0x0470, 0x0322 },   // J
    { 0x0074, 0x0622, 0x0170, 0x0223 },   // L
};

// 生成位置:x = 3 让 4 宽的形状居中(10 列,左右各留 3 列)。
#define SPAWN_X 3
// 每行得分(第 0 项占位):1 行 100、2 行 300、3 行 500、4 行 800,再乘等级。
static const uint16_t LINE_SCORE[5] = { 0, 100, 300, 500, 800 };

static void events_reset(tetris_events_t *ev) {
    if (ev != NULL) memset(ev, 0, sizeof(*ev));
}

// --- 随机与队列 -------------------------------------------------------------
// xorshift32:够随机、状态只有 4 字节、在主机上可用固定种子复现同一串方块。
static uint32_t rand_next(tetris_model_t *m) {
    uint32_t x = m->rng ? m->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x;
    return x;
}

// 7-bag:一袋里 7 种各一个并洗牌。比"每步均匀随机"手感好得多
// (均匀随机会出现连续 6 个 S 这种让人骂街的序列)。
static tetris_piece_t bag_take(tetris_model_t *m) {
    if (m->bag_n == 0) {
        for (uint8_t i = 0; i < TETRIS_PIECE_COUNT; i++) m->bag[i] = (tetris_piece_t)i;
        for (int i = TETRIS_PIECE_COUNT - 1; i > 0; i--) {
            const int j = (int)(rand_next(m) % (uint32_t)(i + 1));
            const tetris_piece_t tmp = m->bag[i];
            m->bag[i] = m->bag[j];
            m->bag[j] = tmp;
        }
        m->bag_n = TETRIS_PIECE_COUNT;
    }
    return m->bag[--m->bag_n];
}

static void queue_fill(tetris_model_t *m) {
    for (int i = 0; i <= TETRIS_PREVIEW; i++) m->queue[i] = bag_take(m);
}

static void queue_pop(tetris_model_t *m) {
    for (int i = 0; i < TETRIS_PREVIEW; i++) m->queue[i] = m->queue[i + 1];
    m->queue[TETRIS_PREVIEW] = bag_take(m);
}

// --- 形状与碰撞 -------------------------------------------------------------
bool tetris_piece_cell(tetris_piece_t piece, uint8_t rotation, int x, int y) {
    if ((int)piece < 0 || piece >= TETRIS_PIECE_COUNT) return false;
    if (x < 0 || x >= 4 || y < 0 || y >= 4) return false;
    return (SHAPES[piece][rotation & 3U] & (uint16_t)(1U << (y * 4 + x))) != 0;
}

// 是否与墙壁/已固定的方块相撞。y < 0 视为在屏幕上方(合法,方块可以"半出"顶部)。
static bool collides(const tetris_model_t *m, tetris_piece_t piece, uint8_t rotation, int x,
                     int y) {
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!tetris_piece_cell(piece, rotation, px, py)) continue;
            const int bx = x + px;
            const int by = y + py;
            if (bx < 0 || bx >= TETRIS_COLS) return true;
            if (by >= TETRIS_ROWS) return true;
            if (by >= 0 && m->board[by][bx] != 0) return true;
        }
    }
    return false;
}

static void ghost_update(tetris_model_t *m) {
    int y = m->y;
    if (m->active) {
        while (y + 1 < TETRIS_ROWS && !collides(m, m->piece, m->rotation, m->x, y + 1)) y++;
    }
    m->ghost_y = (int8_t)y;
}

// 生成下一个方块。无法生成(= 出生点被占)即游戏结束。
static bool spawn(tetris_model_t *m, tetris_events_t *ev) {
    m->piece = m->queue[0];
    queue_pop(m);
    m->rotation = 0;
    m->x = SPAWN_X;
    m->y = 0;
    m->lock_ms = 0;
    m->lock_resets = 0;
    m->gravity_acc_ms = 0;
    m->active = true;
    ghost_update(m);
    if (collides(m, m->piece, m->rotation, m->x, m->y)) {
        m->active = false;
        m->state = TETRIS_STATE_OVER;
        if (ev != NULL) ev->game_over = true;
        return false;
    }
    return true;
}

// --- 计分 -------------------------------------------------------------------
uint32_t tetris_gravity_ms(uint8_t level) {
    // 每级 70ms,最低 80ms —— 再快就不是"难"而是"没法玩"了。
    const uint32_t step = level > 0 ? (uint32_t)(level - 1u) * 70u : 0u;
    return step >= 720u ? 80u : 800u - step;
}

// 所有加分都走这里:顺便维护"本局是否破纪录"的标记(UI 结算页要用)。
static void score_add(tetris_model_t *m, uint32_t points) {
    m->score += points;
    if (m->score > m->best) m->record = true;
}

static uint8_t level_of(uint16_t lines) {
    const uint8_t level = (uint8_t)(lines / 10u + 1u);
    return level > TETRIS_MAX_LEVEL ? TETRIS_MAX_LEVEL : level;
}

void tetris_begin(tetris_model_t *m, uint32_t seed) {
    const uint32_t best = m->best;
    memset(m, 0, sizeof(*m));
    m->best = best;
    m->rng = seed ? seed : 0x2545F491u;
    m->state = TETRIS_STATE_PLAYING;
    m->level = 1;
    queue_fill(m);
    spawn(m, NULL);
}

void tetris_set_best(tetris_model_t *m, uint32_t best) {
    m->best = best;
    if (m->score > m->best) m->record = true;
}

// --- 输入 -------------------------------------------------------------------
static void lock_reset(tetris_model_t *m) {
    // "移动重置":贴地后靠移动/旋转还能拖延一会儿,但不能无限拖(否则可以原地
    // 无限转圈)。超过次数后不再重置,锁定延迟照常走完。
    if (m->lock_resets >= TETRIS_LOCK_RESETS) return;
    m->lock_resets++;
    m->lock_ms = 0;
}

bool tetris_move(tetris_model_t *m, int dx, tetris_events_t *ev) {
    events_reset(ev);
    if (m->state != TETRIS_STATE_PLAYING || !m->active) return false;
    if (dx != -1 && dx != 1) return false;
    if (collides(m, m->piece, m->rotation, m->x + dx, m->y)) return false;
    m->x = (int8_t)(m->x + dx);
    lock_reset(m);
    ghost_update(m);
    if (ev != NULL) ev->moved = true;
    return true;
}

bool tetris_rotate(tetris_model_t *m, int dir, tetris_events_t *ev) {
    events_reset(ev);
    if (m->state != TETRIS_STATE_PLAYING || !m->active) return false;
    if (dir != 1 && dir != -1) return false;
    const uint8_t next = (uint8_t)((m->rotation + (dir > 0 ? 1 : 3)) & 3U);
    // 踢墙:先试原地,再左右各 1、左右各 2 —— 够应付贴墙和贴别的方块的情况,
    // 又不做 SRS 的车轮踢(那种踢法在 10 列的小屏上收益很小,反而更容易转错)。
    static const int8_t KICK[] = { 0, -1, 1, -2, 2 };
    for (size_t i = 0; i < sizeof(KICK) / sizeof(KICK[0]); i++) {
        const int nx = m->x + KICK[i];
        if (collides(m, m->piece, next, nx, m->y)) continue;
        m->rotation = next;
        m->x = (int8_t)nx;
        lock_reset(m);
        ghost_update(m);
        if (ev != NULL) ev->rotated = true;
        return true;
    }
    return false;
}

void tetris_set_soft_drop(tetris_model_t *m, bool on) {
    m->soft_drop = on;
}

bool tetris_toggle_pause(tetris_model_t *m) {
    if (m->state == TETRIS_STATE_PLAYING) {
        m->state = TETRIS_STATE_PAUSED;
        m->soft_drop = false;          // 暂停时别让"下键按住"延续到恢复之后
        return true;
    }
    if (m->state == TETRIS_STATE_PAUSED) {
        m->state = TETRIS_STATE_PLAYING;
        m->gravity_acc_ms = 0;
        return false;
    }
    return m->state == TETRIS_STATE_PAUSED;
}

// --- 落子与消行 -------------------------------------------------------------
// 把方块写进棋盘。返回是否有格子落在棋盘上方(顶到天花板 = 立即结束)。
static bool lock_into_board(tetris_model_t *m) {
    bool above = false;
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!tetris_piece_cell(m->piece, m->rotation, px, py)) continue;
            const int bx = m->x + px;
            const int by = m->y + py;
            if (by < 0) {
                above = true;
            } else if (bx >= 0 && bx < TETRIS_COLS && by < TETRIS_ROWS) {
                m->board[by][bx] = (uint8_t)(m->piece + 1);
            }
        }
    }
    return above;
}

// 标记消行动画。整行留在棋盘上继续闪,动画结束才真正删除(见 tick)。
static void start_clear(tetris_model_t *m, uint32_t rows, uint8_t count, tetris_events_t *ev) {
    m->clear_rows = rows;
    m->clear_count = count;
    m->clear_ms = 0;
    m->active = false;
    m->state = TETRIS_STATE_CLEARING;
    if (ev != NULL) ev->clear_begin = true;
}

static void do_lock(tetris_model_t *m, tetris_events_t *ev) {
    m->active = false;
    if (lock_into_board(m)) {
        // 有格子落在棋盘上方:顶死。此时仍然把它固定下来,让玩家看到是哪一格堵死的。
        m->state = TETRIS_STATE_OVER;
        if (ev != NULL) ev->game_over = true;
        return;
    }
    if (ev != NULL) ev->locked = true;

    uint32_t rows = 0;
    uint8_t count = 0;
    for (int y = 0; y < TETRIS_ROWS; y++) {
        bool full = true;
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (m->board[y][x] == 0) { full = false; break; }
        }
        if (full) { rows |= (uint32_t)1u << y; count++; }
    }
    if (count > 0) {
        start_clear(m, rows, count, ev);
        return;
    }
    spawn(m, ev);
}

// 动画结束:删掉整行、上面的行整体下移、结算分数与等级。
static void finish_clear(tetris_model_t *m, tetris_events_t *ev) {
    const uint32_t rows = m->clear_rows;
    const uint8_t count = m->clear_count;
    int write = TETRIS_ROWS - 1;
    for (int y = TETRIS_ROWS - 1; y >= 0; y--) {
        if (rows & ((uint32_t)1u << y)) continue;
        if (write != y) memcpy(m->board[write], m->board[y], TETRIS_COLS);
        write--;
    }
    for (int y = write; y >= 0; y--) memset(m->board[y], 0, TETRIS_COLS);

    score_add(m, (uint32_t)LINE_SCORE[count > 4 ? 4 : count] * (uint32_t)m->level);
    const uint16_t before = m->lines;
    m->lines = (uint16_t)(m->lines + count);
    const uint8_t level_before = m->level;
    m->level = level_of(m->lines);

    m->clear_rows = 0;
    m->clear_count = 0;
    m->clear_ms = 0;
    m->state = TETRIS_STATE_PLAYING;
    if (ev != NULL) {
        ev->lines_cleared = count;
        ev->level_up = m->level != level_before;
    }
    (void)before;
    spawn(m, ev);
}

bool tetris_hard_drop(tetris_model_t *m, tetris_events_t *ev) {
    events_reset(ev);
    if (m->state != TETRIS_STATE_PLAYING || !m->active) return false;
    int distance = 0;
    while (!collides(m, m->piece, m->rotation, m->x, m->y + 1)) {
        m->y++;
        distance++;
    }
    // 硬降每格 2 分:鼓励果断落子,但比消行的收益低得多。
    score_add(m, (uint32_t)distance * 2u);
    m->gravity_acc_ms = 0;
    if (ev != NULL) ev->dropped = true;
    do_lock(m, ev);
    return true;
}

// --- 帧推进 -----------------------------------------------------------------
void tetris_tick(tetris_model_t *m, uint32_t dt_ms, tetris_events_t *ev) {
    events_reset(ev);
    if (m->state == TETRIS_STATE_PAUSED || m->state == TETRIS_STATE_OVER) return;
    if (dt_ms > TETRIS_MAX_DT_MS) dt_ms = TETRIS_MAX_DT_MS;

    if (m->state == TETRIS_STATE_CLEARING) {
        m->clear_ms = (uint16_t)(m->clear_ms + dt_ms);
        if (m->clear_ms >= TETRIS_CLEAR_MS) finish_clear(m, ev);
        return;
    }

    if (!m->active) return;

    // 是否已贴地:贴地后开始走锁定延迟,不贴地则清零。
    const bool on_floor = collides(m, m->piece, m->rotation, m->x, m->y + 1);
    if (on_floor) {
        m->lock_ms = (uint16_t)(m->lock_ms + dt_ms);
        if (m->lock_ms >= TETRIS_LOCK_MS) {
            m->lock_ms = TETRIS_LOCK_MS;
            do_lock(m, ev);
            return;
        }
    } else {
        m->lock_ms = 0;
        m->lock_resets = 0;
    }

    // 重力:按住下键时用软降间隔。软降每下落一格 +1 分。
    const uint32_t interval = m->soft_drop
                                  ? TETRIS_SOFT_DROP_MS
                                  : tetris_gravity_ms(m->level);
    m->gravity_acc_ms += dt_ms;
    while (m->gravity_acc_ms >= interval) {
        m->gravity_acc_ms -= interval;
        if (collides(m, m->piece, m->rotation, m->x, m->y + 1)) {
            m->gravity_acc_ms = 0;   // 贴地:时间交给锁定延迟,别积攒成"一落地就锁"
            break;
        }
        m->y++;
        if (m->soft_drop) score_add(m, 1u);
        m->lock_ms = 0;
        m->lock_resets = 0;
        ghost_update(m);
    }
}

// --- 查询 -------------------------------------------------------------------
uint8_t tetris_cell_at(const tetris_model_t *m, int x, int y) {
    if (x < 0 || x >= TETRIS_COLS || y < 0 || y >= TETRIS_ROWS) return 0;
    if (m->board[y][x] != 0) return m->board[y][x];
    if (m->active) {
        // 棋盘上方的格子(y<0)画不到,这里只处理可见区域。
        const int px = x - m->x;
        const int py = y - m->y;
        if (px >= 0 && px < 4 && py >= 0 && py < 4 &&
            tetris_piece_cell(m->piece, m->rotation, px, py)) {
            return (uint8_t)(m->piece + 1);
        }
    }
    return 0;
}

int tetris_ghost_y(const tetris_model_t *m) {
    return m->ghost_y;
}

tetris_piece_t tetris_queue_at(const tetris_model_t *m, int index) {
    if (index < 0) index = 0;
    if (index > TETRIS_PREVIEW) index = TETRIS_PREVIEW;
    return m->queue[index];
}

bool tetris_row_clearing(const tetris_model_t *m, int row) {
    if (row < 0 || row >= TETRIS_ROWS) return false;
    return (m->clear_rows & ((uint32_t)1u << row)) != 0;
}

uint8_t tetris_clear_flash(const tetris_model_t *m) {
    if (m->state != TETRIS_STATE_CLEARING) return 0;
    // 三角波:0 → 255 → 0,周期 120ms。留一点余量让最后一刻不是刚好最亮。
    const uint16_t period = 60;
    const uint16_t phase = (uint16_t)(m->clear_ms % (period * 2u));
    const uint16_t v = phase < period ? phase : (uint16_t)(period * 2u - phase);
    return (uint8_t)(v * 255u / period);
}
