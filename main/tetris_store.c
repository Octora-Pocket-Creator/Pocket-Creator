// main/tetris_store.c —— 最高分的 NVS 持久化(独立命名空间 "tetris")。
#include "tetris_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "tetris_store";
static const char *NVS_NAMESPACE = "tetris";
static const char *NVS_KEY_BEST = "best";

esp_err_t tetris_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区写满或版本不兼容:擦掉重建。记录的是最高分,丢了可以再打。
        ESP_LOGW(TAG, "NVS 需要重建(%s),擦除后重试", esp_err_to_name(err));
        if (nvs_flash_erase() != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

uint32_t tetris_store_load_best(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    uint32_t best = 0;
    const esp_err_t err = nvs_get_u32(handle, NVS_KEY_BEST, &best);
    nvs_close(handle);
    if (err != ESP_OK) return 0;
    // 异常大的值(旧版本/损坏)直接丢弃,免得结算页显示一个假纪录。
    if (best > 100000000u) return 0;
    return best;
}

esp_err_t tetris_store_save_best(uint32_t score) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, NVS_KEY_BEST, score);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "保存最高分失败:%s", esp_err_to_name(err));
    return err;
}
