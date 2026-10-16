// main/main.c —— 应用入口(设备上唯一的应用:俄罗斯方块)。
//
// 板级差异全部在 BSP 里,这里只做四件事:
//   1. 交给 BSP 初始化 I2C(有才初始化)与显示;
//   2. 按键事件只做入队,由应用自己的渲染任务消费(按键回调不能阻塞);
//   3. 屏幕/按键拿不到就直接报错退出;
//   4. 电量读不到只是不显示数字。
//
// 没有游戏菜单:开机直接进俄罗斯方块首页。同一棵树里的小鸡快跑
// (chick_run_ui.c)与两个参考应用(番茄闹钟、方向快手)保留源码但不参与构建,
// 它们共用字形表、设置存储(亮度/音量/色温)与 UI 绘制原语。
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "tetris_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 冒烟调试用:一眼看出设备上跑的是哪一版固件(串口第一行)。
#define APP_BUILD_TAG "1.2.0-s3"

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TASK_STACK  3072

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

// 按键回调运行在 button 组件的共享 esp_timer 任务里:只入队,立刻返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || s_input_queue == NULL) return;
    const input_event_t event = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &event, 0);
}

#ifdef APP_KEY_SELFTEST
// 开发自检(默认关):按时间表把按键塞进同一条输入队列,与真实按键走完全相同的
// 代码路径。目的是在没有人在场的情况下让设备把整局游戏打完:移动/旋转/硬降 ->
// 堆到顶消行(听消行音效、看消行闪烁)-> 顶到天花板结束(游戏结束音效)-> 再开一局。
// 只有"按住生效"的状态(左右自动重复、下键加速)才注入 PRESS/RELEASE。
static void key_selftest_step(uint32_t now) {
    static const struct {
        uint32_t at_ms;
        bsp_btn_t btn;
        bsp_btn_ev_t ev;
    } script[] = {
        {  2500, BSP_BTN_A,    BSP_BTN_CLICK },     // 首页:开一局
        {  6000, BSP_BTN_B,    BSP_BTN_CLICK },     // 暂停(暂停音效 + 弹层)
        {  8000, BSP_BTN_B,    BSP_BTN_CLICK },     // 继续
        { 10000, BSP_BTN_A,    BSP_BTN_CLICK },     // 旋转
        { 10500, BSP_BTN_UP,   BSP_BTN_CLICK },     // 硬降(硬降音效 + 锁定)
        { 12000, BSP_BTN_LEFT, BSP_BTN_PRESS },     // 左移到墙(撞墙音效)
        { 14500, BSP_BTN_LEFT, BSP_BTN_RELEASE },
        { 16000, BSP_BTN_RIGHT, BSP_BTN_PRESS },    // 右移到墙(自动重复音效)
        { 18000, BSP_BTN_RIGHT, BSP_BTN_RELEASE },
        { 19000, BSP_BTN_DOWN, BSP_BTN_PRESS },     // 按住下加速下落
        { 21000, BSP_BTN_DOWN, BSP_BTN_RELEASE },
    };
    static size_t next;
    if (next >= sizeof(script) / sizeof(script[0])) return;
    while (next < sizeof(script) / sizeof(script[0]) && now >= script[next].at_ms) {
        const input_event_t event = { .btn = script[next].btn, .event = script[next].ev };
        ESP_LOGI(TAG, "SELFTEST 注入 btn=%d ev=%d", (int)event.btn, (int)event.event);
        (void)xQueueSend(s_input_queue, &event, 0);
        next++;
    }
}
#endif

static void input_task(void *arg) {
    (void)arg;
    input_event_t event;
    for (;;) {
        // 超时轮询而不是死等:游戏要低延迟,同时也留出周期给自检脚本。
        if (xQueueReceive(s_input_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            // 顺带记录当时的按键分压:出现"没碰却自己开始"时,这行日志能证明
            // 是幻触(电压落在某个键的窗口里)还是真有按压。
            ESP_LOGI(TAG, "key btn=%d ev=%d mv=%d",
                     (int)event.btn, (int)event.event, bsp_button_read_mv());
            tetris_ui_key(event.btn, event.event);
        }
#ifdef APP_KEY_SELFTEST
        key_selftest_step((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
#endif
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreate(input_task, "app_input", INPUT_TASK_STACK, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "PockiGo —— 俄罗斯方块 build=%s", APP_BUILD_TAG);

    // I2C 是可选能力:本板无 I2C 从设备,bsp_i2c_init() 返回"不支持"。
    // 这里只记不报错 —— 没有 I2C 不是故障。
    if (bsp_i2c_init() == ESP_OK) {
        bsp_i2c_scan();
    }

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_RST, BSP_LCD_BL);
        return;
    }
    // 这里不要开背光:面板虽然已上电,但屏上还不是我们的画面。
    // 开屏的时机交给渲染任务:首帧整块推给面板之后才点亮背光,这样用户看到的是
    // "亮屏即首页",不会先在开机那几百毫秒里闪一段花屏。

    // 电量是可选项:探测与降级都放在界面层(tetris_ui_start),
    // 探测结果直接决定要不要画电量数字。
    if (input_dispatch_init() != ESP_OK) {
        ESP_LOGE(TAG, "输入任务创建失败,游戏无法操作");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,游戏无法操作");
        return;
    }
    s_input_ready = true;

    if (tetris_ui_start() != ESP_OK) {
        ESP_LOGE(TAG, "游戏界面启动失败");
        return;
    }
#if BSP_BTN_HAS_B
    ESP_LOGI(TAG, "就绪:首页 A 开始 / B 设置;游戏中 左右 移动 A 旋转 上 快落 下 加速 B 暂停");
#else
    ESP_LOGI(TAG, "就绪:首页 OK 开始 / 长按 OK 设置;游戏中 上下 移动 OK 旋转 长按 OK 快落");
#endif
}
