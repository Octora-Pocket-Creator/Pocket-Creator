// components/bsp/src/bsp_battery_math.c
// 电池采样的纯计算实现:防抖 + 分压还原 + 电量曲线(无 ESP-IDF 依赖,主机可测)。
#include "bsp_battery_math.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   // qsort(去极值平均要用)

// 电压上限保护:单节锂电最多 4.2~4.3V,2 节串也就 8.4V。
// 超过 20V 只可能是分压参数填错或引脚接错,此时宁可返回 -1 让上层显示"--",
// 也不要把错值当电量百分比送到界面上。
#define VBAT_SANITY_MAX_MV 20000

// 单节锂电的"静置电压 → 剩余电量"近似曲线(从高到低,单位 mV / %)。
//
// 满电 4.1V、空电 3.3V 是本板的设计取值(用户确认满电 4.1V)。
// 这张表是最常见的锂电平台特征,不是精确模型:
//   * 3.9~3.7V 是平台段,电压变化很小而电量变化很大,所以中间点最密;
//   * 正在充电、或大电流放电时会明显偏(不做负载补偿);
//   * 换电池类型/低压保护值时改这张表即可,接口不用动。
static const struct {
    int mv;
    int soc;
} BATT_CURVE[] = {
    { 4100, 100 }, { 4050, 96 }, { 4000, 90 }, { 3950, 84 },
    { 3900, 76 },  { 3850, 68 }, { 3800, 60 }, { 3750, 52 },
    { 3700, 42 },  { 3650, 33 }, { 3600, 25 }, { 3550, 17 },
    { 3500, 11 },  { 3450, 6 },  { 3400, 2 },  { 3300, 0 },
};

static int compare_int(const void *left, const void *right) {
    const int a = *(const int *)left;
    const int b = *(const int *)right;
    return (a > b) - (a < b);
}

int bsp_battery_math_trimmed_mean(const int *samples, int count) {
    if (samples == NULL || count <= 0) return -1;
    if (count < 4) {
        // 样本太少就没法"去极值",直接平均。
        int64_t sum = 0;
        for (int i = 0; i < count; i++) sum += samples[i];
        return (int)(sum / count);
    }

    // 样本数很小(16 个),直接复制到栈上排序,不用堆分配。
    int sorted[64];
    if (count > (int)(sizeof(sorted) / sizeof(sorted[0]))) count = (int)(sizeof(sorted) / sizeof(sorted[0]));
    for (int i = 0; i < count; i++) sorted[i] = samples[i];
    qsort(sorted, (size_t)count, sizeof(sorted[0]), compare_int);

    // 丢掉最低/最高 quarter,只对中间一半取平均。
    const int drop = count / 4;
    int64_t sum = 0;
    int used = 0;
    for (int i = drop; i < count - drop; i++) {
        sum += sorted[i];
        used++;
    }
    if (used == 0) return -1;
    return (int)(sum / used);
}

int bsp_battery_math_iir(int previous, int sample, int shift) {
    if (shift <= 0 || shift > 16) return sample;
    if (previous < 0) return sample;   // 首帧:不要被"0"拖住
    const int32_t delta = sample - previous;
    if (delta == 0) return previous;
    // 四舍五入,而且小差值时至少走 1 步。
    // 为什么不能直接 delta >> shift:算术右移下 -1>>3 得 -1、+1>>3 得 0,
    // 平滑值越过目标后会长期停在目标另一侧(整数 IIR 的经典漂移)——
    // 比如从 2050 降到 1900,会停在 1893 再也回不来。
    int32_t step = (delta + (1 << (shift - 1))) >> shift;
    if (step == 0) step = delta > 0 ? 1 : -1;
    return (int)(previous + step);
}

int bsp_battery_math_soc_hysteresis(int previous_soc, int candidate_soc, int threshold) {
    if (candidate_soc < 0) return previous_soc;      // 读失败:保持上次的值
    if (previous_soc < 0) return candidate_soc;      // 首次上报:直接采用
    if (threshold <= 0) return candidate_soc;
    const int delta = candidate_soc - previous_soc;
    const int magnitude = delta < 0 ? -delta : delta;
    return magnitude >= threshold ? candidate_soc : previous_soc;
}

int bsp_battery_math_vbat_mv(int pin_mv, int r_top_ohm, int r_bottom_ohm, int scale_ppm) {
    if (pin_mv < 0 || r_top_ohm <= 0 || r_bottom_ohm <= 0 || scale_ppm <= 0) return -1;

    // Vbat = Vpin × (Rtop + Rbottom) / Rbottom
    // 先乘后除,并用 64 位中间量:分压电阻是几十~几百 kΩ,32 位乘法会溢出。
    int64_t vbat = (int64_t)pin_mv * (r_top_ohm + r_bottom_ohm) / r_bottom_ohm;
    vbat = vbat * scale_ppm / 1000000;
    if (vbat > VBAT_SANITY_MAX_MV) return -1;
    return (int)vbat;
}

int bsp_battery_math_soc_from_mv(int vbat_mv) {
    if (vbat_mv < 0) return -1;

    const size_t count = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);
    if (vbat_mv >= BATT_CURVE[0].mv) return 100;
    if (vbat_mv <= BATT_CURVE[count - 1].mv) return 0;

    for (size_t i = 0; i + 1 < count; i++) {
        const int mv_hi = BATT_CURVE[i].mv, mv_lo = BATT_CURVE[i + 1].mv;
        if (vbat_mv > mv_hi || vbat_mv <= mv_lo) continue;
        // 段内线性插值:表格足够密,不需要更复杂的拟合。
        // 低端向下取整是有意的:宁可早一点显示没电,不要在没电边缘还报百分比。
        const int soc_hi = BATT_CURVE[i].soc, soc_lo = BATT_CURVE[i + 1].soc;
        return soc_lo + (vbat_mv - mv_lo) * (soc_hi - soc_lo) / (mv_hi - mv_lo);
    }
    return 0;
}

bool bsp_battery_math_pin_mv_is_plausible(int pin_mv, int r_top_ohm, int r_bottom_ohm,
                                          int scale_ppm, int vbat_min_mv, int vbat_max_mv) {
    if (vbat_min_mv <= 0 || vbat_max_mv <= vbat_min_mv) return false;
    const int vbat = bsp_battery_math_vbat_mv(pin_mv, r_top_ohm, r_bottom_ohm, scale_ppm);
    if (vbat < 0) return false;
    return vbat >= vbat_min_mv && vbat <= vbat_max_mv;
}
