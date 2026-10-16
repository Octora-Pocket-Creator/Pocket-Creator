// main/chick_color.h —— 屏幕色温补偿(逐通道增益),纯计算、可在主机上测。
//
// 为什么需要它:面板+背光 LED 的白点偏冷(偏蓝),这是硬件决定的,改不动;
// 但 RGB565 是每像素的整数数据,在【推屏前】对三个通道各乘一个系数,
// 就能把白点拉回来。乘性增益的好处是黑色仍然是黑色(0 × k = 0),
// 所以背景不会发灰,只会整体色温变化。
//
// 为什么放在推屏路径而不是画布:画布是"真值",色温只影响最终显示;
// 而且推屏是全屏唯一的汇聚点(脏区渲染每帧只推 1~2k 像素),代价可以忽略。
//
// 实现用查表:RGB565 每个通道只有 5/6/5 位(32/64/32 档),把"增益 → 目标值"
// 预先算成三张小表(共 128 个 uint16),每像素只做三次查表 + 拼装。
#pragma once

#include <stdint.h>

// 暖度取值 0..100:0 = 原色不处理,100 = 片子能达到的最暖。
#define CHICK_COLOR_WARMTH_MIN 0
#define CHICK_COLOR_WARMTH_MAX 100

// 三个通道的增益,定点 Q8(256 = 1.0)。
typedef struct {
    uint16_t r;
    uint16_t g;
    uint16_t b;
} chick_color_gain_t;

// 增益查表:索引就是原始通道值,内容是该通道补偿后的通道值。
typedef struct {
    uint16_t r5[32];
    uint16_t g6[64];
    uint16_t b5[32];
} chick_color_lut_t;

// 暖度百分比 → 三通道增益。0 恒等于 (256,256,256)(严格不改变画面);
// 100 为最暖预设(只抬红、压蓝,绿色保持不动,避免整体发黄绿)。
// 超出 [0,100] 按端点夹取。
chick_color_gain_t chick_color_gain_from_warmth(uint8_t warmth);

// 由增益生成查表。增益为 (256,256,256) 时表格是恒等映射(可用测试钉死)。
void chick_color_lut_build(const chick_color_gain_t *gain, chick_color_lut_t *lut);

// 直接按增益处理一个 RGB565 像素(不查表,给测试和小批量用)。
uint16_t chick_color_apply_gain(uint16_t px, const chick_color_gain_t *gain);

// 按查表处理一个 RGB565 像素(推屏路径用这个)。
uint16_t chick_color_apply_lut(uint16_t px, const chick_color_lut_t *lut);
