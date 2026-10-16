// main/main.c —— 《番茄闹钟》(Pocket Pomodoro)应用入口。
//
// 分层与跑酷/方向快手一致,内容换成了离线计时工具:
//   pocket_pomodoro_model.c  25/5 循环、暂停/重置、调时长(纯 C,主机可测)
//   pocket_pomodoro_ui.c     画面、按键队列、推屏
//   本文件                   只做板级初始化与按键事件入队
//
// 板级差异全部在 BSP 里(S3R8 六键 GPIO 已被 BSP 归一成
// UP/DOWN/A/B/LEFT/RIGHT 六个语义键),这里不做任何目标相关的分支。
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pocket_pomodoro_ui.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TASK_STACK  3072

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;

// 按键回调跑在 button 组件的共享 esp_timer 任务里:只入队,立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || s_input_queue == NULL) return;
    const input_event_t event = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &event, 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t event;
    for (;;) {
        if (xQueueReceive(s_input_queue, &event, portMAX_DELAY) == pdTRUE) {
            pocket_pomodoro_ui_key(event.btn, event.event);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "pomodoro_input", INPUT_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "PockiGo —— 番茄闹钟 build=%s", POCKET_POMODORO_BUILD_TAG);

    // I2C 是可选能力:S3R8 板没有 I2C 从设备,bsp_i2c_init() 返回"不支持"。
    // 只记不报错 —— 没有 I2C 不是故障。
    if (bsp_i2c_init() == ESP_OK) {
        bsp_i2c_scan();
    }

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_RST, BSP_LCD_BL);
        return;
    }
    // 这里不开背光:面板已上电但屏上还不是我们的画面。开屏交给渲染任务,
    // 它推完首帧才点亮,用户看到的是"亮屏即标题页"。

    if (input_dispatch_init() != ESP_OK) {
        ESP_LOGE(TAG, "输入任务创建失败,游戏无法操作");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,游戏无法操作");
        return;
    }
    s_input_ready = true;

    if (pocket_pomodoro_ui_start() != ESP_OK) {
        ESP_LOGE(TAG, "界面启动失败");
        return;
    }
    ESP_LOGI(TAG, "就绪:A 开始/暂停 B 重置;上 下 调时长 左 右 切阶段");
}
