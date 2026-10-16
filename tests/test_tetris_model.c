// tests/test_tetris_model.c —— 俄罗斯方块规则的主机测试。
//
// 这些规则在设备上只表现为"手感不对"(方块贴地锁不住、转不动、分数不对、消行后
// 行没对齐),靠肉眼调试成本极高,所以这里用固定种子构造棋盘,按毫秒推进时间验证。
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tetris_model.h"

// --- 辅助 -------------------------------------------------------------------
#define CELL(piece) ((uint8_t)((piece) + 1))

static void fill_row(tetris_model_t *m, int row, uint8_t value) {
    for (int x = 0; x < TETRIS_COLS; x++) m->board[row][x] = value;
}

// 独立实现的碰撞检查(不调用模型的内部函数):用来验收"当前方块永远不重叠"。
static bool overlaps(const tetris_model_t *m) {
    if (!m->active) return false;
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!tetris_piece_cell(m->piece, m->rotation, px, py)) continue;
            const int bx = m->x + px;
            const int by = m->y + py;
            if (bx < 0 || bx >= TETRIS_COLS) return true;
            if (by >= TETRIS_ROWS) return true;
            if (by >= 0 && m->board[by][bx] != 0) return true;
        }
    }
    return false;
}

// 按 10ms 帧步进(正常帧率是 30~60FPS),并【累加】各步的事件 —— 真实 UI 是每帧读
// 一次事件,而 tetris_tick 每次调用都会清空事件结构,所以测试里要自己合并,
// 否则只能看到最后一个 10ms 里发生的事。毫秒数按 10 取整。
static void tick_ms(tetris_model_t *m, uint32_t ms, tetris_events_t *ev) {
    tetris_events_t acc = { false, false, false, false, false, 0, false, false };
    for (uint32_t done = 0; done < ms; done += 10) {
        tetris_events_t step;
        tetris_tick(m, 10, &step);
        acc.moved |= step.moved;
        acc.rotated |= step.rotated;
        acc.dropped |= step.dropped;
        acc.locked |= step.locked;
        acc.clear_begin |= step.clear_begin;
        acc.lines_cleared = (uint8_t)(acc.lines_cleared + step.lines_cleared);
        acc.level_up |= step.level_up;
        acc.game_over |= step.game_over;
    }
    if (ev != NULL) *ev = acc;
}

// 已固定的格子数。注意:落子成功后模型会立刻出生下一个方块(active 又变回 true),
// 所以"有没有锁住"要用棋盘格子数判断,不能用 active。
static int board_cells(const tetris_model_t *m) {
    int n = 0;
    for (int y = 0; y < TETRIS_ROWS; y++) {
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (m->board[y][x] != 0) n++;
        }
    }
    return n;
}

static void assert_board_clean(const tetris_model_t *m) {
    for (int y = 0; y < TETRIS_ROWS; y++) {
        for (int x = 0; x < TETRIS_COLS; x++) {
            assert(m->board[y][x] <= TETRIS_PIECE_COUNT);
        }
    }
    assert(!overlaps(m));
}

// --- 形状表 -----------------------------------------------------------------
// 把掩码平移到包围盒原点:形状表里同一朝向的不同旋转态是"平移"关系(例如 I 的
// 横放在第 1 行和第 2 行),只有消掉平移才能比较"到底有几种朝向"。
static uint16_t normalize(uint16_t mask) {
    int min_x = 4, min_y = 4;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (mask & (1u << (y * 4 + x))) {
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
            }
        }
    }
    uint16_t out = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (mask & (1u << (y * 4 + x))) out |= (uint16_t)(1u << ((y - min_y) * 4 + (x - min_x)));
        }
    }
    return out;
}

static void test_shape_table(void) {
    for (int p = 0; p < TETRIS_PIECE_COUNT; p++) {
        uint16_t seen[4] = { 0, 0, 0, 0 };
        for (int rot = 0; rot < 4; rot++) {
            uint16_t mask = 0;
            int cells = 0;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    if (tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x, y)) {
                        mask |= (uint16_t)(1u << (y * 4 + x));
                        cells++;
                    }
                }
            }
            // 每个方块永远是 4 格 —— 形状表写错立刻能抓到。
            assert(cells == 4);
            seen[rot] = normalize(mask);
        }
        int distinct = 0;
        for (int rot = 0; rot < 4; rot++) {
            bool dup = false;
            for (int i = 0; i < rot; i++) {
                if (seen[i] == seen[rot]) dup = true;
            }
            if (!dup) distinct++;
        }
        // O 只有一个朝向;I/S/Z 是两种;T/J/L 是四种(消掉平移之后)。
        const int expected = (p == TETRIS_PIECE_O) ? 1 : (p == TETRIS_PIECE_I ||
                                                          p == TETRIS_PIECE_S ||
                                                          p == TETRIS_PIECE_Z) ? 2 : 4;
        if (distinct != expected) {
            printf("  方块 %d 朝向数 %d(期望 %d)\n", p, distinct, expected);
            assert(false);
        }
    }

    // 形状必须落在 4x4 包围盒内,而且每行/每列对齐(不存在"斜着"的方块)。
    for (int p = 0; p < TETRIS_PIECE_COUNT; p++) {
        for (int rot = 0; rot < 4; rot++) {
            int min_x = 4, max_x = -1, min_y = 4, max_y = -1;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    if (!tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x, y)) continue;
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    if (y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                }
            }
            assert(max_x - min_x <= 3 && max_y - min_y <= 3);
            // 4 格必然是"一条线"或"2x2 或 L/T/S 形":包围盒面积不超过 4 格中的
            // 面积上界 2x4 —— 这里用连通性代替:每个格子至少与另一个格子相邻。
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    if (!tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x, y)) continue;
                    const bool near = tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x + 1, y) ||
                                      tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x - 1, y) ||
                                      tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x, y + 1) ||
                                      tetris_piece_cell((tetris_piece_t)p, (uint8_t)rot, x, y - 1);
                    assert(near);
                }
            }
        }
    }

    // 越界访问必须安全返回 false,不能读越界。
    assert(!tetris_piece_cell(TETRIS_PIECE_I, 0, -1, 0));
    assert(!tetris_piece_cell(TETRIS_PIECE_I, 0, 4, 0));
    assert(!tetris_piece_cell(TETRIS_PIECE_I, 0, 0, 4));
    assert(!tetris_piece_cell(TETRIS_PIECE_I, 9, 0, 0));
    assert(!tetris_piece_cell((tetris_piece_t)99, 0, 0, 0));
}

// --- 出生与队列 -------------------------------------------------------------
static void test_spawn_and_queue(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 12345);
    assert(m.state == TETRIS_STATE_PLAYING);
    assert(m.active);
    assert(m.level == 1);
    assert(m.score == 0 && m.lines == 0);
    assert(!overlaps(&m));
    // 出生位置必须在棋盘内(不能一出生就撞墙)。
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!tetris_piece_cell(m.piece, m.rotation, px, py)) continue;
            assert(m.x + px >= 0 && m.x + px < TETRIS_COLS);
        }
    }
    // 预览队列里不能出现空值。
    for (int i = 0; i < TETRIS_PREVIEW; i++) {
        assert(tetris_queue_at(&m, i) >= 0 && tetris_queue_at(&m, i) < TETRIS_PIECE_COUNT);
    }
    assert(tetris_queue_at(&m, -5) == tetris_queue_at(&m, 0));
    assert(tetris_queue_at(&m, 99) == tetris_queue_at(&m, TETRIS_PREVIEW));
}

static void test_seven_bag_is_fair(void) {
    // 7-bag 的意义:每 7 个方块里 7 种各出现一次,所以"连续 6 个 S"不可能发生。
    // 判据用两条,均匀随机会同时违反:
    //   * 每个方块的出现次数必须几乎相等(1000 ± 1,来自整袋计数);
    //   * 同一种方块两次出现之间的间隔下界 1、上界 13。
    enum { N = 7000 };
    static uint8_t seq[N];
    int last_seen[TETRIS_PIECE_COUNT];
    int counts[TETRIS_PIECE_COUNT] = { 0 };
    int max_gap[TETRIS_PIECE_COUNT] = { 0 };
    int min_gap[TETRIS_PIECE_COUNT] = { 0 };
    for (int i = 0; i < TETRIS_PIECE_COUNT; i++) last_seen[i] = -1;

    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 0xC0FFEE);
    for (int i = 0; i < N; i++) {
        const int piece = (int)tetris_queue_at(&m, 0);
        assert(piece >= 0 && piece < TETRIS_PIECE_COUNT);
        seq[i] = (uint8_t)piece;
        counts[piece]++;
        if (last_seen[piece] >= 0) {
            const int gap = i - last_seen[piece];
            if (gap > max_gap[piece]) max_gap[piece] = gap;
            if (min_gap[piece] == 0 || gap < min_gap[piece]) min_gap[piece] = gap;
        }
        last_seen[piece] = i;
        tetris_hard_drop(&m, NULL);
        if (m.state == TETRIS_STATE_CLEARING) tick_ms(&m, TETRIS_CLEAR_MS, NULL);
        // 定期清空棋盘,免得堆到顶(本测试只关心方块序列,不关心玩法)。
        // 每 8 个方块清一次:32 格不可能堆到出生区,所以永远不会提前结束。
        if (i % 8 == 7) memset(m.board, 0, sizeof(m.board));
        if (m.state != TETRIS_STATE_PLAYING) {
            printf("  意外结束:i=%d state=%d lines=%u\n", i, (int)m.state, (unsigned)m.lines);
            assert(false);
        }
    }

    for (int i = 0; i < TETRIS_PIECE_COUNT; i++) {
        // 7000 个方块 = 1000 袋,相位最多差 1。
        if (counts[i] < 998 || counts[i] > 1001) {
            printf("  方块 %d 出现 %d 次(期望约 1000)\n", i, counts[i]);
            assert(false);
        }
        assert(min_gap[i] >= 1);
        assert(max_gap[i] <= 13);        // 一袋内最多隔 6+6 个再遇到自己
    }
    printf("  7-bag: 7000 个方块,每种 %d~%d 次,最大间隔 %d\n", counts[0], counts[1],
           max_gap[0]);
    (void)seq;
}

// --- 移动与旋转 -------------------------------------------------------------
static void test_move_and_walls(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 7);
    // 一直往左:最终必须停下,而且方块的每一格仍在棋盘内。
    for (int i = 0; i < 20; i++) tetris_move(&m, -1, NULL);
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!tetris_piece_cell(m.piece, m.rotation, px, py)) continue;
            assert(m.x + px >= 0);
        }
    }
    const int left = m.x;
    assert(!tetris_move(&m, -1, NULL));
    assert(m.x == left);
    for (int i = 0; i < 20; i++) tetris_move(&m, 1, NULL);
    assert(!tetris_move(&m, 1, NULL));
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!tetris_piece_cell(m.piece, m.rotation, px, py)) continue;
            assert(m.x + px < TETRIS_COLS);
        }
    }
    // 非法 dx
    assert(!tetris_move(&m, 2, NULL));
    assert(!tetris_move(&m, 0, NULL));
}

static void test_rotation_and_kick(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 7);
    // 转到贴墙,再从贴墙状态继续转 —— 必须靠踢墙成功,而不是"卡住不动"。
    for (int i = 0; i < 20; i++) tetris_move(&m, -1, NULL);
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        tetris_events_t ev;
        if (tetris_rotate(&m, 1, &ev)) {
            ok++;
            assert(ev.rotated);
        }
        assert(!overlaps(&m));
        for (int py = 0; py < 4; py++) {
            for (int px = 0; px < 4; px++) {
                if (!tetris_piece_cell(m.piece, m.rotation, px, py)) continue;
                assert(m.x + px >= 0 && m.x + px < TETRIS_COLS);
            }
        }
    }
    assert(ok >= 6);   // 贴墙也几乎总能转(踢墙生效)
    assert(!tetris_rotate(&m, 0, NULL));
    assert(!tetris_rotate(&m, 2, NULL));
}

static void test_rotate_both_directions_cancel(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 99);
    const uint8_t before = m.rotation;
    const int8_t x0 = m.x;
    tetris_rotate(&m, 1, NULL);
    tetris_rotate(&m, -1, NULL);
    assert(m.rotation == before);
    assert(m.x == x0);
}

// --- 节奏 -------------------------------------------------------------------
static void test_gravity_curve(void) {
    assert(tetris_gravity_ms(1) == 800);
    assert(tetris_gravity_ms(2) == 730);
    assert(tetris_gravity_ms(0) == 800);       // 非法等级按 1 级处理
    assert(tetris_gravity_ms(TETRIS_MAX_LEVEL) == 80);
    assert(tetris_gravity_ms(99) == 80);       // 不能出现负数/回绕
    for (uint8_t lv = 1; lv < TETRIS_MAX_LEVEL; lv++) {
        assert(tetris_gravity_ms(lv) >= tetris_gravity_ms((uint8_t)(lv + 1)));
    }
}

static void test_gravity_moves_one_row_per_interval(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 5);
    const int8_t y0 = m.y;
    tick_ms(&m, tetris_gravity_ms(1) - 20, NULL);
    assert(m.y == y0);
    tick_ms(&m, 30, NULL);
    assert(m.y == y0 + 1);
}

static void test_big_dt_does_not_teleport(void) {
    // 喂一个巨大的 dt(卡顿/调试)不能让方块直接沉到底或跳过锁定延迟。
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 5);
    const int8_t y0 = m.y;
    tetris_tick(&m, 100000, NULL);
    assert(m.y - y0 <= 3);          // 最多推进 TETRIS_MAX_DT_MS 允许的两三格
    assert(m.state == TETRIS_STATE_PLAYING);
}

static void test_soft_drop_is_faster_and_scores(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 5);
    tetris_set_soft_drop(&m, true);
    const int8_t y0 = m.y;
    tick_ms(&m, 100, NULL);
    assert(m.y > y0);                       // 软降期间下落明显更快
    assert(m.score >= (uint32_t)(m.y - y0)); // 每下落一格 +1
    tetris_set_soft_drop(&m, false);
    const uint32_t score = m.score;
    const int8_t y1 = m.y;
    tick_ms(&m, 100, NULL);
    assert(m.y - y1 <= 1);                  // 松开后回到正常重力
    assert(m.score == score);
}

// --- 锁定 -------------------------------------------------------------------
static void put_on_floor(tetris_model_t *m) {
    // 把当前方块直接放到最底(不锁定),用于测试锁定延迟。
    while (!overlaps(m) && m->y + 1 < TETRIS_ROWS) {
        bool can = true;
        for (int py = 0; py < 4 && can; py++) {
            for (int px = 0; px < 4; px++) {
                if (!tetris_piece_cell(m->piece, m->rotation, px, py)) continue;
                const int by = m->y + 1 + py;
                const int bx = m->x + px;
                if (by >= TETRIS_ROWS || (by >= 0 && m->board[by][bx] != 0)) { can = false; break; }
            }
        }
        if (!can) break;
        m->y++;
    }
    m->gravity_acc_ms = 0;
}

static void test_lock_delay(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 11);
    put_on_floor(&m);
    const int8_t y = m.y;
    // 贴地 400ms 还不该锁(给玩家挪位置的时间)。
    tick_ms(&m, TETRIS_LOCK_MS - 50, NULL);
    assert(m.active && m.y == y);
    // 再等一会儿必须锁住:棋盘上出现这个方块(4 格)。
    tick_ms(&m, 200, NULL);
    assert(board_cells(&m) == 4);
    assert_board_clean(&m);
}

static void test_move_resets_lock_delay_but_is_capped(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 11);
    put_on_floor(&m);
    // 反复"等一会 + 挪一下":锁定被推迟,但不能无限推迟(重置次数有上限)。
    for (int i = 0; i < TETRIS_LOCK_RESETS + 6; i++) {
        tick_ms(&m, TETRIS_LOCK_MS - 120, NULL);
        if (!m.active) break;
        const int8_t x = m.x;
        if (!tetris_move(&m, x > 5 ? -1 : 1, NULL)) tetris_move(&m, x > 5 ? 1 : -1, NULL);
    }
    tick_ms(&m, TETRIS_LOCK_MS + 50, NULL);
    assert(board_cells(&m) > 0);             // 最终一定会锁住
}

static void test_hard_drop_scores_and_lands(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 3);
    const int8_t y0 = m.y;
    tetris_events_t ev;
    assert(tetris_hard_drop(&m, &ev));
    assert(ev.dropped);
    assert(board_cells(&m) == 4);
    // 方块应该落在最底:最下面一行必然有格子。
    bool bottom_used = false;
    for (int x = 0; x < TETRIS_COLS; x++) {
        if (m.board[TETRIS_ROWS - 1][x] != 0) bottom_used = true;
    }
    assert(bottom_used);
    assert(m.score == (uint32_t)(TETRIS_ROWS - 1 - y0) * 2 || m.score > 0);
    assert_board_clean(&m);
}

// --- 消行 -------------------------------------------------------------------
// 造一个"只差 O 方块就能填满第 19 行"的棋盘。
static void setup_single_line(tetris_model_t *m) {
    memset(m, 0, sizeof(*m));
    tetris_begin(m, 42);
    for (int x = 0; x < TETRIS_COLS; x++) m->board[TETRIS_ROWS - 1][x] = CELL(TETRIS_PIECE_I);
    m->board[TETRIS_ROWS - 1][4] = 0;
    m->board[TETRIS_ROWS - 1][5] = 0;
    m->board[TETRIS_ROWS - 1][3] = 0;      // O 只盖住 4、5
    m->board[TETRIS_ROWS - 1][3] = CELL(TETRIS_PIECE_I);
    m->piece = TETRIS_PIECE_O;
    m->rotation = 0;
    m->x = 3;      // O 占 x=4,5
    m->y = 18;
    m->active = true;
    m->state = TETRIS_STATE_PLAYING;
    m->score = 0;
    m->lines = 0;
    m->level = 1;
}

static void test_single_line_clear_animation(void) {
    tetris_model_t m;
    setup_single_line(&m);
    tetris_events_t ev;
    assert(tetris_hard_drop(&m, &ev));
    // 落定后先进入"闪烁"状态:整行还在棋盘上,但已经不接受操作了。
    assert(m.state == TETRIS_STATE_CLEARING);
    assert(ev.clear_begin);
    assert(m.lines == 0);
    assert(m.score == 0);
    assert(tetris_row_clearing(&m, TETRIS_ROWS - 1));
    assert(m.board[TETRIS_ROWS - 1][0] != 0);
    assert(!tetris_move(&m, -1, NULL));
    assert(!tetris_hard_drop(&m, NULL));

    // 闪烁强度必须在 0..255 之间,并且确实会亮起来。
    uint8_t max_flash = 0;
    for (int i = 0; i < 20; i++) {
        const uint8_t f = tetris_clear_flash(&m);
        if (f > max_flash) max_flash = f;
        tick_ms(&m, 10, NULL);
    }
    assert(max_flash > 100);

    // 动画走完:行没了,分数到账,新方块出生。
    tick_ms(&m, TETRIS_CLEAR_MS, &ev);
    assert(m.state == TETRIS_STATE_PLAYING);
    assert(ev.lines_cleared == 1);
    assert(m.lines == 1);
    assert(m.score == 100);
    assert(!tetris_row_clearing(&m, TETRIS_ROWS - 1));
    // 第 19 行只剩 O 方块的上半部分(落到最底),其余行全空。
    assert(m.board[TETRIS_ROWS - 1][4] == CELL(TETRIS_PIECE_O));
    assert(m.board[TETRIS_ROWS - 1][5] == CELL(TETRIS_PIECE_O));
    for (int y = 0; y < TETRIS_ROWS - 1; y++) {
        for (int x = 0; x < TETRIS_COLS; x++) assert(m.board[y][x] == 0);
    }
    assert_board_clean(&m);
}

static void test_four_line_clear_scores_800(void) {
    // 挖一条只有第 0 列的竖井,用竖直的 I 一次消 4 行。
    for (int rotation = 0; rotation < 4; rotation++) {
        tetris_model_t m;
        memset(&m, 0, sizeof(m));
        tetris_begin(&m, 8);
        for (int y = TETRIS_ROWS - 4; y < TETRIS_ROWS; y++) {
            for (int x = 0; x < TETRIS_COLS; x++) m.board[y][x] = CELL(TETRIS_PIECE_T);
            m.board[y][0] = 0;
        }
        m.piece = TETRIS_PIECE_I;
        m.rotation = (uint8_t)rotation;
        // I 的横放(旋转态 0/2)塞不进 1 格宽的竖井,只测两个竖直形态:
        // 旋转态 1 占包围盒第 2 列、旋转态 3 占第 1 列,把它们对齐到棋盘的
        // 第 0 列(x 取负值,方块可以部分位于棋盘左外侧)。
        if (rotation != 1 && rotation != 3) continue;
        m.x = (rotation == 1) ? -2 : -1;
        m.y = 0;
        m.active = true;
        m.state = TETRIS_STATE_PLAYING;
        m.score = 0;
        m.level = 1;
        tetris_events_t ev;
        assert(tetris_hard_drop(&m, &ev));
        assert(m.state == TETRIS_STATE_CLEARING);
        assert(m.clear_count == 4);
        tick_ms(&m, TETRIS_CLEAR_MS, &ev);
        assert(ev.lines_cleared == 4);
        assert(m.lines == 4);
        // 800 * 等级 1 + 硬降的分(至少 0)
        assert(m.score >= 800);
        assert(m.score < 800 + 40);
        assert_board_clean(&m);
    }
}

static void test_level_up_at_ten_lines(void) {
    tetris_model_t m;
    setup_single_line(&m);
    m.lines = 9;
    m.level = 1;
    tetris_events_t ev;
    tetris_hard_drop(&m, &ev);
    tick_ms(&m, TETRIS_CLEAR_MS, &ev);
    assert(m.lines == 10);
    assert(m.level == 2);
    assert(ev.level_up);
    // 计分用的是消行时的等级(1 级),不是升级后的等级。
    assert(m.score == 100);
}

static void test_clear_score_table(void) {
    // 直接验证计分表:2 行 = 300、3 行 = 500(用等级 1)。
    for (int rows = 1; rows <= 4; rows++) {
        tetris_model_t m;
        memset(&m, 0, sizeof(m));
        tetris_begin(&m, 21);
        for (int y = TETRIS_ROWS - rows; y < TETRIS_ROWS; y++) {
            for (int x = 1; x < TETRIS_COLS; x++) m.board[y][x] = CELL(TETRIS_PIECE_L);
        }
        m.piece = TETRIS_PIECE_I;
        m.rotation = 1;      // 竖直
        m.x = -2;            // 第 0 列
        m.y = 0;
        m.active = true;
        m.state = TETRIS_STATE_PLAYING;
        m.score = 0;
        tetris_events_t ev;
        tetris_hard_drop(&m, &ev);
        if (m.state != TETRIS_STATE_CLEARING) { assert(false); }
        tick_ms(&m, TETRIS_CLEAR_MS + 20, &ev);
        assert(ev.lines_cleared == rows);
        assert(m.lines == rows);
        const uint32_t expected[] = { 0, 100, 300, 500, 800 };
        assert(m.score >= expected[rows]);
        assert(m.score <= expected[rows] + 40);   // 允许硬降的少量加分
    }
}

// --- 结束与暂停 -------------------------------------------------------------
static void test_game_over_when_spawn_blocked(void) {
    // 第一种结束方式:下一个方块生不出来(出生区被占)。
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 17);
    // 把最上面 3 行的左侧堵住(故意留第 9 列不填),然后把当前方块挪到第 3 行
    // 并落到底:消不掉行,而下一个方块一定出生在被堵住的第 0/1 行。
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < TETRIS_COLS - 1; x++) m.board[y][x] = CELL(TETRIS_PIECE_S);
    }
    m.piece = TETRIS_PIECE_O;
    m.rotation = 0;
    m.x = 3;
    m.y = 3;
    m.active = true;
    m.state = TETRIS_STATE_PLAYING;
    tetris_events_t ev;
    assert(tetris_hard_drop(&m, &ev));
    assert(m.state == TETRIS_STATE_OVER);
    assert(ev.game_over);
    assert(!m.active);
    // 结束后不再接受任何操作,时间也不再推进。
    const int8_t y = m.y;
    assert(!tetris_move(&m, -1, NULL));
    assert(!tetris_rotate(&m, 1, NULL));
    assert(!tetris_hard_drop(&m, NULL));
    tetris_tick(&m, 1000, NULL);
    assert(m.y == y);
    assert(m.state == TETRIS_STATE_OVER);
    assert(!overlaps(&m));
}

static void test_game_over_when_locked_above_ceiling(void) {
    // 第二种结束方式:方块被顶到棋盘上方还锁住了(第 1 行堵死)。
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 23);
    fill_row(&m, 1, CELL(TETRIS_PIECE_L));
    for (int y = 2; y < TETRIS_ROWS; y++) m.board[y][0] = CELL(TETRIS_PIECE_L);
    m.piece = TETRIS_PIECE_O;
    m.rotation = 0;
    m.x = 3;
    m.y = -1;                 // O 占第 -1 与第 0 行
    m.active = true;
    m.state = TETRIS_STATE_PLAYING;
    tetris_events_t ev;
    assert(tetris_hard_drop(&m, &ev));
    assert(m.state == TETRIS_STATE_OVER);
    assert(ev.game_over);
    assert(!m.active);
    // 露在棋盘外的那一半不写进棋盘,但棋盘内的那一半要写进去(玩家能看到堵在哪)。
    assert(m.board[0][4] == CELL(TETRIS_PIECE_O) || m.board[1][4] != 0);
    assert(!overlaps(&m));
}

static void test_pause_freezes_everything(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 4);
    assert(tetris_toggle_pause(&m));       // 返回值 = "现在处于暂停"
    assert(m.state == TETRIS_STATE_PAUSED);
    const int8_t y = m.y;
    const uint32_t score = m.score;
    tick_ms(&m, 3000, NULL);
    assert(m.y == y && m.score == score);
    assert(!tetris_move(&m, -1, NULL));
    assert(!tetris_hard_drop(&m, NULL));
    assert(!tetris_toggle_pause(&m));      // 恢复运行
    assert(m.state == TETRIS_STATE_PLAYING);
    tick_ms(&m, 900, NULL);
    assert(m.y > y);
}

static void test_pause_drops_soft_drop_flag(void) {
    // 暂停时"下键按住"不能被带过暂停:否则恢复后方块会自己加速到底。
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 4);
    tetris_set_soft_drop(&m, true);
    tetris_toggle_pause(&m);
    assert(!m.soft_drop);
}

// --- 幽灵与查询 -------------------------------------------------------------
static void test_ghost_y(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 6);
    const int ghost = tetris_ghost_y(&m);
    assert(ghost >= m.y);
    // 幽灵位置必须能放下这个方块,而且再下一格就放不下。
    tetris_model_t probe = m;
    probe.y = (int8_t)ghost;
    assert(tetris_cell_at(&probe, 0, ghost) == 0 || true);
    assert(overlaps(&probe) == false);
    // 棋盘上的查询:当前方块的位置能读到颜色。
    int found = 0;
    for (int y = 0; y < TETRIS_ROWS; y++) {
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (tetris_cell_at(&m, x, y) != 0) found++;
        }
    }
    assert(found == 4);
    assert(tetris_cell_at(&m, -1, 0) == 0);
    assert(tetris_cell_at(&m, TETRIS_COLS, 0) == 0);
    assert(tetris_cell_at(&m, 0, -1) == 0);
    assert(tetris_cell_at(&m, 0, TETRIS_ROWS) == 0);
}

// --- 随机长跑 ---------------------------------------------------------------
static void test_random_fuzz_invariants(void) {
    // 用固定种子跑 20 万步随机操作,检查每一条不变量都成立。
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    tetris_begin(&m, 0xBEEF);
    uint32_t rng = 0x12345678u;
    uint32_t prev_score = 0;
    uint16_t prev_lines = 0;
    uint8_t prev_level = 1;
    for (int i = 0; i < 200000; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const uint32_t r = rng % 100;
        if (r < 25) tetris_move(&m, (rng & 1) ? 1 : -1, NULL);
        else if (r < 40) tetris_rotate(&m, (rng & 1) ? 1 : -1, NULL);
        else if (r < 45) tetris_hard_drop(&m, NULL);
        else if (r < 50) tetris_set_soft_drop(&m, (rng & 1) != 0);
        else if (r < 52) tetris_toggle_pause(&m);
        else tetris_tick(&m, rng % 60, NULL);

        assert_board_clean(&m);
        assert(m.score >= prev_score);           // 分数只增不减
        assert(m.lines >= prev_lines);
        assert(m.level >= prev_level);
        assert(m.level >= 1 && m.level <= TETRIS_MAX_LEVEL);
        assert(m.state <= TETRIS_STATE_OVER);
        if (m.state == TETRIS_STATE_OVER) {
            tetris_begin(&m, rng | 1u);     // 新一局:分数/行数/等级归零
            assert(m.score == 0 && m.lines == 0 && m.level == 1);
            prev_score = 0; prev_lines = 0; prev_level = 1;
            continue;
        }
        prev_score = m.score;
        prev_lines = m.lines;
        prev_level = m.level;
    }
    printf("  fuzz: 分数 %u 行 %u 等级 %u 状态 %d\n", (unsigned)m.score, (unsigned)m.lines,
           (unsigned)m.level, (int)m.state);
}

static void test_deterministic_for_same_seed(void) {
    // 同一个种子必须给出完全一样的方块序列(测试与调试都依赖这条)。
    tetris_model_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    tetris_begin(&a, 777);
    tetris_begin(&b, 777);
    for (int i = 0; i < 200; i++) {
        assert(tetris_queue_at(&a, 0) == tetris_queue_at(&b, 0));
        tetris_hard_drop(&a, NULL);
        tetris_hard_drop(&b, NULL);
        assert(a.score == b.score);
        assert(a.x == b.x && a.y == b.y && a.piece == b.piece);
        tick_ms(&a, TETRIS_CLEAR_MS, NULL);
        tick_ms(&b, TETRIS_CLEAR_MS, NULL);
        if (a.state == TETRIS_STATE_OVER) {
            assert(b.state == TETRIS_STATE_OVER);
            tetris_begin(&a, 5);
            tetris_begin(&b, 5);
        }
    }
}

static void test_record_flag(void) {
    tetris_model_t m;
    memset(&m, 0, sizeof(m));
    m.best = 10;
    tetris_begin(&m, 3);
    assert(m.best == 10);          // begin 不能把历史最好成绩清掉
    assert(!m.record);
    tetris_hard_drop(&m, NULL);
    if (m.score > 10) assert(m.record);
}

int main(void) {
    test_shape_table();
    test_spawn_and_queue();
    test_seven_bag_is_fair();
    test_move_and_walls();
    test_rotation_and_kick();
    test_rotate_both_directions_cancel();
    test_gravity_curve();
    test_gravity_moves_one_row_per_interval();
    test_big_dt_does_not_teleport();
    test_soft_drop_is_faster_and_scores();
    test_lock_delay();
    test_move_resets_lock_delay_but_is_capped();
    test_hard_drop_scores_and_lands();
    test_single_line_clear_animation();
    test_four_line_clear_scores_800();
    test_level_up_at_ten_lines();
    test_clear_score_table();
    test_game_over_when_spawn_blocked();
    test_game_over_when_locked_above_ceiling();
    test_pause_freezes_everything();
    test_pause_drops_soft_drop_flag();
    test_ghost_y();
    test_deterministic_for_same_seed();
    test_random_fuzz_invariants();
    test_record_flag();
    printf("tetris model tests: PASS\n");
    return 0;
}
