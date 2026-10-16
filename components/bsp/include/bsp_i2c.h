// components/bsp/include/bsp_i2c.h
// 共享 I2C 总线接口。本板没有 I2C 从设备，该接口在板上返回“不支持”(见 bsp_none.c)，
// 应用据此降级；带 I2C 设备的派生工程由本模块持有总线句柄，避免“谁先初始化谁建总线”。
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

// 初始化共享总线。幂等:重复调用直接返回 ESP_OK,可在每个驱动的 init 里放心调。
esp_err_t bsp_i2c_init(void);

// 取共享总线句柄。未初始化时返回 NULL。
i2c_master_bus_handle_t bsp_i2c_bus(void);

// 扫描 0x08..0x77 并打印所有应答的设备，排查“芯片是不是没焊好/地址对不对”。
// 直接在正式总线上扫，不要另开临时总线。本板返回“不支持”。
esp_err_t bsp_i2c_scan(void);

// deep sleep 专用：所有共享外设的最后一笔 I2C 事务完成后，将 SDA/SCL
// 设为无上下拉的高阻输入。调用后禁止再访问总线，必须立即进入 deep sleep
// 或重启。板上外部上拉电阻的静态功耗不受此接口控制。
esp_err_t bsp_i2c_prepare_deep_sleep(void);
