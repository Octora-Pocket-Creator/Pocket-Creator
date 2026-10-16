// tests/test_rtttl_wave.c —— RTTTL 波形/包络的主机测试。
//
// 直接盯住这次的缺陷:发声段末尾必须是"幅度收敛到 0",不能是满幅硬切 ——
// 满幅切到静音就是听感上的"啪"一声爆音。用绝对值界限来断言,
// 而不是"听着好像没有"。
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rtttl_wave.h"

#define AMP 4600

static int16_t *render_note(uint16_t frequency, uint32_t duration_ms, rtttl_wave_t wave,
                            uint32_t *total_out, uint32_t *sound_out) {
    const uint32_t total = rtttl_wave_total_samples(duration_ms);
    const uint32_t sound = rtttl_wave_sound_samples(frequency, duration_ms);
    int16_t *samples = calloc(total, sizeof(int16_t));
    assert(samples != NULL);
    uint32_t phase = 0;
    for (uint32_t i = 0; i < total; i++) {
        samples[i] = rtttl_wave_sample(frequency, AMP, wave, i, sound, &phase);
    }
    *total_out = total;
    *sound_out = sound;
    return samples;
}

static void test_lengths(void) {
    assert(rtttl_wave_total_samples(300) == 4800);
    assert(rtttl_wave_sound_samples(440, 300) == 4080);          // 85% 发声 + 15% 断句
    assert(rtttl_wave_sound_samples(0, 300) == 0);                // 休止符不发声
    assert(rtttl_wave_total_samples(0) == 0);
}

// 核心回归:发声段首尾都必须淡入淡出,且末尾与后面的静音平滑衔接。
static void test_no_step_at_boundaries(void) {
    const rtttl_wave_t waves[] = {
        RTTTL_WAVE_SQUARE, RTTTL_WAVE_TRIANGLE, RTTTL_WAVE_TREMOLO,
    };
    for (size_t w = 0; w < sizeof(waves) / sizeof(waves[0]); w++) {
        uint32_t total = 0, sound = 0;
        int16_t *samples = render_note(440, 300, waves[w], &total, &sound);

        // 起手不能突然跳起来(第一个采样必须是静音,后面逐步抬升)。
        assert(samples[0] == 0);
        assert(abs(samples[1]) < AMP / 8);

        // 结尾:最后一个发声采样必须正好收敛到 0(与后面留白无阶跃)。
        // 这就是当初的 bug:按总长算包络时这里会是满幅(±AMP)。
        assert(samples[sound - 1] == 0);
        assert(labs((long)samples[sound - 2]) < AMP / 8);

        // 留白段必须完全静音。
        for (uint32_t i = sound; i < total; i++) assert(samples[i] == 0);

        free(samples);
    }
}

// 包络只影响首尾,中间段不能莫名其妙被压低。
static void test_body_at_full_amplitude(void) {
    uint32_t total = 0, sound = 0;
    int16_t *samples = render_note(440, 300, RTTTL_WAVE_SQUARE, &total, &sound);
    for (uint32_t i = RTTTL_WAVE_EDGE_SAMPLES; i < sound - RTTTL_WAVE_EDGE_SAMPLES; i++) {
        assert(samples[i] == AMP || samples[i] == -AMP);
    }
    free(samples);
}

// 相位跨调用保持连续:分块渲染与整段渲染必须逐采样一致。
static void test_phase_continuity(void) {
    const rtttl_wave_t waves[] = {
        RTTTL_WAVE_SQUARE, RTTTL_WAVE_TRIANGLE, RTTTL_WAVE_TREMOLO,
    };
    for (size_t w = 0; w < sizeof(waves) / sizeof(waves[0]); w++) {
        const uint32_t total = rtttl_wave_total_samples(200);
        const uint32_t sound = rtttl_wave_sound_samples(523, 200);
        int16_t *whole = calloc(total, sizeof(int16_t));
        int16_t *split = calloc(total, sizeof(int16_t));
        assert(whole && split);

        uint32_t phase_a = 0, phase_b = 0;
        for (uint32_t i = 0; i < total; i++) {
            whole[i] = rtttl_wave_sample(523, AMP, waves[w], i, sound, &phase_a);
        }
        for (uint32_t i = 0; i < total; i += 7) {           // 故意用非整除的分块
            for (uint32_t k = 0; k < 7 && i + k < total; k++) {
                split[i + k] = rtttl_wave_sample(523, AMP, waves[w], i + k, sound, &phase_b);
            }
        }
        for (uint32_t i = 0; i < total; i++) assert(whole[i] == split[i]);

        free(whole);
        free(split);
    }
}

// 颤音(振幅调制):身体段必须在"接近满幅"与"明显压低"之间摆动 ——
// 这是加速道具那段"嗡嗡往前冲"的旋律的核心听感,退化成普通方波就没效果了。
static void test_tremolo_modulates_amplitude(void) {
    uint32_t total = 0, sound = 0;
    int16_t *samples = render_note(440, 1000, RTTTL_WAVE_TREMOLO, &total, &sound);
    const uint32_t body_from = RTTTL_WAVE_EDGE_SAMPLES;
    const uint32_t body_to = sound - RTTTL_WAVE_EDGE_SAMPLES;

    int loud = 0, quiet = 0;
    for (uint32_t i = body_from; i < body_to; i++) {
        const int32_t mag = samples[i] < 0 ? -samples[i] : samples[i];
        assert(mag <= AMP);                                  // 不允许过冲
        if (mag >= AMP * 9 / 10) loud++;
        if (mag <= AMP * 1 / 2) quiet++;
    }
    assert(loud > 0);                                        // 有接近满幅的时刻
    assert(quiet > 0);                                       // 也有被压下去的时刻
    // 两个极端都要反复出现(LFO 在 1 秒里转 ~11 圈),而不是只有一次。
    assert(loud > 10 && quiet > 10);

    // 摇幅的时间尺度:调制周期 ≈ 采样率 / 11Hz(允许 ±20% 的量化误差)。
    uint32_t first_peak = 0;
    for (uint32_t i = body_from; i < body_to; i++) {
        const int32_t mag = samples[i] < 0 ? -samples[i] : samples[i];
        if (mag >= AMP * 9 / 10) { first_peak = i - body_from; break; }
    }
    const uint32_t expected = RTTTL_WAVE_SAMPLE_RATE / 22U;   // 半个周期到峰值
    assert(first_peak > expected * 8 / 10 && first_peak < expected * 12 / 10);

    free(samples);
}

// 被打断时的淡出段:从当前幅度平滑降到 0,末尾必须是静音。
static void test_release_ramp(void) {
    int16_t out[RTTTL_WAVE_EDGE_SAMPLES];
    for (int wave = 0; wave <= RTTTL_WAVE_TREMOLO; wave++) {
        const bool waves_is_square = wave == RTTTL_WAVE_SQUARE;
        uint32_t phase = 0;
        rtttl_wave_release(440, AMP, (rtttl_wave_t)wave, 0, &phase, out);
        assert(out[RTTTL_WAVE_EDGE_SAMPLES - 1] == 0);              // 收在静音
        for (uint32_t i = 0; i < RTTTL_WAVE_EDGE_SAMPLES; i++) {
            assert(labs((long)out[i]) <= AMP);
        }
        if (waves_is_square) {
            for (uint32_t i = 1; i < RTTTL_WAVE_EDGE_SAMPLES; i++) {
                assert(labs((long)out[i]) <= labs((long)out[i - 1]));   // 幅度不回升
            }
            assert(labs((long)out[0]) == AMP);                     // 从满幅开始降
        }
    }

    // 颤音的淡出要接着音符本身的调制相位走:从"低幅度处"被打断不能突然变响。
    // 载波是三角波 —— 它在相位 0 处恰好是 0,所以这里把相位放在半周期
    // (三角波的顶点、载波幅度最大)上比:这样比较的才是调制增益,而不是载波本身。
    {
        const uint32_t apex = RTTTL_WAVE_SAMPLE_RATE / 2U;
        uint32_t phase_low = apex;
        int16_t out_low[RTTTL_WAVE_EDGE_SAMPLES];
        rtttl_wave_release(440, AMP, RTTTL_WAVE_TREMOLO, 0, &phase_low, out_low);
        uint32_t phase_high = apex;
        int16_t out_high[RTTTL_WAVE_EDGE_SAMPLES];
        const uint32_t half_period = RTTTL_WAVE_SAMPLE_RATE / 22U;
        rtttl_wave_release(440, AMP, RTTTL_WAVE_TREMOLO, half_period, &phase_high, out_high);
        assert(labs((long)out_high[0]) > labs((long)out_low[0]));
        assert(labs((long)out_high[0]) > AMP / 2);      // 高幅度相位从接近满幅开始
        assert(out_high[RTTTL_WAVE_EDGE_SAMPLES - 1] == 0);
    }

    // 休止符被"打断"时不能凭空发出声音。
    uint32_t phase = 0;
    rtttl_wave_release(0, AMP, RTTTL_WAVE_SQUARE, 0, &phase, out);
    for (uint32_t i = 0; i < RTTTL_WAVE_EDGE_SAMPLES; i++) assert(out[i] == 0);
}

static void test_degenerate_inputs(void) {
    uint32_t phase = 0;
    assert(rtttl_wave_sample(0, AMP, RTTTL_WAVE_SQUARE, 0, 100, &phase) == 0);
    assert(rtttl_wave_sample(440, 0, RTTTL_WAVE_SQUARE, 0, 100, &phase) == 0);
    assert(rtttl_wave_sample(440, AMP, RTTTL_WAVE_SQUARE, 100, 100, &phase) == 0);
    rtttl_wave_release(440, AMP, RTTTL_WAVE_SQUARE, 0, &phase, NULL);   // 空指针必须安全
}

int main(void) {
    test_lengths();
    test_no_step_at_boundaries();
    test_body_at_full_amplitude();
    test_phase_continuity();
    test_tremolo_modulates_amplitude();
    test_release_ramp();
    test_degenerate_inputs();
    printf("rtttl wave tests: PASS\n");
    return 0;
}
