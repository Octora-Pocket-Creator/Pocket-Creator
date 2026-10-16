// main/chick_run_store.c —— 最高分的 NVS 持久化。
//
// NVS 键值很小(一个 u32),每次破纪录才写一次,不需要额外缓存策略。
// 读失败/无记录一律返回 0:游戏照常能玩,只是没有历史成绩。
#include "chick_run_store.h"

#include <stdbool.h>

#include "chick_run_settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "chick_run";
static const char *NVS_NAMESPACE = "chick_run";
static const char *NVS_KEY_BEST = "best_m";
static const char *NVS_KEY_BRIGHTNESS = "bright";
static const char *NVS_KEY_VOLUME = "vol";
static const char *NVS_KEY_WARMTH = "warm";
static const char *NVS_KEY_MODE = "mode";
static const char *NVS_KEY_BEST_LEVEL = "best_lv";

static bool s_ready;

esp_err_t chick_run_store_init(void) {
    if (s_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区已满或是旧格式:擦除后重试一次(最高分丢失,但设备能正常工作)。
        ESP_LOGW(TAG, "NVS 需要重新初始化(%s),擦除后重试", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s —— 最高分不会保存", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间失败: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(handle);
    s_ready = true;
    return ESP_OK;
}

uint32_t chick_run_store_load_best(void) {
    if (!s_ready) return 0;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;

    uint32_t best = 0;
    esp_err_t err = nvs_get_u32(handle, NVS_KEY_BEST, &best);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "暂无历史最高分(%s)", esp_err_to_name(err));
        return 0;
    }
    return best;
}

esp_err_t chick_run_store_save_best(uint32_t best_m) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u32(handle, NVS_KEY_BEST, best_m);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "最高分写入失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t chick_run_store_load_settings(chick_run_settings_t *settings) {
    if (settings == NULL) return ESP_ERR_INVALID_ARG;
    chick_run_settings_defaults(settings);   // 先给可用值,缺项就保持默认
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;

    uint8_t brightness = settings->brightness;
    uint8_t volume = settings->volume;
    uint8_t warmth = settings->warmth;
    const esp_err_t brightness_err = nvs_get_u8(handle, NVS_KEY_BRIGHTNESS, &brightness);
    const esp_err_t volume_err = nvs_get_u8(handle, NVS_KEY_VOLUME, &volume);
    // 色温是后加的设置项:老设备上这个键不存在,保持默认值即可,不影响其它项。
    const esp_err_t warmth_err = nvs_get_u8(handle, NVS_KEY_WARMTH, &warmth);
    nvs_close(handle);

    if (brightness_err == ESP_OK) settings->brightness = brightness;
    if (volume_err == ESP_OK) settings->volume = volume;
    if (warmth_err == ESP_OK) settings->warmth = warmth;
    chick_run_settings_sanitize(settings);   // 旧版本/损坏值自愈
    return (brightness_err == ESP_OK && volume_err == ESP_OK) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t chick_run_store_save_settings(const chick_run_settings_t *settings) {
    if (settings == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, NVS_KEY_BRIGHTNESS, settings->brightness);
    if (err == ESP_OK) err = nvs_set_u8(handle, NVS_KEY_VOLUME, settings->volume);
    if (err == ESP_OK) err = nvs_set_u8(handle, NVS_KEY_WARMTH, settings->warmth);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置写入失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t chick_run_store_load_mode(uint8_t *mode) {
    if (mode == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;

    uint8_t stored = 0;
    const esp_err_t err = nvs_get_u8(handle, NVS_KEY_MODE, &stored);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    *mode = stored;
    return ESP_OK;
}

esp_err_t chick_run_store_save_mode(uint8_t mode) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, NVS_KEY_MODE, mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "玩法模式写入失败: %s", esp_err_to_name(err));
    return err;
}

uint32_t chick_run_store_load_best_level(void) {
    if (!s_ready) return 0;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;

    uint32_t level = 0;
    const esp_err_t err = nvs_get_u32(handle, NVS_KEY_BEST_LEVEL, &level);
    nvs_close(handle);
    if (err != ESP_OK) return 0;
    return level;
}

esp_err_t chick_run_store_save_best_level(uint32_t level) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, NVS_KEY_BEST_LEVEL, level);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "最高关写入失败: %s", esp_err_to_name(err));
    return err;
}
