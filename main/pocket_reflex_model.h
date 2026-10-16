// main/pocket_reflex_model.h —— 《方向快手》的游戏规则(纯逻辑,零 ESP-IDF 依赖)。
//
// 和 chick_run_model.c 一个套路:规则、计时、计分与随机出题都在这里,主机上
// 直接编译就能测(tests/test_pocket_reflex_model.c);画面与按键接线在
// pocket_reflex.c。两者之间只传 now_ms,不碰任何硬件。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 玩家能按的键。A/B 不参与答题(答题只认四个方向),它们只负责开始/重开,
// 这样上层 UI 不需要自己判"现在是不是标题页"。
typedef enum {
    POCKET_KEY_UP = 0,
    POCKET_KEY_DOWN,
    POCKET_KEY_LEFT,
    POCKET_KEY_RIGHT,
    POCKET_KEY_A,
    POCKET_KEY_B,
} pocket_key_t;

typedef enum {
    POCKET_PHASE_TITLE = 0,  // 标题页,等 A
    POCKET_PHASE_WAIT,       // 箭头已出,等按键
    POCKET_PHASE_JUDGE,      // 判定结果闪一下(HIT/WRONG/SLOW)
    POCKET_PHASE_OVER,       // 结算页,等 A 重开
} pocket_phase_t;

typedef enum {
    POCKET_JUDGE_NONE = 0,
    POCKET_JUDGE_HIT,    // 按对
    POCKET_JUDGE_WRONG,  // 按错方向
    POCKET_JUDGE_SLOW,   // 超时
} pocket_judge_t;

// --- 手感参数(想调难度改这里)-----------------------------------------------
// 第一题的限时。1400ms 是"看一眼就要动"的量级,不会一上来就劝退。
#define POCKET_WINDOW_START_MS 1400
// 每答对一题缩短的限时,到 MIN 为止。70ms/题 → 约 15 题后摸到下限。
#define POCKET_WINDOW_STEP_MS  70
// 限时下限。380ms 已经是人类反应极限附近,再短就只剩运气了。
#define POCKET_WINDOW_MIN_MS   380
// 判定结果在屏上停留的时间(按对/按错/超时的画面反馈窗口)。
#define POCKET_JUDGE_FLASH_MS  420
// 命数。掉光就结算。
#define POCKET_LIVES           3

typedef struct {
    pocket_phase_t phase;
    pocket_judge_t judge;   // 最近一次判定,UI 按它上色
    pocket_key_t arrow;     // 当前该按的方向
    pocket_key_t last_key;  // 最近一次按键(按错时告诉玩家他按了哪边)
    uint16_t score;         // 本局答对数
    uint16_t best;          // 历史最好(只活在内存里)
    uint16_t last_rt_ms;    // 上一题的反应时间
    uint8_t lives;
    uint32_t window_ms;     // 本题限时
    uint32_t prompt_ms;     // 本题出题时刻
    uint32_t deadline_ms;   // prompt_ms + window_ms
    uint32_t flash_until_ms;
    uint32_t rng;           // 自带的 LCG 状态,种子来自开机时间
} pocket_reflex_t;

// 建立标题页状态。seed 传开机毫秒数即可(同一秒内开机才会出同一串题)。
void pocket_reflex_init(pocket_reflex_t *g, uint32_t seed);

// 开新的一局(A 键)。
void pocket_reflex_start(pocket_reflex_t *g, uint32_t now_ms);

// 按键。返回 true = 这一下被游戏吃掉了,画面需要重画。
bool pocket_reflex_press(pocket_reflex_t *g, pocket_key_t key, uint32_t now_ms);

// 每帧调用一次:处理超时与判定闪烁结束。返回 true = 状态变了。
bool pocket_reflex_tick(pocket_reflex_t *g, uint32_t now_ms);

// 本题剩余时间,0..100(%)。只有 WAIT 阶段有意义,其余阶段返回 0。
int pocket_reflex_time_left_pct(const pocket_reflex_t *g, uint32_t now_ms);
