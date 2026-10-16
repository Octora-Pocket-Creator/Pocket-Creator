// components/bsp/include/bsp_battery_math.h
// 电池采样的纯计算部分:采样防抖 → 分压还原 → 电压转电量。全部不依赖 ESP-IDF。
//
// 为什么把这三步单独拆出来测试:
//   * 分压系数写反、曲线端点写反、插值方向写反,在设备上只表现为"电量数字有点怪",
//     在屏幕上根本定位不到;
//   * 防抖参数的边界(阈值刚好相等、首帧没有历史值)也只有在主机上才好钉死。
// ADC 采集本身(unit/通道/校准/换算成引脚 mV)在 bsp_battery_adc.c。
//
// 数据流(每次读电量走一遍):
//   16 次 ADC 原始值 → 合理性过滤(丢掉不可能的读数)
//     → bsp_battery_math_trimmed_mean() 去极值求均值(抗尖峰)
//     → bsp_battery_math_iir() 一阶平滑(抗抖动/负载跳变,保留历史)
//     → bsp_battery_math_vbat_mv() 按分压还原电池电压 + 标定系数
//     → bsp_battery_math_soc_from_mv() 查曲线得百分比
//     → bsp_battery_math_soc_hysteresis() 小变化不更新(防止图标反复跳数)
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- 防抖 ------------------------------------------------------------------

// 去极值平均(trimmed mean):先排序,丢掉最高/最低 quarter,再对中间取平均。
// 用途:ADC 偶发的单个尖峰(比如背光 PWM 切换瞬间耦合进来的一次读数)不能
// 直接把电量拉掉 20%。count < 4 时退化为普通平均。
// samples 为 NULL 或 count <= 0 返回 -1。
int bsp_battery_math_trimmed_mean(const int *samples, int count);

// 一阶 IIR 平滑:result = previous + (sample - previous) / 2^shift。
// shift 越大越平稳、响应越慢(板级参数 BSP_BATT_IIR_SHIFT,默认 3 ≈ 1/8 权重)。
// previous < 0 表示"还没有历史值",此时直接采用 sample(首帧不被 0 拖住)。
// shift 非法(<=0 或 >16)时退化为直接采用 sample。
int bsp_battery_math_iir(int previous, int sample, int shift);

// 电量百分比滞回:候选值与上次上报值相差不到 threshold 就保持上次的值。
// 用途:电压在段边界抖动时,百分比会在 76/77 之间反复跳,图标跟着闪。
// 变化幅度达到 threshold(绝对值)才更新。
// previous < 0 表示首次上报,直接采用候选值。
int bsp_battery_math_soc_hysteresis(int previous_soc, int candidate_soc, int threshold);

// --- 换算 ------------------------------------------------------------------

// 把 ADC 引脚上的电压还原成电池电压(mV)。
//   r_top_ohm / r_bottom_ohm:VBAT -- Rtop -- 引脚 -- Rbottom -- GND
//   scale_ppm:标定系数,1000000 = 不修正。用万用表实测电池电压后按比例调它:
//              读数偏低 2.5% 就填 1025000(按 实测/读数 的比值换算)。
// 参数非法返回 -1;结果做上限保护(>20V 视为接线或配置错误,返回 -1)。
int bsp_battery_math_vbat_mv(int pin_mv, int r_top_ohm, int r_bottom_ohm, int scale_ppm);

// 电池电压 → 剩余电量百分比 0..100。曲线端点见 .c 里的表。
// 参数非法(<0)返回 -1;空电以下 0,满电以上 100。
int bsp_battery_math_soc_from_mv(int vbat_mv);

// 该引脚电压是否落在"电池电压合理区间"内。
// 用途:ADC 偶尔会读到被耦合进来的高电平(比如背光 PWM 边沿),
// 单看引脚可能只高几百 mV,换算成电池电压就是 4.3V+ 的不可能值。
// 这类采样必须【丢掉】而不是交给滤波 —— 滤波只能减弱它,不能消除它。
// vbat_min_mv/vbat_max_mv 来自板级参数;参数非法时返回 false(宁可不采)。
bool bsp_battery_math_pin_mv_is_plausible(int pin_mv, int r_top_ohm, int r_bottom_ohm,
                                          int scale_ppm, int vbat_min_mv, int vbat_max_mv);

#ifdef __cplusplus
}
#endif
