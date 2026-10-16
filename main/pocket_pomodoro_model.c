// main/pocket_pomodoro_model.c —— 《番茄闹钟》计时规则实现。见 .h 的说明。
#include "pocket_pomodoro_model.h"

#include <stdio.h>

// 对齐到格点并夹在 [min, max] 里。用固定步长而不是自由加减:上下一格一格地跳,
// 用户按几下就知道自己调到哪儿了。
static uint16_t clamp_minutes(int value, uint16_t min_min, uint16_t max_min, uint16_t step) {
    const int lo = min_min, hi = max_min;
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    return (uint16_t)(lo + (value - lo) / step * step);
}

static bool is_focus(pomo_phase_t phase) { return phase == POMO_PHASE_FOCUS; }

// 当前/即将运行的那一段。POMO_PHASE_DONE 只是个"通知窗口",它跑哪一段由 next 决定;
// 标题页上摆的是专注时长(按下 A 就走专注),所以也归到专注 ——
// 所有跟时长有关的计算都要先过这里,否则会把休息的时长当成专注的用。
static pomo_phase_t active_phase(const pomo_timer_t *t) {
    if (t->phase == POMO_PHASE_DONE) return (pomo_phase_t)t->next;
    if (t->phase == POMO_PHASE_TITLE) return POMO_PHASE_FOCUS;
    return t->phase;
}

// 某个阶段跑一整段要多久(毫秒)。
static uint32_t segment_ms(const pomo_timer_t *t, pomo_phase_t phase) {
    const uint32_t minutes = is_focus(phase) ? t->focus_min : t->break_min;
    return minutes * 60000u;
}

// 从此刻起开始走当前阶段,剩余量取 leftover_ms(夹在本段时长内)。
static void arm(pomo_timer_t *t, uint32_t now_ms) {
    t->phase = active_phase(t);   // 通知窗口结束/按 A 时,这一步才真正落到下一段
    const uint32_t total = segment_ms(t, t->phase);
    if (t->leftover_ms > total) t->leftover_ms = total;
    t->deadline_ms = now_ms + t->leftover_ms;
    t->remaining_ms = t->leftover_ms;
    t->running = true;
}

// 进入某个阶段的"准备"状态:停着,剩余量是整段时长。
static void stage_idle(pomo_timer_t *t, pomo_phase_t phase) {
    t->phase = phase;
    t->running = false;
    t->leftover_ms = segment_ms(t, phase);
    t->remaining_ms = t->leftover_ms;
    t->deadline_ms = 0;
}

// 当前阶段走完。专注 → 完成数 +1;下一段是另一段,但先把大字提示亮出来,不马上跳。
static void advance(pomo_timer_t *t, uint32_t now_ms) {
    pomo_phase_t next = POMO_PHASE_FOCUS;
    if (t->phase == POMO_PHASE_FOCUS) {
        if (t->done_count < UINT16_MAX) t->done_count++;
        next = POMO_PHASE_BREAK;
    }
    stage_idle(t, next);          // 先把下一段的剩余量摆好,通知窗口结束就能直接走
    t->phase = POMO_PHASE_DONE;
    t->next = (uint8_t)next;
    t->notify_ms = now_ms + POMO_NOTIFY_MS;
}

void pomo_init(pomo_timer_t *t) {
    t->phase = POMO_PHASE_TITLE;
    t->running = false;
    t->focus_min = POMO_FOCUS_DEFAULT_MIN;
    t->break_min = POMO_BREAK_DEFAULT_MIN;
    t->done_count = 0;
    t->next = (uint8_t)POMO_PHASE_FOCUS;
    t->leftover_ms = (uint32_t)POMO_FOCUS_DEFAULT_MIN * 60000u;
    t->remaining_ms = t->leftover_ms;
    t->deadline_ms = 0;
    t->notify_ms = 0;
}

bool pomo_set_duration(pomo_timer_t *t, bool focus, uint16_t minutes) {
    if (focus) {
        const uint16_t value = clamp_minutes(minutes, POMO_FOCUS_MIN_MIN, POMO_FOCUS_MAX_MIN,
                                             POMO_FOCUS_STEP_MIN);
        if (value == t->focus_min) return false;
        t->focus_min = value;
    } else {
        const uint16_t value = clamp_minutes(minutes, POMO_BREAK_MIN_MIN, POMO_BREAK_MAX_MIN,
                                             POMO_BREAK_STEP_MIN);
        if (value == t->break_min) return false;
        t->break_min = value;
    }
    // 正在走的计时不打断:改的是"设定",新时长从下一段开始生效。
    // 停着的这一段(含通知窗口里已经摆好的下一段)则当场跟着变,所见即所得。
    if (!t->running) {
        const pomo_phase_t shown = active_phase(t);
        if ((shown == POMO_PHASE_FOCUS || shown == POMO_PHASE_BREAK) && is_focus(shown) == focus) {
            t->leftover_ms = segment_ms(t, shown);
            t->remaining_ms = t->leftover_ms;
        }
    }
    return true;
}

bool pomo_toggle(pomo_timer_t *t, uint32_t now_ms) {
    switch (t->phase) {
        case POMO_PHASE_TITLE:
            stage_idle(t, POMO_PHASE_FOCUS);
            arm(t, now_ms);
            return true;
        case POMO_PHASE_DONE:
            // 通知窗口里按 A = 立刻开始下一段(阶段在 advance() 里已经定好)。
            arm(t, now_ms);
            return true;
        case POMO_PHASE_FOCUS:
        case POMO_PHASE_BREAK:
            if (t->running) {
                // 暂停:把剩余量从"绝对时刻"换算回来冻住,继续时再算新的截止时刻。
                const int32_t left = (int32_t)(t->deadline_ms - now_ms);
                t->leftover_ms = left > 0 ? (uint32_t)left : 0;
                t->remaining_ms = t->leftover_ms;
                t->running = false;
            } else {
                arm(t, now_ms);
            }
            return true;
        default:
            return false;
    }
}

bool pomo_reset(pomo_timer_t *t, uint32_t now_ms) {
    (void)now_ms;   // 重置只是把剩余量恢复成整段,不需要时刻
    switch (t->phase) {
        case POMO_PHASE_FOCUS:
        case POMO_PHASE_BREAK:
            stage_idle(t, t->phase);
            return true;
        case POMO_PHASE_DONE:
            // 通知窗口里按 B:停在下一段的准备状态,不再自动开始。
            stage_idle(t, (pomo_phase_t)t->next);
            return true;
        default:
            return false;   // 标题页没有可重置的东西
    }
}

bool pomo_adjust(pomo_timer_t *t, bool up) {
    // 通知窗口里按上下没有意义(那一段还没开始走),标题页调的是专注时长。
    if (t->phase == POMO_PHASE_DONE) return false;
    const bool focus = is_focus(active_phase(t));
    const int step = focus ? POMO_FOCUS_STEP_MIN : POMO_BREAK_STEP_MIN;
    const int current = focus ? t->focus_min : t->break_min;
    return pomo_set_duration(t, focus, (uint16_t)(current + (up ? step : -step)));
}

bool pomo_switch(pomo_timer_t *t, bool forward) {
    // 计时中不切:按一下就把整段丢掉太容易误触。先按 A 暂停,再左右切。
    if (t->running) return false;
    switch (t->phase) {
        case POMO_PHASE_TITLE:
            // 标题页:右 = 去专注(默认入口),左 = 直接去休息。
            stage_idle(t, forward ? POMO_PHASE_FOCUS : POMO_PHASE_BREAK);
            return true;
        case POMO_PHASE_FOCUS:
        case POMO_PHASE_BREAK:
            // 就两段,左右都是"去另一段"。
            stage_idle(t, is_focus(t->phase) ? POMO_PHASE_BREAK : POMO_PHASE_FOCUS);
            return true;
        case POMO_PHASE_DONE:
            // 通知窗口里按左右 = 不等了,撤掉提示,回到另一段的准备状态。
            stage_idle(t, is_focus((pomo_phase_t)t->next) ? POMO_PHASE_BREAK : POMO_PHASE_FOCUS);
            return true;
        default:
            return false;
    }
}

bool pomo_tick(pomo_timer_t *t, uint32_t now_ms) {
    if (t->phase == POMO_PHASE_DONE) {
        if ((int32_t)(now_ms - t->notify_ms) >= 0) {
            arm(t, now_ms);   // 通知窗口结束:自动开始下一段
            return true;
        }
        return false;
    }
    if (!t->running) return false;

    const int32_t left = (int32_t)(t->deadline_ms - now_ms);
    if (left > 0) {
        t->remaining_ms = (uint32_t)left;
        return false;
    }
    t->remaining_ms = 0;
    advance(t, now_ms);
    return true;
}

void pomo_format_mmss(uint32_t seconds, char *out /*[6]*/) {
    if (seconds > 99u * 60u + 59u) seconds = 99u * 60u + 59u;   // 显示宽度就 5 个字符
    snprintf(out, 6, "%02u:%02u", (unsigned)(seconds / 60u), (unsigned)(seconds % 60u));
}

int pomo_progress_pct(const pomo_timer_t *t) {
    if (t->phase != POMO_PHASE_FOCUS && t->phase != POMO_PHASE_BREAK) return 0;
    const uint32_t total = segment_ms(t, t->phase);
    if (total == 0) return 0;
    const uint32_t left = t->remaining_ms > total ? total : t->remaining_ms;
    return (int)(((total - left) * 100u) / total);
}

bool pomo_flash_on(const pomo_timer_t *t, uint32_t now_ms) {
    if (t->phase != POMO_PHASE_DONE) return false;
    const uint32_t elapsed = now_ms - (t->notify_ms - POMO_NOTIFY_MS);
    return (elapsed / POMO_FLASH_PERIOD_MS) % 2u == 0u;
}
