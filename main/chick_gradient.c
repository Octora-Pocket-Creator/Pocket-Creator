// main/chick_gradient.c —— 渐变量化 + 蓝噪声抖动(无 ESP-IDF 依赖,主机可测)。
//
// 设计取舍见 chick_gradient.h 的说明:抖动像素总数固定,只优化分布 ——
// 每行的占空比精确等于该行的量化余量,所以行平均严格执行理想渐变(不会有"大条");
// 具体哪些像素进位由 32x32 蓝噪声掩码决定(不会有整齐的网格/条纹)。
#include "chick_gradient.h"

#include <stddef.h>

#include "chick_dither.h"

// 量化余量用 1/256 档的定点表示,免得缓渐变被整数除法抹成台阶。
#define FRAC_FULL   256
// 阈值满量程 = 掩码级数:rank 最大 CHICK_DITHER_LEVELS-1,所以用 1024 表示"恒进位"。
#define LEVELS_FULL ((int32_t)CHICK_DITHER_LEVELS)

static int32_t abs32(int32_t value) { return value < 0 ? -value : value; }

// 单个通道:把 [top, bottom] 在 rows 行上的理想值转成"基准档位 + 抖动阈值"。
static void build_channel(uint8_t top, uint8_t bottom, int row, int rows, int step_bits,
                          uint16_t *level_out, uint16_t *threshold_out) {
    const int32_t step = 1 << step_bits;      // 5 位通道步长 8,6 位通道步长 4
    const int32_t last_row = rows > 1 ? rows - 1 : 1;

    // 理想值:单位是 1/256 个量化档。
    const int32_t span8 = (int32_t)bottom - (int32_t)top;
    const int32_t ideal_q8 = (int32_t)top * FRAC_FULL + span8 * FRAC_FULL * row / last_row;
    const int32_t units_q8 = ideal_q8 / step;
    const int32_t level = units_q8 / FRAC_FULL;
    const int32_t frac = units_q8 - level * FRAC_FULL;      // 0..255

    // 整条渐变走过的档数(1/256 档)与每行走过的量。
    const int32_t span_q8 = abs32(span8) * FRAC_FULL / step;
    const int32_t per_row_q8 = span_q8 / last_row;

    int32_t threshold;
    if (rows <= 1 || span_q8 < FRAC_FULL || per_row_q8 == 0) {
        // 整条渐变不足一个量化档(或缓到每行变化量取整为 0):取最近档位、完全平坦。
        // 这种情况按余量抖的话,每一行都会撒一层稀疏麻点(整幅看着更脏);
        // 而取最近档位最多产生 1 个量化档的落差,远低于可见阈值。
        threshold = frac >= FRAC_FULL / 2 ? LEVELS_FULL : 0;
    } else {
        // 占空比精确等于量化余量:行平均严格执行理想渐变(这是"不出现大条分区"的关键)。
        threshold = frac * LEVELS_FULL / FRAC_FULL;
    }

    int32_t value = level * step;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    *level_out = (uint16_t)value;
    if (threshold < 0) threshold = 0;
    if (threshold > LEVELS_FULL) threshold = LEVELS_FULL;
    *threshold_out = (uint16_t)threshold;
}

void chick_gradient_row(uint32_t top_rgb888, uint32_t bottom_rgb888, int row, int rows,
                        chick_gradient_row_t *out) {
    if (out == NULL || rows <= 0) return;
    if (row < 0) row = 0;
    if (row >= rows) row = rows - 1;

    uint16_t r = 0, g = 0, b = 0, tr = 0, tg = 0, tb = 0;
    build_channel((uint8_t)((top_rgb888 >> 16) & 0xFF), (uint8_t)((bottom_rgb888 >> 16) & 0xFF),
                  row, rows, 3, &r, &tr);
    build_channel((uint8_t)((top_rgb888 >> 8) & 0xFF), (uint8_t)((bottom_rgb888 >> 8) & 0xFF),
                  row, rows, 2, &g, &tg);
    build_channel((uint8_t)(top_rgb888 & 0xFF), (uint8_t)(bottom_rgb888 & 0xFF),
                  row, rows, 3, &b, &tb);

    out->base = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    out->thr_r = tr;
    out->thr_g = tg;
    out->thr_b = tb;
}

uint16_t chick_gradient_apply(const chick_gradient_row_t *row, uint16_t rank) {
    if (row == NULL) return 0;

    uint16_t px = row->base;
    // 三个通道共用同一个 rank:进位总是发生在同样的像素上,所以抖出来的点是中性灰,
    // 不会出现彩色噪点(彩色噪点比亮度噪点显眼得多)。
    // 每个通道都要先判是否已到顶:到顶还加分就会进位到相邻通道(白色里会冒出黑点)。
    if (rank < row->thr_r && ((px >> 11) & 0x1F) < 0x1F) px = (uint16_t)(px + (1u << 11));
    if (rank < row->thr_g && ((px >> 5) & 0x3F) < 0x3F) px = (uint16_t)(px + (1u << 5));
    if (rank < row->thr_b && (px & 0x1F) < 0x1F) px = (uint16_t)(px + 1u);
    return px;
}
