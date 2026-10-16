// main/chick_run_store.h —— 最高分与设置的持久化(独立小模块,便于单独替换/测试)。
#pragma once

#include <stdint.h>

#include "chick_run_settings.h"
#include "esp_err.h"

// 准备 NVS 命名空间。可重复调用;失败时调用方可以继续跑,只是数据不落盘。
esp_err_t chick_run_store_init(void);

// 读取历史最高分(米)。没有记录或读取失败返回 0。
uint32_t chick_run_store_load_best(void);

// 写入最高分。只在数值提高时由调用方调用,避免频繁擦写 NVS。
esp_err_t chick_run_store_save_best(uint32_t best_m);

// 读取背光/音量设置。先把 settings 填成默认值,再用 NVS 里的记录覆盖,
// 缺项保持默认值 —— 所以调用方即使看到错误码也已经拿到可用配置。
esp_err_t chick_run_store_load_settings(chick_run_settings_t *settings);

// 写入设置。只在用户离开设置页时调用一次,不做逐次调整落盘。
esp_err_t chick_run_store_save_settings(const chick_run_settings_t *settings);

// 玩法模式(0 = 自由,1 = 关卡)。用 uint8_t 而不是模型枚举,避免 store 反向依赖模型;
// 取值合法性由调用方(UI)负责夹取。
esp_err_t chick_run_store_load_mode(uint8_t *mode);
esp_err_t chick_run_store_save_mode(uint8_t mode);

// 关卡模式的最高关。没有记录返回 0。
uint32_t chick_run_store_load_best_level(void);
esp_err_t chick_run_store_save_best_level(uint32_t level);
