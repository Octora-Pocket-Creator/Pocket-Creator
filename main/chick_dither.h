// main/chick_dither.h —— 蓝噪声抖动掩码(查表)。
//
// 为什么需要它:RGB565 每通道只有 5/6 位,天空/草坪这种长渐变必须在"看起来是硬边"
// 和"看起来是颗粒"之间做取舍。抖动像素的总数是固定的(由量化误差决定,与算法无关),
// 能优化的只有【这些像素怎么分布】:
//   * 有规则的矩阵(4x4 / 8x8 Bayer)会把阈值排成整齐网格 —— 实机上表现为
//     一块一块的颗粒,以及纵向周期带来的横向条纹;
//   * 把抖动限制在"台阶附近的窄带"里,平坦区之间的 1 LSB 落差又会变成肉眼可见的
//     大条分层(实机验证过,这条路是错的);
//   * 蓝噪声把阈值在空间上打散:低频能量低、高频能量高,于是同样的量化误差看起来
//     只是细密的颗粒,眼睛在正常观看距离上会把它积分掉。
//
// 掩码是生成物(见 tools/generate-dither-mask.py,void-and-cluster 算法):
// 32x32 个格子,每个格子一个 0..1023 的唯一 rank。判断"这个像素要不要进位"就是
// 比较 rank 与阈值,因此同一像素永远得到同一结果 —— 画面可重复,不会每帧闪。
#pragma once

#include <stdint.h>

// 掩码边长与总级数(阈值有效范围 0..CHICK_DITHER_LEVELS)。
#define CHICK_DITHER_SIZE   32
#define CHICK_DITHER_LEVELS (CHICK_DITHER_SIZE * CHICK_DITHER_SIZE)

// 取 (x, y) 处的 rank(0..1023)。负坐标按绝对值取模,越界坐标自动环绕。
uint16_t chick_dither_mask(int x, int y);
