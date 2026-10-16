// components/bsp/include/bsp_battery.h
// 电池电量接口。两块板两种实现,对上层是同一套契约:
//   * ESP32-S3R8 板:没有芯片 —— 电池电压经 200K/200K 分压进 GPIO1(ADC1_CH0),
//     固件做采样防抖 + 分压还原 + 电压→电量曲线(见 bsp_battery_adc.c /
//     bsp_battery_math.c)。所以本板上"电量"是估算值,精度受 ADC 与电池曲线限制。
// 共同的降级约定:拿不到数据时返回 -1 或初始化失败,界面据此不画电量槽。
#pragma once

#include "esp_err.h"

// 初始化。
//   创建 ADC unit / 通道 / 校准;本板固定接了 GPIO1 分压,校准失败会返回错误,
//   而不是拿未校准的原始值凑一个电压。
esp_err_t bsp_battery_init(void);

// 进入 deep sleep 前的收尾操作。本板没有待机寄存器,只重置滤波状态,直接返回成功。
// 这是终端操作;芯片未初始化时视为无需处理并返回成功。
esp_err_t bsp_battery_sleep(void);

// 剩余电量百分比 0..100;读失败返回 -1。
// S3R8 板:无数据时保持上次的读数(不让界面闪"--"),从未读到过才返回 -1。
int bsp_battery_soc(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);
