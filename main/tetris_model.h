// main/tetris_model.h —— 俄罗斯方块的游戏规则(纯逻辑,不依赖 ESP-IDF/LVGL/绘制)。
//
// 为什么单独成层:下落节奏、消行、旋转踢墙、锁定延迟这些规则全部是"看不见的算术",
// 在设备上只表现为"手感不对/方块卡住/分数不对",靠眼睛调试成本极高。放在这里就能在
// 主机上按毫秒推进时间、构造任意棋盘来验证(见 tests/test_tetris_model.c)。
//
// 时间基准:调用方把每帧经过的毫秒喂给 tetris_tick(),模型内部自己累计重力与锁定
// 延迟,所以规则不依赖帧率(掉帧不会让方块变慢)。
//
// 与 S31 版三键 demo(origin/demo/tetris-game)的差别:那份没有锁定延迟、没有消行动画、
// 没有预览队列、旋转只有左右平移踢墙;这里是六键板的产品版,补齐了这些体验细节。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TETRIS_COLS       10
#define TETRIS_ROWS       20
// 侧栏预览的方块个数(队列长度 = 预览数 + 当前正在下落的那个)。
#define TETRIS_PREVIEW    3

typedef enum {
    TETRIS_PIECE_I = 0,
    TETRIS_PIECE_O,
    TETRIS_PIECE_T,
    TETRIS_PIECE_S,
    TETRIS_PIECE_Z,
    TETRIS_PIECE_J,
    TETRIS_PIECE_L,
    TETRIS_PIECE_COUNT,
} tetris_piece_t;

typedef enum {
    TETRIS_STATE_PLAYING = 0,
    TETRIS_STATE_CLEARING,   // 消行动画中:方块已落定,整行还在闪,不接受操作
    TETRIS_STATE_PAUSED,
    TETRIS_STATE_OVER,
} tetris_state_t;

// 一次操作/一帧推进产生的事件。调用方用它决定播哪个音效;不需要时传 NULL。
typedef struct {
    bool moved;             // 成功左右移动
    bool rotated;           // 成功旋转
    bool dropped;           // 硬降成功(落到底并锁定)
    bool locked;            // 本次落子(硬降/重力/锁定延迟都算)
    bool clear_begin;       // 本次开始消行动画(UI 可以播"消行"音效)
    uint8_t lines_cleared;  // 本次真正消掉的行数(动画结束后才非 0)
    bool level_up;
    bool game_over;
} tetris_events_t;

typedef struct {
    // 棋盘:0 = 空,非 0 = 该格已固定的方块颜色号(方块类型 + 1)。
    uint8_t board[TETRIS_ROWS][TETRIS_COLS];

    tetris_piece_t piece;      // 当前方块
    uint8_t rotation;          // 0..3
    int8_t x, y;               // 方块 4x4 包围盒在棋盘上的左上角
    bool active;               // 是否有方块在下落(消行动画/结束时为 false)

    // 队列:queue[0..TETRIS_PREVIEW-1] 是预览,queue[TETRIS_PREVIEW] 是后备。
    tetris_piece_t queue[TETRIS_PREVIEW + 1];

    // 7-bag 随机器:保证每 7 个方块里 7 种各出现一次,不会连续来 5 个 S。
    tetris_piece_t bag[TETRIS_PIECE_COUNT];
    uint8_t bag_n;
    uint32_t rng;

    tetris_state_t state;
    uint32_t score;
    uint16_t lines;
    uint8_t level;
    uint32_t best;             // 本局开始前的最好成绩(存储层填;模型只做比较)

    // 节奏状态
    uint32_t gravity_acc_ms;   // 重力累计时间
    uint16_t lock_ms;          // 贴地后已等待的时间(锁定延迟)
    uint8_t lock_resets;       // 贴地后允许的"移动重置"次数
    bool soft_drop;            // 按住下键:重力加速,每下落一格 +1 分

    // 消行动画
    uint32_t clear_rows;       // 位图:bit y 表示第 y 行正在闪
    uint8_t clear_count;       // 本次消掉的行数(用于计分表)
    uint16_t clear_ms;         // 动画已进行时间

    int8_t ghost_y;            // 当前方块的落点行(模型维护,UI 直接画)
    bool record;               // 本局分数已超过 best(历史为 0 时第一次得分即为新纪录)
} tetris_model_t;

// --- 生命周期 ---------------------------------------------------------------
// 开新局。seed = 0 时用固定种子(测试需要可复现);正式游戏传真随机数。
void tetris_begin(tetris_model_t *m, uint32_t seed);

// 填入历史最高分(存储层读出后调用)。它只影响"本局是否新纪录"的判断,不参与玩法;
// begin 会保留这个值,所以顺序是 begin -> set_best。
void tetris_set_best(tetris_model_t *m, uint32_t best);

// 推进 dt_ms 毫秒(重力、锁定延迟、消行动画)。dt 内部夹取,掉帧不会瞬移多格。
// 注意:落子成功后模型会【立刻出生下一个方块】,所以 active 又变回 true —— 想判断
// "刚才锁住了没有"请用 ev->locked / 棋盘格子数,别用 active。
void tetris_tick(tetris_model_t *m, uint32_t dt_ms, tetris_events_t *ev);

// --- 操作 -------------------------------------------------------------------
bool tetris_move(tetris_model_t *m, int dx, tetris_events_t *ev);
// dir = +1 顺时针 / -1 逆时针。
bool tetris_rotate(tetris_model_t *m, int dir, tetris_events_t *ev);
bool tetris_hard_drop(tetris_model_t *m, tetris_events_t *ev);
void tetris_set_soft_drop(tetris_model_t *m, bool on);
// 只有 PLAYING/PAUSED 之间可以切换;返回 true 表示现在是暂停状态。
bool tetris_toggle_pause(tetris_model_t *m);

// --- 查询(绘制用,全部是 const,不改状态)----------------------------------
bool tetris_piece_cell(tetris_piece_t piece, uint8_t rotation, int x, int y);
// 网格里这一格的颜色号(0 = 空)。包含正在下落的方块;软降/幽灵请用下面的接口。
uint8_t tetris_cell_at(const tetris_model_t *m, int x, int y);
int tetris_ghost_y(const tetris_model_t *m);
tetris_piece_t tetris_queue_at(const tetris_model_t *m, int index);
bool tetris_row_clearing(const tetris_model_t *m, int row);
// 消行闪烁强度 0..255(三角波脉冲,UI 叠一层白)。
uint8_t tetris_clear_flash(const tetris_model_t *m);

// 某一等级的重力间隔(毫秒/格)。
uint32_t tetris_gravity_ms(uint8_t level);

// 常量:锁定延迟、消行动画时长、软降间隔、最大锁定重置次数。
#define TETRIS_LOCK_MS        450
#define TETRIS_LOCK_RESETS    10
#define TETRIS_CLEAR_MS       260
#define TETRIS_SOFT_DROP_MS   45
#define TETRIS_MAX_LEVEL      15
// 单次 tick 最多推进的毫秒数:一次卡顿(或调试时喂一个大 dt)不能让方块瞬移到底,
// 也不会让锁定延迟一步走完。
#define TETRIS_MAX_DT_MS      100
