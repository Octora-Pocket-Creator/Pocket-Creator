// components/bsp/src/bsp_battery_adc.c
// S3R8 板的电量计:电池电压经 200K/200K 分压进 GPIO1(ADC1_CH0),固件换算 mV + SOC。
//
// 本实现的特点:
//   * 没有芯片,SOC 由固件按电压曲线估出来;
//   * 没有 I2C 事务,所以 init 不会因为芯片不应答而失败;
//   * 采样精度受 100kΩ 分压阻抗影响,必须做防抖(见 bsp_battery_math.c)。
//
// 一次读取的完整链路:
//   16 次 ADC 采样 → 去极值平均 → 一阶 IIR 平滑(跨次保留状态)
//     → 分压还原 + 标定 → 电压→电量曲线 → 百分比滞回
//
// 上层契约:bsp_battery_soc() / bsp_battery_mv() 读失败返回 -1,
// 界面据此不画数字;init 成功即表示"本板有可读的电池电压"。
#include "bsp_battery.h"
#include "bsp_battery_math.h"
#include "bsp_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "bsp_batt";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_ready;

// 滤波状态:跨次调用保留。放在文件作用域而不是每次读重建,
// 因为"防抖"的本质就是利用历史值 —— 每次从零开始等于没做。
static int s_pin_mv_filtered = -1;   // 引脚电压的平滑值(mV)
static int s_soc_reported = -1;      // 上次上报的百分比(滞回用)

// 采一次 ADC 引脚电压(mV):raw → 校准 → mV,失败返回 -1。
static int sample_pin_mv_once(void) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, BSP_BATT_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}

// 取一次"引脚电压":多次采样 → 合理性过滤 → 去极值平均 → IIR 平滑。
static int read_pin_mv(void) {
    int samples[BSP_BATT_SAMPLE_TIMES];
    int count = 0;
    for (int i = 0; i < BSP_BATT_SAMPLE_TIMES; i++) {
        const int mv = sample_pin_mv_once();
        if (mv < 0) continue;
        // 明显不可能的读数(耦合干扰)直接丢:滤波只能减弱它,不能消除它。
        if (!bsp_battery_math_pin_mv_is_plausible(mv, BSP_BATT_DIVIDER_R_TOP,
                                                 BSP_BATT_DIVIDER_R_BOTTOM,
                                                 BSP_BATT_MV_SCALE_PPM,
                                                 BSP_BATT_VBAT_MIN_MV, BSP_BATT_VBAT_MAX_MV)) {
            continue;
        }
        samples[count++] = mv;
    }
    if (count == 0) return -1;

    const int averaged = bsp_battery_math_trimmed_mean(samples, count);
    if (averaged < 0) return -1;

    s_pin_mv_filtered = bsp_battery_math_iir(s_pin_mv_filtered, averaged, BSP_BATT_IIR_SHIFT);
    return s_pin_mv_filtered;
}

esp_err_t bsp_battery_init(void) {
    if (s_ready) return ESP_OK;
    if (s_adc || s_cali) {
        ESP_LOGE(TAG, "上次电量计初始化回滚不完整,拒绝覆盖仍存活的 ADC 句柄");
        return ESP_ERR_INVALID_STATE;
    }

    const adc_oneshot_unit_init_cfg_t unit = { .unit_id = BSP_BATT_ADC_UNIT };
    esp_err_t e = adc_oneshot_new_unit(&unit, &s_adc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "电量 ADC unit 创建失败: %s", esp_err_to_name(e));
        s_adc = NULL;
        return e;
    }

    const adc_oneshot_chan_cfg_t channel = {
        .atten = BSP_BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    e = adc_oneshot_config_channel(s_adc, BSP_BATT_ADC_CHANNEL, &channel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "电量 ADC 通道配置失败: %s", esp_err_to_name(e));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return e;
    }

    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = BSP_BATT_ADC_UNIT,
        .chan = BSP_BATT_ADC_CHANNEL,
        .atten = BSP_BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    e = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (e != ESP_OK) {
        // 没有校准就只能拿原始码值猜电压,误差大到让电量数字没意义 ——
        // 与其显示一个错的百分比,不如让电量不可用(界面会隐藏电量槽)。
        ESP_LOGE(TAG, "电量 ADC 校准创建失败: %s", esp_err_to_name(e));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return e;
    }

    s_ready = true;
    ESP_LOGI(TAG, "电量就绪:ADC1_CH%d(GPIO1) 分压 %dΩ/%dΩ, 采样 %d 次去极值+IIR>>%d, 标定 %d ppm",
             BSP_BATT_ADC_CHANNEL, BSP_BATT_DIVIDER_R_TOP, BSP_BATT_DIVIDER_R_BOTTOM,
             BSP_BATT_SAMPLE_TIMES, BSP_BATT_IIR_SHIFT, BSP_BATT_MV_SCALE_PPM);
    return ESP_OK;
}

int bsp_battery_mv(void) {
    if (!s_ready) return -1;
    const int pin_mv = read_pin_mv();
    if (pin_mv < 0) return -1;
    return bsp_battery_math_vbat_mv(pin_mv, BSP_BATT_DIVIDER_R_TOP,
                                    BSP_BATT_DIVIDER_R_BOTTOM, BSP_BATT_MV_SCALE_PPM);
}

int bsp_battery_soc(void) {
    const int mv = bsp_battery_mv();
    if (mv < 0) {
        // 一次性读失败不要清掉上次的显示值:界面会闪一下"--"再跳回来。
        // 真正的"没有电池"由 bsp_battery_init() 的返回值表达。
        return s_soc_reported;
    }
    s_soc_reported = bsp_battery_math_soc_hysteresis(
        s_soc_reported, bsp_battery_math_soc_from_mv(mv), BSP_BATT_SOC_HYSTERESIS);
    return s_soc_reported;
}

esp_err_t bsp_battery_sleep(void) {
    // ADC 没有待机寄存器:不需要在 deep sleep 前写任何东西。
    // 保留这个接口是为了让上层始终按同一套调用序列使用电池能力。
    s_pin_mv_filtered = -1;   // 休眠唤醒后环境已变,重新起滤波
    return ESP_OK;
}
