// main/pocket_pomodoro_model.h —— 《番茄闹钟》的计时规则(纯逻辑,零 ESP-IDF 依赖)。
//
// 分层和 pocket_reflex_model.c 一个套路:阶段、倒计时、暂停/重置、完成数都在这里,
// 只吃 now_ms,不碰硬件、不碰 RTOS、不开定时器 —— 主机上直接编译就能测
// (tests/test_pomodoro_model.c),设备上出错又只表现为"时间不对"的规则全部钉在这层。
// 画面与按键接线在 pocket_pomodoro_ui.c。
//
// 时间基准用调用方传进来的单调毫秒(设备上是 esp_timer)。不存"还剩多少毫秒",
// 只存"本阶段什么时候结束",这样暂停/继续只要加减一次偏移,不会累积漂移。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POMO_PHASE_TITLE = 0,  // 标题页,等 A
    POMO_PHASE_FOCUS,      // 专注中
    POMO_PHASE_BREAK,      // 休息中
    // 本阶段刚走完:大字 + 闪屏提醒,等 A 进下一段(不按也会在 POMO_NOTIFY_MS 后自动进)。
    // 这个窗口里 remaining_ms 摆的是【下一段】的整段时长,画面据此显示"休息 05:00"。
    POMO_PHASE_DONE,
} pomo_phase_t;

typedef enum {
    POMO_KEY_UP = 0,
    POMO_KEY_DOWN,
    POMO_KEY_LEFT,
    POMO_KEY_RIGHT,
    POMO_KEY_A,
    POMO_KEY_B,
} pomo_key_t;

// --- 时长范围(分钟)。上下键在这两个区间内以一格的步长调整 -------------------
#define POMO_FOCUS_MIN_MIN     5
#define POMO_FOCUS_MAX_MIN     60
#define POMO_FOCUS_STEP_MIN    5
#define POMO_BREAK_MIN_MIN     1
#define POMO_BREAK_MAX_MIN     30
#define POMO_BREAK_STEP_MIN    1

// 默认:经典番茄钟。
#define POMO_FOCUS_DEFAULT_MIN 25
#define POMO_BREAK_DEFAULT_MIN 5

// 阶段刚走完的"通知窗口":大字提示 + 闪屏。期间 A 直接开始下一阶段,
// 窗口过去后自动开始。休息自动开始是有意的 —— 专注结束那一下才是需要提醒的,
// 提醒完就该进入休息,不然用户很容易在"等一个按钮"里把休息也等没了。
#define POMO_NOTIFY_MS         8000

// 通知窗口的屏幕闪烁周期(亮/暗交替一次)。
#define POMO_FLASH_PERIOD_MS   400

typedef struct {
    pomo_phase_t phase;
    bool running;          // 倒计时是否在走(标题页/通知窗口为 false)
    uint16_t focus_min;    // 专注时长(分钟),可调,掉电后从 NVS 回来
    uint16_t break_min;    // 休息时长(分钟)
    uint16_t done_count;   // 已完成的番茄数(专注走完一次 +1,休息不算)
    uint32_t remaining_ms; // 当前阶段剩余毫秒;每 tick 由 deadline - now 刷新
    // --- 以下是内部状态。UI 只用 next 一个:通知窗口里判断"刚才结束的是哪一段",
    //     好把大字提示写成"专注结束"而不是"休息开始"。---
    uint32_t deadline_ms;  // 运行中:本阶段结束的绝对时刻
    uint32_t notify_ms;    // 通知窗口结束的绝对时刻(仅 POMO_PHASE_DONE)
    uint32_t leftover_ms;  // 暂停时冻结的剩余量(继续时用它重算 deadline)
    uint8_t next;          // 通知窗口结束后要走的阶段(仅 POMO_PHASE_DONE 有效)
} pomo_timer_t;

// 标题页状态,时长取默认值。设备上接着用 pomo_set_duration() 覆盖成 NVS 里的值。
// 标题页摆的就是专注时长(按 A 即开始专注),所以这时上下键调的是专注。
void pomo_init(pomo_timer_t *t);

// 设置当前阶段的时长,并让它成为这一阶段新的剩余量(仅未运行时接受)。
// 返回 true = 改了,UI 需要重画。
bool pomo_set_duration(pomo_timer_t *t, bool focus, uint16_t minutes);

// A:开始 / 暂停 / 继续;通知窗口里按 A 直接进下一个阶段。
// 返回 true = 状态变了,UI 需要重画。
bool pomo_toggle(pomo_timer_t *t, uint32_t now_ms);

// B:回到准备状态 —— 计时停下,剩余量恢复成整段时长,完成数保留。
bool pomo_reset(pomo_timer_t *t, uint32_t now_ms);

// 上/下:在 POMO_PHASE_FOCUS / POMO_PHASE_BREAK 里调本阶段时长,其它阶段不动。
// up = 加一格,down = 减一格。返回 true = 改了。
bool pomo_adjust(pomo_timer_t *t, bool up);

// 左/右:切到另一段(只有两段,所以左右等价)。停着时才接受 —— 计时中途按一下
// 就把 25 分钟的专注丢掉,那是误触不是操作。forward 只影响标题页的默认落点。
bool pomo_switch(pomo_timer_t *t, bool forward);

// 每帧调用一次。返回 true = 状态变了(阶段切换 / 通知窗口结束),UI 需要重画。
bool pomo_tick(pomo_timer_t *t, uint32_t now_ms);

// mm:ss 格式化(number = 总秒数)。缓冲区至少 6 字节。
void pomo_format_mmss(uint32_t seconds, char *out /*[6]*/);

// 本阶段进度 0..100(%)。标题页返回 0。
int pomo_progress_pct(const pomo_timer_t *t);

// 闪烁相位:通知窗口内按 POMO_FLASH_PERIOD_MS 交替亮暗,其它阶段恒为 false。
// 给"专注结束"的视觉提醒用 —— 屏幕明暗交替,离得远也看得见。
bool pomo_flash_on(const pomo_timer_t *t, uint32_t now_ms);
