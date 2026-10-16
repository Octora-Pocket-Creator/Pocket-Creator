// main/rtttl_wave.h —— RTTTL 的波形与包络(纯逻辑,不依赖 ESP-IDF)。
//
// 单独成层的理由:爆音的本质是"相邻采样之间出现阶跃",靠耳朵只能听出"有咔哒",
// 很难判断是哪一段;抽出来之后,主机测试可以直接断言"发声段末尾的幅度必须收敛到 0"。
#pragma once

#include <stdint.h>

#define RTTTL_WAVE_SAMPLE_RATE  16000
// 每个音符首尾的淡入淡出采样数(16kHz 下 48 ≈ 3ms)。
#define RTTTL_WAVE_EDGE_SAMPLES 48

typedef enum {
    RTTTL_WAVE_SQUARE = 0,   // 音效(经典 8bit 方波)
    RTTTL_WAVE_TRIANGLE,     // 背景音乐(谐波少,听感柔和)
    RTTTL_WAVE_TREMOLO,      // 三角波 + 振幅调制(颤音):加速道具的"冲刺"旋律(柔和)
} rtttl_wave_t;

// 一个音符的采样总长;尾部留白是刻意的 —— 让音与音之间有断句,不是连奏。
uint32_t rtttl_wave_total_samples(uint32_t duration_ms);

// 真正发声的长度(总长去掉尾部留白)。休止符为 0。
uint32_t rtttl_wave_sound_samples(uint16_t frequency, uint32_t duration_ms);

// 取第 index 个采样:frequency == 0(休止符)或 index >= sound_samples 时为静音。
// *phase 由调用方持有并跨采样保持连续。
//
// ⚠ 淡入淡出必须按【真正发声的长度】算。曾经按 total_samples 算,结果淡出窗口落进
// 尾部留白里,声音在满幅处被硬切 —— 每个音符边界都"啪"一声。
int16_t rtttl_wave_sample(uint16_t frequency, int16_t amplitude, rtttl_wave_t wave,
                          uint32_t index, uint32_t sound_samples, uint32_t *phase);

// 被打断时的淡出段(长度 RTTTL_WAVE_EDGE_SAMPLES):从当前相位平滑降到 0。
// 不淡出直接 return,同样会在满幅处截断出爆音(跳跃音效打断 BGM 就是这么响的)。
// index 是"本音符已经播到第几个采样",颤音要靠它接着算调制相位 —— 否则淡出段会
// 从固定的调制幅度起跳,又是一次阶跃。
// out 必须能放下 RTTTL_WAVE_EDGE_SAMPLES 个 int16_t。
void rtttl_wave_release(uint16_t frequency, int16_t amplitude, rtttl_wave_t wave,
                        uint32_t index, uint32_t *phase, int16_t *out);
