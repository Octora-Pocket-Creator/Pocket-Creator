// main/chick_run_model.h —— 小鸡快跑(Chick Run)的纯逻辑状态机。
//
// 本文件不依赖 LVGL / ESP-IDF,可在主机上直接编译测试:物理、生成节奏、碰撞、
// 计分、难度曲线都在这里;渲染与按键映射属于 UI 层(chick_run.c)。
//
// 坐标系:屏幕左上为原点,x 向右、y 向下(和 LVGL 一致);
// 但"离地高度"用 chick_y_milli 表示,单位是 1/1000 像素,0 表示站在地面线上。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// --- 场地几何(逻辑像素,竖屏 240x320)---------------------------------------
#define CHICK_RUN_SCREEN_W       240
#define CHICK_RUN_SCREEN_H       320
#define CHICK_RUN_GROUND_Y       248    // 小鸡脚底所在的地面线
#define CHICK_RUN_CHICK_X        34     // 小鸡碰撞盒左边缘(水平固定)
// 美术尺寸(与 tools/author-chick-run-sprites.py 的生成结果一致,主机测试会核对)。
// 精灵按 1:1 画到屏幕,不再做整数放大 —— 放大过一轮的结果是"糊块",细节全丢。
#define CHICK_RUN_CHICK_ART_W        40
#define CHICK_RUN_CHICK_ART_H        44
#define CHICK_RUN_CHICK_ART_DUCK_H   24
// 碰撞盒比美术小一圈:左右各让 8px、头顶让 4px(啄和尾巴都在判定之外)。
// 横向宽容尤其重要 —— 玩家觉得"擦着过去了"是好事,"明明没碰到却死了"是灾难。
// 脚底与美术底行对齐,踩在地上。
#define CHICK_RUN_CHICK_ART_MARGIN_X 8
#define CHICK_RUN_CHICK_ART_MARGIN_Y 4
#define CHICK_RUN_CHICK_W        (CHICK_RUN_CHICK_ART_W - 2 * CHICK_RUN_CHICK_ART_MARGIN_X)
#define CHICK_RUN_CHICK_H        (CHICK_RUN_CHICK_ART_H - CHICK_RUN_CHICK_ART_MARGIN_Y)
#define CHICK_RUN_CHICK_DUCK_H   (CHICK_RUN_CHICK_ART_DUCK_H - CHICK_RUN_CHICK_ART_MARGIN_Y)
#define CHICK_RUN_MAX_OBSTACLES  6

// --- 手感参数(改这里就能调整难度与跳跃高度)--------------------------------
// 重力与初速一起决定"跳多高"和"滞空多久"。这里刻意用较低的重力:
// 同样的顶点下,重力越小,脚底高于障碍顶部的时间窗越长,低速时也从容 ——
// 也就是玩家感觉的"跳起来能往前飞一段"。顶点 ~110px,滞空 ~0.82s。
#define CHICK_RUN_GRAVITY         1300  // 重力 px/s^2
// 跳跃:短按顶点约 110px(最高障碍仙人掌 40px 的 2.7 倍)。高度不是拍脑袋定的:
// 障碍越宽,小鸡"脚底高于障碍顶部"的时间窗越短,而通过障碍所需时间随速度变化,
// 低速时最吃亏 —— 测试用"时间窗 > 通过时间 + 100ms"把这条不变量钉住。
// 长按再高约 20px:给的是容错,不是必需操作。
#define CHICK_RUN_JUMP_V          535   // 起跳初速度 px/s
#define CHICK_RUN_HOLD_GRAVITY    700   // 按住跳跃键时的上升重力(长按跳更高)
#define CHICK_RUN_HOLD_MS         120   // 长按助力的最长作用时间
#define CHICK_RUN_FALL_MULT       3     // 空中按"下"的快速下落倍率
#define CHICK_RUN_SLIDE_MS        650   // 一次滑铲(蹲下)持续时间
#define CHICK_RUN_SPEED_START     90    // 起始滚动速度 px/s(约 2.7 秒横穿屏幕)
#define CHICK_RUN_SPEED_MAX       160   // 速度上限 px/s
#define CHICK_RUN_SPEED_GAIN      2     // 每存活 1 秒增加的速度 px/s
#define CHICK_RUN_PX_PER_M        12    // 每 12 世界像素记作 1 米
#define CHICK_RUN_GAP_MIN_MS      1150  // 相邻障碍的最小间隔(按时间,保证任何速度下都跳得过去)
#define CHICK_RUN_GAP_MAX_MS      2000  // 相邻障碍的最大间隔
#define CHICK_RUN_MILESTONE_M     100   // 每 100 米触发一次里程碑事件
// --- 关卡模式 -----------------------------------------------------------------
// 每关 1000 米:到了就过关,下一关起点更高、障碍更密(但始终留出可跳窗口)。
// 3 条命:整局共用 —— 撞一次掉一条命并进入短无敌,掉光就结束,过关【不】补命。
// 每关 500 米:实测跑满约 45 秒 —— 够长到有"一关"的分量,又不至于让孩子等太久。
// 进度条、过关判定、主机测试都跟着这个常量走,改这一处即可。
#define CHICK_RUN_LEVEL_TARGET_M      500    // 每关目标距离(米)
#define CHICK_RUN_LEVEL_LIVES         3      // 关卡模式初始命数
#define CHICK_RUN_HURT_INVINCIBLE_MS  1500   // 掉命后的无敌时间(避免一撞到底)
#define CHICK_RUN_LEVEL_SPEED_STEP    18     // 每升一关,基础速度 +18 px/s
#define CHICK_RUN_LEVEL_GAP_MIN_STEP  50     // 每升一关,最小生成间隔 -50ms
#define CHICK_RUN_LEVEL_GAP_MAX_STEP  120    // 每升一关,最大生成间隔 -120ms
// 间隔下限必须大于一次跳跃的滞空时间(约 820ms),否则会出现"落地即撞"的死局。
#define CHICK_RUN_LEVEL_GAP_MIN_FLOOR 1000
#define CHICK_RUN_LEVEL_GAP_MAX_FLOOR 1400
#define CHICK_RUN_STEP_MS         16    // 物理子步长上限:掉帧也不会让障碍穿过小鸡

// 玩法模式。
typedef enum {
    CHICK_RUN_MODE_FREE = 0,   // 自由模式:撞一下结束,比距离(历史行为)
    CHICK_RUN_MODE_LEVEL,      // 关卡模式:每关 1000 米,3 条命
    CHICK_RUN_MODE_COUNT,
} chick_run_mode_t;

typedef enum {
    CHICK_STATE_READY = 0,   // 首页(或刚回到首页)
    CHICK_STATE_RUNNING,
    CHICK_STATE_PAUSED,
    CHICK_STATE_GAME_OVER,
} chick_state_t;

// 目前全部是地面障碍,统一靠跳跃通过(参考版本里也没有"必须下蹲"的障碍)。
typedef enum {
    CHICK_OBSTACLE_FENCE = 0,  // 木栅栏
    CHICK_OBSTACLE_CACTUS,     // 仙人掌(最高)
    CHICK_OBSTACLE_FOX,        // 小狐狸(最宽)
    CHICK_OBSTACLE_KIND_COUNT,
} chick_obstacle_kind_t;

// 障碍尺寸。必须与美术尺寸一一相等(主机测试核对),
// 且必须满足"最高速度最慢时也跳得过去"的时间窗不变量。
typedef struct {
    uint8_t w;
    uint8_t h;
    uint8_t y_bottom;
} chick_obstacle_shape_t;

// --- 收集道具(金币 / 宝石 / 音符 / 加速护身符)------------------------------
// 全部悬在空中,必须跳起来才能吃到:道具的最低高度高过最高的障碍(仙人掌 40px),
// 所以"想吃道具"永远不会和"躲障碍"打架 —— 这条不变量由主机测试守住。
// 得分规则:金币 +10,宝石 +30,音符 +10(音符另外触发一段《欢乐颂》),
// 加速护身符不加分 —— 它的价值是那 5.5 秒的加速与无敌。
// 池子按"最坏情况"定尺:最慢速度下屏上可同时有 6 个障碍(见 host 测试),
// 每个障碍最多带 3 个道具 —— 24 个槽位保证极端情况下也不会丢道具。
#define CHICK_RUN_MAX_PICKUPS     24   // 同时在场的道具上限
#define CHICK_RUN_COIN_POINTS     10
#define CHICK_RUN_GEM_POINTS      30
#define CHICK_RUN_NOTE_POINTS     10
#define CHICK_RUN_BOOST_POINTS    0
#define CHICK_RUN_PICKUP_MIN_Y    46    // 道具底边的最低离地高度 px(> 仙人掌 40)
// 跳跃顶点约 105px、碰撞盒高 40px,所以道具上边缘最高到 ~108px 仍然够得到。
#define CHICK_RUN_PICKUP_MAX_TOP  108

// --- 加速护身符 ---------------------------------------------------------
// 吃到后:自动加速 1.8 倍、撞碎碰到的障碍(不是"躲开",是"冲过去"),不加分。
// 持续 5.5 秒 —— 用户要求 5~6 秒,配合一段同长度的"冲刺"旋律。
#define CHICK_RUN_BOOST_MS             5500
#define CHICK_RUN_BOOST_SPEED_PERMILLE 1800   // 1.8 倍速(1000 = 原速)
// 音符道具:吃到后小鸡头顶飘一个 🎵 的时长。两首音乐旋律(《欢乐颂》/《小星星》)
// 都约 5.8 秒,音乐停了符号也消失,所以两首共用这一个常量。
#define CHICK_RUN_NOTE_FLOAT_MS        5800

typedef enum {
    CHICK_PICKUP_COIN = 0,   // 金币
    CHICK_PICKUP_GEM,        // 宝石
    CHICK_PICKUP_NOTE,       // 音符(音乐道具)
    CHICK_PICKUP_BOOST,      // 加速护身符
    CHICK_PICKUP_KIND_COUNT,
} chick_pickup_kind_t;

// 道具尺寸 == 美术尺寸(所见即所吃)。
typedef struct {
    uint8_t w;
    uint8_t h;
} chick_pickup_shape_t;

typedef struct {
    chick_pickup_kind_t kind;
    bool active;
    int32_t x_milli;   // 左边缘,单位 1/1000 像素
    int32_t y_milli;   // 底边离地高度,单位 1/1000 像素(与 chick_y_milli 同一基准)
} chick_pickup_t;

typedef struct {
    chick_obstacle_kind_t kind;
    bool active;
    int32_t x_milli;   // 左边缘,单位 1/1000 像素(避免慢速时整数截断)
} chick_obstacle_t;

typedef struct {
    chick_state_t state;
    chick_run_mode_t mode;  // 本局玩法,开局时确定
    uint32_t rng;
    uint32_t seed;          // 记录本局随机种子,便于复现问题
    uint32_t elapsed_ms;    // 本局已进行的游戏时间(暂停期间不累加)
    uint32_t score_m;       // 自由模式:本局距离;关卡模式:本关进度(米)
    uint32_t best_m;        // 自由模式历史最高(距离 + 道具加分,由 NVS 载入并在破纪录时更新)
    uint16_t coins;         // 本局已收集的金币数
    uint16_t gems;          // 本局已收集的宝石数
    uint16_t notes;         // 本局已收集的音符数(音乐道具)
    uint16_t boosts;        // 本局已收集的加速护身符数
    uint32_t bonus_points;  // 道具累计加分(已计入 total score)
    uint32_t boost_ms;      // 加速剩余时间(> 0 = 加速 + 无敌 + 撞碎障碍)
    uint32_t note_ms;       // 头顶音符飘动的剩余时间(只用于表现)
    uint16_t smashed;       // 加速期间撞碎的障碍数(日志/调试用)
    uint32_t level;         // 关卡模式:当前第几关(1 起);自由模式固定 1
    uint32_t best_level;    // 关卡模式:历史到达过的最高关
    uint8_t lives;          // 关卡模式:剩余命数
    uint32_t invincible_ms; // 掉命后的剩余无敌时间
    int32_t speed_base;     // 本关的基础滚动速度(关卡模式每关抬高)
    int32_t level_base_milli; // 本关起点在 distance_milli 上的位置
    int32_t distance_milli; // 未满 1 米的累计距离,单位 1/1000 像素
    bool new_best;          // 本局是否已经超过了开局时的历史最高分
    uint32_t next_gap_ms;   // 距离下一个障碍生成还有多久
    int32_t speed;          // 当前滚动速度 px/s
    int32_t vy_milli;       // 垂直速度,单位 1/1000 px/s
    int32_t chick_y_milli;  // 脚底离地高度,单位 1/1000 px
    bool on_ground;
    bool ducking;           // 本帧是否处于蹲下姿态(slide_ms > 0 且在地面)
    uint32_t slide_ms;      // 滑铲剩余时间
    bool jump_held;         // 玩家是否仍按住"上"
    uint32_t jump_hold_ms;  // 本次起跳已获得的助力时间
    uint32_t milestone_m;   // 下一个里程碑
    uint32_t dropped_spawns; // 障碍池满而丢弃的生成次数(正常应为 0,用于回归测试)
    uint32_t dropped_pickups; // 道具池满而丢弃的次数(道具是加分项,允许丢弃但要能看见)
    chick_obstacle_t obstacles[CHICK_RUN_MAX_OBSTACLES];
    chick_pickup_t pickups[CHICK_RUN_MAX_PICKUPS];
} chick_run_t;

// tick 返回的事件位掩码,UI 据此播音效/刷新 HUD/切页。
enum {
    CHICK_EV_NONE      = 0u,
    CHICK_EV_START     = 1u << 0,
    CHICK_EV_JUMP      = 1u << 1,
    CHICK_EV_LAND      = 1u << 2,
    CHICK_EV_HIT       = 1u << 3,
    CHICK_EV_GAME_OVER = 1u << 4,
    CHICK_EV_MILESTONE = 1u << 5,
    CHICK_EV_NEW_BEST  = 1u << 6,
    CHICK_EV_HURT      = 1u << 7,   // 关卡模式掉了一条命(未结束)
    CHICK_EV_LEVEL_UP  = 1u << 8,   // 关卡模式过关(已切到下一关)
    CHICK_EV_COIN      = 1u << 9,   // 吃到金币
    CHICK_EV_GEM       = 1u << 10,  // 吃到宝石
    CHICK_EV_NOTE      = 1u << 11,  // 吃到音符(UI 播放《欢乐颂》+ 头顶飘 🎵)
    CHICK_EV_BOOST     = 1u << 12,  // 吃到加速护身符(UI 播冲刺旋律 + 画光圈)
    CHICK_EV_SMASH     = 1u << 13,  // 加速期间撞碎了一个障碍
};

// 初始化到首页状态。seed 决定本局障碍序列,便于复现。
void chick_run_init(chick_run_t *run, uint32_t seed);

// 载入 NVS 中的历史最高分(自由模式)。
void chick_run_set_best(chick_run_t *run, uint32_t best_m);
// 载入 NVS 中的历史最高关(关卡模式)。
void chick_run_set_best_level(chick_run_t *run, uint32_t best_level);

// 关卡模式的每关参数(纯函数,主机测试用它们核对"永远跳得过去/不会落地即撞")。
int32_t chick_run_level_speed_base(uint32_t level);
uint32_t chick_run_level_gap_min(uint32_t level);
uint32_t chick_run_level_gap_max(uint32_t level);

// 关卡模式:本关已跑的距离(米);自由模式等同 score_m。
uint32_t chick_run_level_progress_m(const chick_run_t *run);

// 本局是否刷新了历史最高(用于结算页显示"新纪录")。
bool chick_run_is_new_best(const chick_run_t *run);

// 开始新的一局(READY/GAME_OVER -> RUNNING),mode 决定玩法。返回是否真的开始。
bool chick_run_start(chick_run_t *run, chick_run_mode_t mode);

// 推进 dt_ms 毫秒。返回 CHICK_EV_* 位掩码;非 RUNNING 状态不推进、返回 0。
uint32_t chick_run_tick(chick_run_t *run, uint32_t dt_ms);

// 按下跳跃键。仅在地面且正在跑动时生效,返回是否真的起跳。
bool chick_run_jump(chick_run_t *run);

// 松开跳跃键:结束长按助力,跳跃高度由按住时长决定。
void chick_run_jump_release(chick_run_t *run);

// 按下"下":开始/刷新一次滑铲。
// 之所以是"定时动作"而不是"按住才蹲":BSP 的按键事件只有按下/单击/长按,
// 长按超过 500ms 后不再有"松开"事件,按住语义会在设备上卡住。
void chick_run_slide(chick_run_t *run);

// 暂停/继续。仅 RUNNING 与 PAUSED 之间切换,返回是否发生了变化。
bool chick_run_toggle_pause(chick_run_t *run);

// 放弃本局,回到首页。
void chick_run_abort(chick_run_t *run);

// 当前碰撞盒:chick_y_milli 换算成像素高度。地面蹲下时变矮。
int32_t chick_run_chick_y(const chick_run_t *run);       // 脚底离地高度(px)
int32_t chick_run_chick_height(const chick_run_t *run);  // 当前碰撞盒高度(px)
int32_t chick_run_obstacle_x(const chick_obstacle_t *ob);  // 左边缘(px)
bool chick_run_obstacle_box(const chick_obstacle_t *ob, int32_t *left, int32_t *top,
                            int32_t *right, int32_t *bottom);

const chick_obstacle_shape_t *chick_run_obstacle_shape(chick_obstacle_kind_t kind);

// 道具尺寸表、位置与碰撞盒(纯查询,便于 HUD 与主机测试使用)。
const chick_pickup_shape_t *chick_run_pickup_shape(chick_pickup_kind_t kind);
int32_t chick_run_pickup_x(const chick_pickup_t *pickup);    // 左边缘(px)
int32_t chick_run_pickup_y(const chick_pickup_t *pickup);    // 底边离地高度(px)
void chick_run_pickup_box(const chick_pickup_t *pickup, int32_t *left, int32_t *bottom,
                          int32_t *right, int32_t *top);
// 吃到一个道具加多少分(金币 10 / 宝石 30 / 音符 10)。
uint32_t chick_run_pickup_points(chick_pickup_kind_t kind);

// 本局道具得分(已收集道具的加分总和)。
uint32_t chick_run_bonus_points(const chick_run_t *run);

// 是否处于加速状态(加速 + 无敌 + 撞碎障碍)。
bool chick_run_is_boosted(const chick_run_t *run);
// 加速剩余秒数(向上取整):HUD 倒计时显示用。未加速时为 0。
uint32_t chick_run_boost_seconds(const chick_run_t *run);
// 头顶音符剩余飘动时间(ms);0 = 不画。
uint32_t chick_run_note_float_ms(const chick_run_t *run);
// 本局撞碎的障碍数。
uint32_t chick_run_smashed(const chick_run_t *run);

// 本局总得分 = 距离(米) + 道具加分。自由模式的纪录、HUD 大号分数、结算都用它;
// 关卡模式的距离进度条仍然看 score_m(chick_run_level_progress_m)。
uint32_t chick_run_total_score(const chick_run_t *run);
