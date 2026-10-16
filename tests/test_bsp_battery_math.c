// tests/test_bsp_battery_math.c —— 电池换算与防抖的主机测试。
//
// 为什么要单独测这些:分压系数算反(乘 Rbottom 而不是 Rtop+Rbottom)、曲线端点写反、
// 插值方向写反、IIR 权重写反,在设备上都只表现为"电量数字有点怪",靠肉眼看屏幕
// 根本定位不到;防抖的边界(首帧没有历史值、阈值刚好相等、单个尖峰)更是如此。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp_battery_math.h"

// S3R8 板:VBAT -- 200K -- GPIO1 -- 200K -- GND
#define R_TOP    200000
#define R_BOTTOM 200000

static void test_divider(void) {
    // 对半分压:引脚电压 ×2 才是电池电压。
    assert(bsp_battery_math_vbat_mv(2050, R_TOP, R_BOTTOM, 1000000) == 4100);
    assert(bsp_battery_math_vbat_mv(1650, R_TOP, R_BOTTOM, 1000000) == 3300);
    assert(bsp_battery_math_vbat_mv(0, R_TOP, R_BOTTOM, 1000000) == 0);

    // 非对称分压:100K 上 / 200K 下 → 引脚是 VBAT 的 2/3,所以 ×1.5。
    assert(bsp_battery_math_vbat_mv(2733, 100000, 200000, 1000000) == 4099);
    // 200K 上 / 100K 下 → 引脚是 VBAT 的 1/3,所以 ×3。
    assert(bsp_battery_math_vbat_mv(1366, 200000, 100000, 1000000) == 4098);

    // 标定系数:读数偏低 2.5% 时用 1025000 修正(4100 × 1.025 = 4202.5 → 4202)。
    assert(bsp_battery_math_vbat_mv(2050, R_TOP, R_BOTTOM, 1025000) == 4202);

    // 大电阻不溢出:1M/1M 分压、引脚 3V → 6V 电池。
    assert(bsp_battery_math_vbat_mv(3000, 1000000, 1000000, 1000000) == 6000);

    // 接线/配置明显出错时不要编一个荒唐值出来。
    assert(bsp_battery_math_vbat_mv(15000, 100000, 100000, 1000000) == -1);

    // 非法参数:负数、0 电阻、0 系数。
    assert(bsp_battery_math_vbat_mv(-1, R_TOP, R_BOTTOM, 1000000) == -1);
    assert(bsp_battery_math_vbat_mv(2000, 0, R_BOTTOM, 1000000) == -1);
    assert(bsp_battery_math_vbat_mv(2000, R_TOP, 0, 1000000) == -1);
    assert(bsp_battery_math_vbat_mv(2000, -R_TOP, R_BOTTOM, 1000000) == -1);
    assert(bsp_battery_math_vbat_mv(2000, R_TOP, R_BOTTOM, 0) == -1);
}

static void test_soc_endpoints(void) {
    // 满电 4.1V(用户确认)、空电 3.3V 是本板的设计取值。
    assert(bsp_battery_math_soc_from_mv(4100) == 100);
    assert(bsp_battery_math_soc_from_mv(4300) == 100);   // 高于满电:夹到 100
    assert(bsp_battery_math_soc_from_mv(3300) == 0);
    assert(bsp_battery_math_soc_from_mv(0) == 0);
    assert(bsp_battery_math_soc_from_mv(-1) == -1);      // 无数据仍然是 -1,不能是 0%
}

static void test_soc_table_points(void) {
    // 表格里的点必须原样命中,不能被插值改掉。
    assert(bsp_battery_math_soc_from_mv(4050) == 96);
    assert(bsp_battery_math_soc_from_mv(3900) == 76);
    assert(bsp_battery_math_soc_from_mv(3700) == 42);
    assert(bsp_battery_math_soc_from_mv(3500) == 11);
    assert(bsp_battery_math_soc_from_mv(3400) == 2);
    // 段内线性插值:4025 在 4050(96) 与 4000(90) 之间 → 93。
    assert(bsp_battery_math_soc_from_mv(4025) == 93);
    // 3.325V 在 3400(2) 与 3300(0) 之间:插值 0.5% 向下取整为 0。
    // 低端向下取整是有意的:宁可早一点显示没电,不要在没电边缘还报百分比。
    assert(bsp_battery_math_soc_from_mv(3325) == 0);
    assert(bsp_battery_math_soc_from_mv(3350) == 1);
}

static void test_soc_is_monotonic_and_bounded(void) {
    // 电压升高 → 电量只能升不能降(插值/段落拼接写错时这里会立刻炸)。
    int previous = -1;
    for (int mv = 3200; mv <= 4300; mv++) {
        const int soc = bsp_battery_math_soc_from_mv(mv);
        assert(soc >= 0 && soc <= 100);
        assert(soc >= previous);
        previous = soc;
    }
    assert(previous == 100);
}

static void test_trimmed_mean_rejects_spikes(void) {
    // 正常读数在 2040~2060 之间,夹一个 3000 的尖峰(比如 PWM 耦合进来的一次)。
    const int with_spike[] = { 2050, 2045, 3000, 2055, 2050, 2040, 2055, 2045 };
    const int mean = bsp_battery_math_trimmed_mean(with_spike, 8);
    assert(mean >= 2040 && mean <= 2060);        // 尖峰必须被丢掉

    // 不用去极值时,一个尖峰能把均值拉高十几 mV;这里对比说明差异确实存在。
    int64_t plain = 0;
    for (int i = 0; i < 8; i++) plain += with_spike[i];
    assert((int)(plain / 8) > mean);

    // 全部相同 → 结果就是那个值。
    const int flat[] = { 2000, 2000, 2000, 2000 };
    assert(bsp_battery_math_trimmed_mean(flat, 4) == 2000);

    // 样本太少(<4)退化为普通平均,而不是直接失败。
    const int two[] = { 1000, 2000 };
    assert(bsp_battery_math_trimmed_mean(two, 2) == 1500);

    // 负数/空指针。
    assert(bsp_battery_math_trimmed_mean(NULL, 4) == -1);
    assert(bsp_battery_math_trimmed_mean(flat, 0) == -1);
    assert(bsp_battery_math_trimmed_mean(flat, -3) == -1);
}

static void test_iir_smoothing(void) {
    // 首帧:没有历史值,直接采用本次采样(不能被 0 拖住)。
    assert(bsp_battery_math_iir(-1, 2050, 3) == 2050);

    // shift=3 → 每次采纳 1/8 的新值,并四舍五入。
    assert(bsp_battery_math_iir(2000, 2080, 3) == 2010);   // 80/8 = 10
    assert(bsp_battery_math_iir(2010, 2080, 3) == 2019);   // 70/8 = 8.75 → 9

    // 单个尖峰推不动平滑值(这才是防抖要的效果)。
    int value = 2050;
    value = bsp_battery_math_iir(value, 3000, 3);
    assert(value - 2050 <= 120);                 // 3000 的尖峰只带来 ~119mV

    // 连续朝着新值走,最终会收敛过去。
    for (int i = 0; i < 400; i++) value = bsp_battery_math_iir(value, 2100, 3);
    assert(value == 2100);

    // 下降方向同样成立(放电时会一路往下),而且不会越过目标停在另一侧。
    int down = 2050;
    for (int i = 0; i < 50; i++) down = bsp_battery_math_iir(down, 1900, 3);
    assert(down == 1900);
    // 已经越过目标(低于目标)时还能拉回来,而不是钉在下方。
    assert(bsp_battery_math_iir(1893, 1900, 3) == 1894);
    assert(bsp_battery_math_iir(1899, 1900, 3) == 1900);

    // shift 非法(<=0 或过大):退化为直接采用新值,而不是算出垃圾。
    assert(bsp_battery_math_iir(2000, 2100, 0) == 2100);
    assert(bsp_battery_math_iir(2000, 2100, 99) == 2100);
}

static void test_soc_hysteresis(void) {
    // 首次上报:直接采用。
    assert(bsp_battery_math_soc_hysteresis(-1, 76, 2) == 76);
    // 变化没到阈值 → 保持上次的值(图标不跳)。
    assert(bsp_battery_math_soc_hysteresis(76, 77, 2) == 76);
    assert(bsp_battery_math_soc_hysteresis(76, 75, 2) == 76);
    // 达到阈值 → 更新。
    assert(bsp_battery_math_soc_hysteresis(76, 78, 2) == 78);
    assert(bsp_battery_math_soc_hysteresis(76, 74, 2) == 74);
    // 读失败(-1)→ 保持上次的值,不要把"读不到"显示成 0%。
    assert(bsp_battery_math_soc_hysteresis(76, -1, 2) == 76);
    // threshold<=0 → 不做滞回。
    assert(bsp_battery_math_soc_hysteresis(76, 77, 0) == 77);
}

// 端到端:模拟一次真实的 10s 轮询序列。
// 链路与驱动一致:合理性过滤 → IIR 平滑 → 分压还原 → 曲线 → 滞回。
static void test_end_to_end_sequence(void) {
    int pin_filtered = -1;
    int reported = -1;
    // 4.1V 满电起步、缓慢下降;第 3、7 轮各插一个"被耦合干扰抬高的"读数(2.6V/3.0V 引脚),
    // 它们换算成电池电压分别是 5.2V / 6.0V,属于不可能值 → 必须被过滤掉。
    const int pin_series[] = { 2050, 2048, 2600, 2043, 2040, 2038, 2040, 3000, 2033, 2030 };
    for (int i = 0; i < (int)(sizeof(pin_series) / sizeof(pin_series[0])); i++) {
        const int pin = pin_series[i];
        if (!bsp_battery_math_pin_mv_is_plausible(pin, R_TOP, R_BOTTOM, 1000000, 2500, 4400)) {
            continue;   // 驱动里就是直接丢掉
        }
        pin_filtered = bsp_battery_math_iir(pin_filtered, pin, 3);
        const int vbat = bsp_battery_math_vbat_mv(pin_filtered, R_TOP, R_BOTTOM, 1000000);
        assert(vbat >= 2500 && vbat <= 4400);
        reported = bsp_battery_math_soc_hysteresis(
            reported, bsp_battery_math_soc_from_mv(vbat), 2);
        assert(reported >= 0 && reported <= 100);
    }
    // 采样只降了 10mV 引脚电压(≈20mV 电池),百分比不应该掉格。
    assert(reported >= 95);
}

static void test_plausibility_gate(void) {
    // 200K/200K、合理区间 2500~4400mV → 引脚 1250~2200mV。
    assert(bsp_battery_math_pin_mv_is_plausible(2050, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    assert(bsp_battery_math_pin_mv_is_plausible(1250, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    assert(bsp_battery_math_pin_mv_is_plausible(2200, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    // 恰好超出边界:丢掉。
    assert(!bsp_battery_math_pin_mv_is_plausible(1249, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    assert(!bsp_battery_math_pin_mv_is_plausible(2201, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    // 干扰毛刺。
    assert(!bsp_battery_math_pin_mv_is_plausible(3000, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    assert(!bsp_battery_math_pin_mv_is_plausible(0, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    // 读失败。
    assert(!bsp_battery_math_pin_mv_is_plausible(-1, R_TOP, R_BOTTOM, 1000000, 2500, 4400));
    // 区间参数写错时宁可不采。
    assert(!bsp_battery_math_pin_mv_is_plausible(2050, R_TOP, R_BOTTOM, 1000000, 0, 4400));
    assert(!bsp_battery_math_pin_mv_is_plausible(2050, R_TOP, R_BOTTOM, 1000000, 4400, 2500));
}

int main(void) {
    test_divider();
    test_soc_endpoints();
    test_soc_table_points();
    test_soc_is_monotonic_and_bounded();
    test_trimmed_mean_rejects_spikes();
    test_iir_smoothing();
    test_soc_hysteresis();
    test_plausibility_gate();
    test_end_to_end_sequence();
    printf("bsp battery math tests: PASS\n");
    return 0;
}
