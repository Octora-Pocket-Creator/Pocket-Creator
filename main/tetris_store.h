// main/tetris_store.h —— 俄罗斯方块的最高分持久化。
//
// 只用 NVS 里的一个键:最高分。设置(亮度/音量/色温)不在这里 —— 那块数据与
// 小鸡快跑共用(chick_run_store.h),理由是同一台设备同一个用户,不该出现
// "两个应用各记一份亮度"的情况。
#pragma once

#include <stdint.h>

#include "esp_err.h"

// 准备 NVS 命名空间。可重复调用;失败时调用方仍可继续跑,只是成绩不落盘。
esp_err_t tetris_store_init(void);

// 历史最高分;没有记录或读取失败返回 0。
uint32_t tetris_store_load_best(void);

// 写入最高分。只在数值提高时调用,避免频繁擦写 NVS。
esp_err_t tetris_store_save_best(uint32_t score);
