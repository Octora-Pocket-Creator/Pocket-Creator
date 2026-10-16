// components/bsp/src/bsp_button_gpio.c
// S3R8 板(ESP32-S3R8)的按键实现:六个独立 GPIO,内部上拉,低电平按下。
//
// 为什么不用 button 组件自带的 GPIO 驱动:本项目坚持"BSP 自己持有 button_driver_t"
// 的写法,把所有权与回滚规则集中在 BSP;而且语义键名(UP/DOWN/A/B/LEFT/RIGHT)必须
// 由 BSP 统一映射,不能让物理丝印泄漏到应用里。
//
// 资源所有权:BSP 持有 BSP_BTN_COUNT 个静态 driver 与 iot_button 句柄;
// 初始化失败时按"先删按钮、后动 GPIO"的顺序回滚,避免定时器回调访问已释放资源。
#include "bsp_button.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "iot_button.h"

static const char *TAG = "bsp_btn";

// 顺序必须与 bsp_btn_t 一致:UP, DOWN, A, B, LEFT, RIGHT。
// 本板没有 OK 键(丝印只有 A/B):A = 确认/跳跃,B = 返回/暂停。
static const gpio_num_t BTN_GPIO[BSP_BTN_COUNT] = BSP_BTN_GPIO_TABLE;

typedef struct {
    button_driver_t base;
    unsigned index;
} bsp_gpio_button_t;

static bsp_gpio_button_t s_drivers[BSP_BTN_COUNT];
static button_handle_t   s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t      s_cb;
static void             *s_user;
static volatile bool     s_ready;

// 低电平按下(内部上拉):0V = 按下。
static uint8_t button_level(button_driver_t *driver) {
    if (!s_ready) return BUTTON_INACTIVE;
    const bsp_gpio_button_t *button = (const bsp_gpio_button_t *)driver;
    return gpio_get_level(BTN_GPIO[button->index]) == 0 ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

static esp_err_t button_driver_delete(button_driver_t *driver) {
    (void)driver; // 静态存储;GPIO 复位在 button_cleanup() 里统一处理。
    return ESP_OK;
}

// button 组件的回调签名固定,故用 usr_data 把"哪个语义键"带回来。
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_ready || !s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);  }
static void cb_click (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);  }
static void cb_double(void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE); }
static void cb_long  (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);   }

// 初始化中途失败时先停掉所有 button driver,再解除 GPIO 配置。
// 顺序不能反:driver 仍在轮询时把 GPIO 交还给别的功能,回调会读到别人的电平。
static void button_cleanup(void) {
    s_cb = NULL;
    s_user = NULL;
    s_ready = false;

    for (int i = BSP_BTN_COUNT - 1; i >= 0; i--) {
        if (!s_btn[i]) continue;
        esp_err_t e = iot_button_delete(s_btn[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "按键 %d 回滚失败: %s", i, esp_err_to_name(e));
            continue;
        }
        s_btn[i] = NULL;
    }

    // 只要有按钮没删掉,就不动 GPIO:那些回调还在用它们。
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (s_btn[i]) return;
    }
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        esp_err_t e = gpio_reset_pin(BTN_GPIO[i]);
        if (e != ESP_OK) ESP_LOGW(TAG, "GPIO%d 复位失败: %s", (int)BTN_GPIO[i], esp_err_to_name(e));
    }
}

static esp_err_t register_callbacks(button_handle_t button, void *index) {
    esp_err_t e = iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, cb_press, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, cb_click, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, cb_double, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, cb_long, index);
    return e;
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    if (s_ready) {
        s_cb = cb;
        s_user = user;
        return ESP_OK;
    }
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        if (s_btn[i]) {
            ESP_LOGE(TAG, "上次按键 %d 回滚不完整，拒绝重复分配资源", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_cb = cb; s_user = user;

    // 六个键各自一份 GPIO 输入配置:上拉、无中断(轮询判定交给 button 组件),
    // 这样按住期间的重复上报节奏由组件的短按窗口统一控制。
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (unsigned)BTN_GPIO[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "GPIO%d 配置失败: %s", (int)BTN_GPIO[i], esp_err_to_name(e));
            button_cleanup();
            return e;
        }
    }

    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        s_drivers[i] = (bsp_gpio_button_t){
            .base = { .get_key_level = button_level, .del = button_driver_delete },
            .index = (unsigned)i,
        };
        // 判定门限由 BSP 显式下发(见 bsp_pins.h):组件 Kconfig 默认长按 1500ms 偏迟钝。
        const button_config_t bc = {
            .short_press_time = BSP_BTN_SHORT_PRESS_MS,
            .long_press_time  = BSP_BTN_LONG_PRESS_MS,
        };
        esp_err_t e = iot_button_create(&bc, &s_drivers[i].base, &s_btn[i]);
        if (e != ESP_OK || !s_btn[i]) {
            ESP_LOGE(TAG, "按键 %d (GPIO%d) 创建失败 (%s)",
                     i, (int)BTN_GPIO[i], esp_err_to_name(e));
            e = e == ESP_OK ? ESP_FAIL : e;
            button_cleanup();
            return e;
        }
        void *idx = (void *)(intptr_t)i;
        e = register_callbacks(s_btn[i], idx);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "按键 %d 回调注册失败: %s", i, esp_err_to_name(e));
            button_cleanup();
            return e;
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "按键就绪:6 键 GPIO 直连(上%d 下%d A%d B%d 左%d 右%d),短按 %dms 长按 %dms",
             (int)BTN_GPIO[BSP_BTN_UP], (int)BTN_GPIO[BSP_BTN_DOWN], (int)BTN_GPIO[BSP_BTN_A],
             (int)BTN_GPIO[BSP_BTN_B], (int)BTN_GPIO[BSP_BTN_LEFT], (int)BTN_GPIO[BSP_BTN_RIGHT],
             BSP_BTN_SHORT_PRESS_MS, BSP_BTN_LONG_PRESS_MS);
    return ESP_OK;
}

int bsp_button_read_mv(void) {
    // 本板没有按键分压节点:电量/按键电压都不可读,返回"无数据"而不是编一个值。
    return -1;
}
