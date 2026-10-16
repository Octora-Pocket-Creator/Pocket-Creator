// tests/test_pomodoro_model.c —— 《番茄闹钟》计时规则的主机测试。
//
// 这些规则在设备上出错只表现为"时间不对/数字不动/该提醒时不提醒",拿手机掐表很难
// 定位,所以把边界(暂停冻结、重置、调时长、阶段切换、通知窗口超时、完成计数)
// 在这里钉死。整段测试不睡真实时间:全部靠喂 now_ms 快进。
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pocket_pomodoro_model.h"

// 默认 25 分钟专注,用真实毫秒推演;比"调成 1 分钟"更接近设备上的行为。
#define FOCUS_MS ((uint32_t)POMO_FOCUS_DEFAULT_MIN * 60000u)
#define BREAK_MS ((uint32_t)POMO_BREAK_DEFAULT_MIN * 60000u)

static void test_starts_on_title_and_waits(void) {
    pomo_timer_t t;
    pomo_init(&t);
    assert(t.phase == POMO_PHASE_TITLE);
    assert(!t.running);
    assert(t.focus_min == POMO_FOCUS_DEFAULT_MIN);
    assert(t.break_min == POMO_BREAK_DEFAULT_MIN);
    assert(t.done_count == 0);
    // 标题页上挂很久也不该自己开始计时。
    for (uint32_t now = 0; now < 600000; now += 1000) {
        assert(!pomo_tick(&t, now));
    }
    assert(t.phase == POMO_PHASE_TITLE);
}

static void test_a_starts_focus_countdown(void) {
    pomo_timer_t t;
    pomo_init(&t);
    assert(pomo_toggle(&t, 0));
    assert(t.phase == POMO_PHASE_FOCUS);
    assert(t.running);
    assert(t.remaining_ms == FOCUS_MS);

    assert(!pomo_tick(&t, FOCUS_MS - 1));
    assert(t.remaining_ms == 1);

    // UI 按"向上取整的秒"显示(见 pocket_pomodoro_ui.c):还剩 1ms 也要显示 00:01,
    // 归零那一刻才显示 00:00 —— 否则最后不足一秒会提前跳成 00:00 再等一秒。
    char buf[6];
    pomo_format_mmss((t.remaining_ms + 999) / 1000, buf);
    assert(strcmp(buf, "00:01") == 0);
    pomo_format_mmss((t.remaining_ms - 1 + 999) / 1000, buf);
    assert(strcmp(buf, "00:00") == 0);
}

static void test_timer_advances_at_the_deadline(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 1000);

    // 差一毫秒不算走完。
    assert(!pomo_tick(&t, 1000 + FOCUS_MS - 1));
    assert(t.phase == POMO_PHASE_FOCUS);

    assert(pomo_tick(&t, 1000 + FOCUS_MS));
    assert(t.phase == POMO_PHASE_DONE);
    assert(t.done_count == 1);                     // 专注走完才算一个番茄
    assert(!t.running);                            // 通知窗口里是停着的
    assert(t.notify_ms == 1000 + FOCUS_MS + POMO_NOTIFY_MS);
    // 通知窗口显示的是"下一段"的整段时长(画面:大字提示 + 休息 05:00),
    // 而不是一个已经归零的 00:00 —— 用户抬眼就知道接下来要歇多久。
    assert(t.remaining_ms == BREAK_MS);
}

static void test_notify_window_auto_starts_break(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    const uint32_t finished = FOCUS_MS;
    pomo_tick(&t, finished);

    // 通知窗口内一直停着,不自己往前走。
    assert(!pomo_tick(&t, finished + POMO_NOTIFY_MS - 1));
    assert(t.phase == POMO_PHASE_DONE);

    // 窗口过去 → 自动进入休息,并从头开始走。
    assert(pomo_tick(&t, finished + POMO_NOTIFY_MS));
    assert(t.phase == POMO_PHASE_BREAK);
    assert(t.running);
    assert(t.remaining_ms == BREAK_MS);
}

static void test_a_during_notify_starts_next_stage_now(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, FOCUS_MS);
    assert(t.phase == POMO_PHASE_DONE);

    assert(pomo_toggle(&t, FOCUS_MS + 500));
    assert(t.phase == POMO_PHASE_BREAK);   // 不该又回到专注
    assert(t.running);
    assert(t.remaining_ms == BREAK_MS);
}

static void test_b_during_notify_stops_auto_start(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, FOCUS_MS);
    assert(pomo_reset(&t, FOCUS_MS + 100));
    assert(t.phase == POMO_PHASE_BREAK);
    assert(!t.running);                     // 停在休息的准备状态,不再自动开始

    // 窗口早就过去了,也不该偷偷启动。
    assert(!pomo_tick(&t, FOCUS_MS + POMO_NOTIFY_MS + 5000));
    assert(t.phase == POMO_PHASE_BREAK && !t.running);
}

static void test_pause_freezes_the_remaining_time(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, 60000);                   // 走了 1 分钟
    assert(t.remaining_ms == FOCUS_MS - 60000);

    assert(pomo_toggle(&t, 60000));         // 暂停
    assert(!t.running);
    const uint32_t frozen = t.remaining_ms;

    // 暂停期间无论过多久,剩余量都不动。
    for (uint32_t now = 60000; now < 60000 + 300000; now += 5000) {
        assert(!pomo_tick(&t, now));
        assert(t.remaining_ms == frozen);
    }

    assert(pomo_toggle(&t, 360000));        // 继续:从冻住的地方接着走
    assert(t.running);
    assert(t.remaining_ms == frozen);
    assert(!pomo_tick(&t, 360000 + frozen - 1));
    assert(pomo_tick(&t, 360000 + frozen));
    assert(t.phase == POMO_PHASE_DONE);
}

static void test_reset_restores_full_segment_and_keeps_count(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, 120000);                  // 走了 2 分钟
    assert(pomo_reset(&t, 120000));
    assert(t.phase == POMO_PHASE_FOCUS);
    assert(!t.running);
    assert(t.remaining_ms == FOCUS_MS);

    // 跑完一个番茄,再重置:完成数不该被抹掉(它记的是"今天做了几个",不是本段进度)。
    pomo_toggle(&t, 1000000);
    pomo_tick(&t, 1000000 + FOCUS_MS);
    assert(t.done_count == 1);
    pomo_reset(&t, 1000000 + FOCUS_MS + 10);
    assert(t.done_count == 1);
    assert(t.phase == POMO_PHASE_BREAK);

    // 标题页上按 B:没什么可重置的。
    pomo_timer_t title;
    pomo_init(&title);
    assert(!pomo_reset(&title, 0));
    assert(title.phase == POMO_PHASE_TITLE);
}

static void test_focus_break_cycle_keeps_counting(void) {
    pomo_timer_t t;
    pomo_init(&t);
    uint32_t now = 0;
    pomo_toggle(&t, now);

    // 走完 3 个完整番茄(专注 + 休息),完成数应该是 3。
    for (int i = 1; i <= 3; i++) {
        now += FOCUS_MS;
        pomo_tick(&t, now);                 // 专注结束 → 通知窗口
        assert(t.phase == POMO_PHASE_DONE);
        assert(t.done_count == (uint16_t)i);
        now += POMO_NOTIFY_MS;
        pomo_tick(&t, now);                 // 自动进休息
        assert(t.phase == POMO_PHASE_BREAK);
        now += BREAK_MS;
        pomo_tick(&t, now);                 // 休息结束 → 也是通知窗口,但不计数
        assert(t.phase == POMO_PHASE_DONE);
        assert(t.done_count == (uint16_t)i);
        now += POMO_NOTIFY_MS;
        pomo_tick(&t, now);                 // 自动回到专注
        assert(t.phase == POMO_PHASE_FOCUS);
    }
    assert(t.done_count == 3);
}

static void test_up_down_adjusts_current_phase_only(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);

    // 计时进行中按上下:只改"设定",这一段照旧走完(不然按一下就等于偷走/加时)。
    assert(pomo_adjust(&t, true));          // 25 → 30
    assert(t.focus_min == 30);
    assert(t.break_min == POMO_BREAK_DEFAULT_MIN);
    assert(t.remaining_ms == FOCUS_MS);

    // 暂停后再调,屏幕上数字当场变成新时长 —— 这才是用户调时长的典型路径。
    assert(pomo_toggle(&t, 1000));
    assert(!t.running);
    for (int i = 0; i < 3; i++) assert(pomo_adjust(&t, false));   // 30 → 15
    assert(t.focus_min == 15);
    assert(t.remaining_ms == 15u * 60000u);

    // 下限 5 分钟:再按也不掉下去。
    for (int i = 0; i < 10; i++) pomo_adjust(&t, false);
    assert(t.focus_min == POMO_FOCUS_MIN_MIN);
    assert(t.remaining_ms == (uint32_t)POMO_FOCUS_MIN_MIN * 60000u);

    // 上限 60 分钟:一路按上去停在上限,不回绕。
    for (int i = 0; i < 40; i++) pomo_adjust(&t, true);
    assert(t.focus_min == POMO_FOCUS_MAX_MIN);

    // 切到休息再调:只动休息那一项,专注不受影响。
    const uint16_t focus_before = t.focus_min;
    assert(pomo_switch(&t, true));
    assert(t.phase == POMO_PHASE_BREAK);
    for (int i = 0; i < 3; i++) assert(pomo_adjust(&t, true));    // 5 → 8
    assert(t.break_min == 8);
    assert(t.focus_min == focus_before);
    assert(t.remaining_ms == 8u * 60000u);

    // 标题页上就能把专注时长调好再开始(那是按 A 之前的自然动作),
    // 画面上的 25:00 也跟着变成 30:00。
    pomo_timer_t title;
    pomo_init(&title);
    assert(pomo_adjust(&title, true));
    assert(title.focus_min == 30);
    assert(title.remaining_ms == 30u * 60000u);
    assert(title.phase == POMO_PHASE_TITLE);      // 调时长不该把标题页顶掉
    pomo_toggle(&title, 0);
    assert(title.phase == POMO_PHASE_FOCUS);
    assert(title.remaining_ms == 30u * 60000u);   // 用刚调好的时长开局
}

static void test_adjust_is_rejected_during_notify_window(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, FOCUS_MS);
    assert(t.phase == POMO_PHASE_DONE);
    // 通知窗口里按上下不该改任何东西:这一段还没开始走,改了也没地方体现。
    assert(!pomo_adjust(&t, true));
    assert(!pomo_adjust(&t, false));
    assert(t.focus_min == POMO_FOCUS_DEFAULT_MIN);
    assert(t.break_min == POMO_BREAK_DEFAULT_MIN);
}

static void test_duration_change_does_not_cut_a_running_timer(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, 60000);
    const uint32_t before = t.remaining_ms;

    // 计时进行中:调时长只改"设定",当前这一段照走不误。
    assert(pomo_set_duration(&t, true, 30));
    assert(t.focus_min == 30);
    assert(t.remaining_ms == before);
    assert(!pomo_tick(&t, 61000));          // 只是数字在走,不是阶段变化
    assert(t.remaining_ms == before - 1000);
}

static void test_switch_ignored_while_running(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    assert(!pomo_switch(&t, true));          // 计时中不切,免得误触丢掉整段
    assert(t.phase == POMO_PHASE_FOCUS);

    pomo_toggle(&t, 1000);                   // 暂停后可以切
    assert(pomo_switch(&t, true));
    assert(t.phase == POMO_PHASE_BREAK);
    assert(!t.running);

    // 标题页也允许切,左键直接落到休息。
    pomo_timer_t title;
    pomo_init(&title);
    assert(pomo_switch(&title, false));
    assert(title.phase == POMO_PHASE_BREAK);
}

static void test_format_mmss(void) {
    char buf[6];
    pomo_format_mmss(0, buf);
    assert(strcmp(buf, "00:00") == 0);
    pomo_format_mmss(59, buf);
    assert(strcmp(buf, "00:59") == 0);
    pomo_format_mmss(60, buf);
    assert(strcmp(buf, "01:00") == 0);
    pomo_format_mmss(25 * 60, buf);
    assert(strcmp(buf, "25:00") == 0);
    pomo_format_mmss(99 * 60 + 59, buf);
    assert(strcmp(buf, "99:59") == 0);
    // 超过两位数的分钟要封顶,不能写爆 6 字节缓冲。
    pomo_format_mmss(999999, buf);
    assert(strcmp(buf, "99:59") == 0);
}

static void test_progress_pct_edges(void) {
    pomo_timer_t t;
    pomo_init(&t);
    assert(pomo_progress_pct(&t) == 0);      // 标题页没有进度可言

    pomo_toggle(&t, 0);
    assert(pomo_progress_pct(&t) == 0);
    pomo_tick(&t, FOCUS_MS / 2);
    assert(pomo_progress_pct(&t) == 50);
    pomo_tick(&t, FOCUS_MS - 1);
    assert(pomo_progress_pct(&t) == 99);     // 差一毫秒也不该提前显示 100
    pomo_tick(&t, FOCUS_MS);
    assert(pomo_progress_pct(&t) == 0);      // 进通知窗口,进度归零
}

static void test_flash_only_during_notify_window(void) {
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    assert(!pomo_flash_on(&t, 12345));       // 专注中不闪

    pomo_tick(&t, FOCUS_MS);
    // 通知窗口内:每 POMO_FLASH_PERIOD_MS 交替一次。
    assert(pomo_flash_on(&t, FOCUS_MS));
    assert(!pomo_flash_on(&t, FOCUS_MS + POMO_FLASH_PERIOD_MS));
    assert(pomo_flash_on(&t, FOCUS_MS + 2 * POMO_FLASH_PERIOD_MS));
    // 窗口结束后不再闪(这时已经自动开始下一段)。
    pomo_tick(&t, FOCUS_MS + POMO_NOTIFY_MS);
    assert(!pomo_flash_on(&t, FOCUS_MS + POMO_NOTIFY_MS));
}

static void test_stale_timestamp_does_not_finish_the_segment(void) {
    // now_ms 只增不减,但帧循环偶尔会连着两帧拿到同一个时刻。重复喂同一时刻
    // 不该把剩余量算成一个巨大的数、更不该提前结束本段。
    pomo_timer_t t;
    pomo_init(&t);
    pomo_toggle(&t, 0);
    pomo_tick(&t, 60000);
    assert(t.remaining_ms == FOCUS_MS - 60000);
    assert(!pomo_tick(&t, 60000));           // 时刻没变:不算走完
    assert(t.remaining_ms == FOCUS_MS - 60000);
    assert(t.phase == POMO_PHASE_FOCUS);
}

int main(void) {
    test_starts_on_title_and_waits();
    test_a_starts_focus_countdown();
    test_timer_advances_at_the_deadline();
    test_notify_window_auto_starts_break();
    test_a_during_notify_starts_next_stage_now();
    test_b_during_notify_stops_auto_start();
    test_pause_freezes_the_remaining_time();
    test_reset_restores_full_segment_and_keeps_count();
    test_focus_break_cycle_keeps_counting();
    test_up_down_adjusts_current_phase_only();
    test_adjust_is_rejected_during_notify_window();
    test_duration_change_does_not_cut_a_running_timer();
    test_switch_ignored_while_running();
    test_format_mmss();
    test_progress_pct_edges();
    test_flash_only_during_notify_window();
    test_stale_timestamp_does_not_finish_the_segment();
    puts("test_pomodoro_model: PASS");
    return 0;
}
