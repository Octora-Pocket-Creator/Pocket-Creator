#include "rtttl_wave.h"

#include <stdbool.h>
#include <stddef.h>

// 发声占比:留出 15% 作为音与音之间的断句。
#define RTTTL_SOUND_PERCENT 85U

// 颤音(振幅调制)的参数:11Hz 的三角波 LFO,幅度在满幅的 25%~100% 之间摆动。
// 听感是"嗡嗡往前冲"的推进感,与加速道具对应;用整数三角波而不是查表正弦,
// 既能直接算又不依赖任何表。
#define RTTTL_TREMOLO_HZ  11
#define RTTTL_TREMOLO_MIN 64     /* 256 制下的最低增益 */

// 调制增益(256 制),只依赖采样序号,所以分块渲染与整段渲染完全一致。
static uint32_t tremolo_gain(uint32_t index) {
    const uint32_t period = RTTTL_WAVE_SAMPLE_RATE / RTTTL_TREMOLO_HZ;
    const uint32_t half = period / 2U;
    const uint32_t t = index % period;
    const uint32_t level = t < half ? (t * 256U / half) : ((period - t) * 256U / half);
    return RTTTL_TREMOLO_MIN + (256U - RTTTL_TREMOLO_MIN) * level / 256U;
}

// 波形值。载波由 wave 决定:
//   * 方波 —— 音效,亮但高音容易"扎耳坟";
//   * 三角波 —— 谐波少、听感柔和(背景音乐与加速道具的冲刺旋律用);
//   * 三角波 + 振幅调制 —— 在柔和载波上叠颤音,保留推进感。
// 加速旋律曾在高八度上用方波,实机反馈"太刺耳",所以它改用三角波载波;
// 调制部分不受影响,仍是 11Hz、25%~100% 的幅度摆动。
static int16_t wave_raw(uint32_t phase, uint32_t index, int16_t amplitude, rtttl_wave_t wave) {
    const bool tremolo = wave == RTTTL_WAVE_TREMOLO;
    int16_t raw;
    if (wave == RTTTL_WAVE_TRIANGLE || tremolo) {
        const uint32_t half = RTTTL_WAVE_SAMPLE_RATE / 2U;
        const int32_t level = phase < half
            ? (int32_t)((uint64_t)phase * (uint32_t)amplitude / half)
            : (int32_t)((uint64_t)(RTTTL_WAVE_SAMPLE_RATE - phase) * (uint32_t)amplitude / half);
        raw = (int16_t)level;
    } else {
        raw = phase < RTTTL_WAVE_SAMPLE_RATE / 2U ? amplitude : (int16_t)-amplitude;
    }
    if (tremolo) {
        return (int16_t)((int32_t)raw * (int32_t)tremolo_gain(index) / 256);
    }
    return raw;
}

static void phase_advance(uint16_t frequency, uint32_t *phase) {
    *phase += frequency;
    if (*phase >= RTTTL_WAVE_SAMPLE_RATE) *phase -= RTTTL_WAVE_SAMPLE_RATE;
}

// 首尾各 RTTTL_WAVE_EDGE_SAMPLES 个采样线性淡入淡出。length 必须是
// 【真正发声的长度】,否则淡出会落到留白里,等于没有淡出。
// 末端精确收到 0:最后一个发声采样就是静音,和后面的留白之间没有阶跃。
static uint32_t edge_gain(uint32_t index, uint32_t length) {
    if (length == 0) return 0;
    uint32_t gain = 256U;
    if (index < RTTTL_WAVE_EDGE_SAMPLES) {
        gain = gain * index / RTTTL_WAVE_EDGE_SAMPLES;
    }
    const uint32_t clamped = index < length ? index : length - 1;
    const uint32_t remaining = length - 1 - clamped;
    if (remaining < RTTTL_WAVE_EDGE_SAMPLES) {
        gain = gain * remaining / RTTTL_WAVE_EDGE_SAMPLES;
    }
    return gain;
}

uint32_t rtttl_wave_total_samples(uint32_t duration_ms) {
    return (uint32_t)((uint64_t)RTTTL_WAVE_SAMPLE_RATE * duration_ms / 1000U);
}

uint32_t rtttl_wave_sound_samples(uint16_t frequency, uint32_t duration_ms) {
    if (frequency == 0) return 0;
    return (uint32_t)((uint64_t)RTTTL_WAVE_SAMPLE_RATE * duration_ms * RTTTL_SOUND_PERCENT /
                      100000U);
}

int16_t rtttl_wave_sample(uint16_t frequency, int16_t amplitude, rtttl_wave_t wave,
                          uint32_t index, uint32_t sound_samples, uint32_t *phase) {
    if (frequency == 0 || amplitude == 0 || phase == NULL) return 0;
    if (index >= sound_samples) return 0;

    const int16_t raw = wave_raw(*phase, index, amplitude, wave);
    phase_advance(frequency, phase);
    return (int16_t)((int32_t)raw * (int32_t)edge_gain(index, sound_samples) / 256);
}

void rtttl_wave_release(uint16_t frequency, int16_t amplitude, rtttl_wave_t wave,
                        uint32_t index, uint32_t *phase, int16_t *out) {
    if (out == NULL) return;
    for (uint32_t i = 0; i < RTTTL_WAVE_EDGE_SAMPLES; i++) {
        // 从满幅线性降到 0:最后一个采样正好是静音,和后面的留白平滑衔接。
        const uint32_t gain = 256U * (RTTTL_WAVE_EDGE_SAMPLES - 1U - i) /
                              (RTTTL_WAVE_EDGE_SAMPLES - 1U);
        if (frequency == 0 || amplitude == 0 || phase == NULL) {
            out[i] = 0;
            continue;
        }
        out[i] = (int16_t)((int32_t)wave_raw(*phase, index + i, amplitude, wave) *
                           (int32_t)gain / 256);
        phase_advance(frequency, phase);
    }
}
