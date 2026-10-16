// components/bsp/src/bsp_none.c
// S3R8 板(ESP32-S3R8)上"板上不存在的外设"的降级实现:I2C 总线。
//
// 为什么要有这个文件,而不是让应用去 #if 判板子:
//   应用只该关心"有没有这个能力",不该关心"哪块板子"。本板没有任何 I2C 从设备,
//   于是这里把同一个接口实现成"明确不可用",应用侧的降级逻辑(bsp_i2c_init()
//   不成功就跳过扫描)自然生效。
//
// 注意:电量不走 I2C —— 本板是 GPIO1 的 ADC 分压,实现在 bsp_battery_adc.c,
// 那个文件是真的能读到电量的,不要和这里的降级实现混在一起。
#include "bsp_i2c.h"
#include "esp_log.h"

static const char *TAG = "bsp_none";

esp_err_t bsp_i2c_init(void) {
    ESP_LOGI(TAG, "本板无 I2C 从设备(无 codec、无电量计芯片),跳过 I2C 总线初始化");
    return ESP_ERR_NOT_SUPPORTED;
}

i2c_master_bus_handle_t bsp_i2c_bus(void) { return NULL; }

esp_err_t bsp_i2c_scan(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t bsp_i2c_prepare_deep_sleep(void) { return ESP_OK; }
