// main/chick_gradient.h —— 渐变的量化与抖动(纯计算,主机可测)。
//
// 问题:RGB565 每通道只有 5/6 位,天空/草坪这种长渐变会被量化成看得见的台阶。
//
// 做法:每行先算出理想颜色(1/256 档精度),再拆成"基准档位 + 抖动阈值":
//   * 阈值 = 该行的量化余量(占空比),填充时与蓝噪声掩码比较,决定这一像素要不要进位;
//   * 于是每行的平均值精确等于理想颜色 —— 相邻行的平均是连续变化的,不会出现"大条分层";
//   * 量化余量恰为 0 的行(理想颜色正好落在档位上)阈值就是 0,纯色区域完全不抖;
//   * 只有"整条渐变不足一个量化档"的通道才取最近档位(那时抖动只会更脏)。
// 注意端点不做特殊处理:曾经让最上/最下一行取最近档位,结果草坪顶部留下半档落差;
// 现在每一行(含首末行)都按余量抖动,行平均严格等于理想值。
//
// 早期两种做法都被实机否掉了,别再走回去:
//   1. 4x4 Bayer 全屏抖动 → 整齐的块状颗粒 + 每 4 行一条的横纹;
//   2. 只在台阶附近抖动、其余行全平 → 平坦区之间的 1 LSB 落差变成明显的大条分层。
// 抖动像素总数由量化误差决定,无法减少;能优化的只有分布 —— 蓝噪声是最优解。
#pragma once

#include <stdint.h>

// 一行的量化结果:基准色 + 三个通道的抖动阈值(0..1024,1024 = 恒进位)。
typedef struct {
    uint16_t base;     // RGB565,每通道取向下取整的档位
    uint16_t thr_r;    // 与 chick_dither_mask() 的返回值比较:rank < thr 则进位
    uint16_t thr_g;
    uint16_t thr_b;
} chick_gradient_row_t;

// 构造第 row 行(共 rows 行)的量化结果。top/bottom 是 0xRRGGBB。
//   * top == bottom 或整条渐变不足一个量化档时:该通道完全平坦(取最近档位);
//   * rows <= 1 时取 top。
// out 为 NULL 或 rows <= 0 时不做任何事。
void chick_gradient_row(uint32_t top_rgb888, uint32_t bottom_rgb888, int row, int rows,
                        chick_gradient_row_t *out);

// 把一行量化结果 + 蓝噪声 rank 还原成实际像素。
// rank 取自 chick_dither_mask(绝对屏幕坐标),超出范围时按端点夹取。
uint16_t chick_gradient_apply(const chick_gradient_row_t *row, uint16_t rank);
