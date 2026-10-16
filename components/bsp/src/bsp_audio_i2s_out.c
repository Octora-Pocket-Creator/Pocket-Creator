// components/bsp/src/bsp_audio_i2s_out.c
// S3R8 板(ESP32-S3R8)的音频实现:板上没有 codec,MCU 直接以 I2S 主机方式驱动功放。
//
// 板上没有 codec,这决定了这里的三个设计点:
//   1. 没有控制口:采样率、位宽只能靠 I2S 时钟;音量只能做【数字增益】,
//      所以 write() 里要过一次软件增益并做削顶保护;
//   2. 功放不需要 MCLK:不接 MCLK 时 BCLK/WS/DOUT 就够(Philips I2S);
//   3. 单声道源的声道落点不统一:有些功放取左声道、有些取 (L+R)/2。
//      因此统一按【立体声双写】输出(RTTTL 是单声道,复制到两个槽位),
//      这样两种功放都能拿到满幅信号,不需要改代码换板。
//
// 采样率切换必须真的把时钟改掉,而不是只记一个变量 ——
// 16kHz 播完接着播 8kHz 时,时钟不变会让音调和速度整体快一倍。
#include "bsp_audio.h"

#include <string.h>

#include "bsp_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_audio";

#define OUT_FRAMES       256                 // 32ms @8kHz / 16ms @16kHz 的合成块
#define WRITE_TIMEOUT_MS 200

static i2s_chan_handle_t s_tx;
static uint32_t          s_hz;
static uint8_t           s_ch;               // 调用方数据的声道数(1=单声道)
static int32_t           s_gain_q15 = 19660; // 默认 60%:未设置音量时也不要炸耳朵
static bool              s_ready;
static bool              s_suspended;

// 单声道 → 立体声 的转换缓冲。放静态区(内部 RAM)而不是每次 malloc:
// 播放路径在音频任务里按块调用,不能引入分配失败的可能。
static int16_t s_out[OUT_FRAMES * 2];

static int32_t percent_to_gain_q15(uint8_t percent) {
    if (percent > 100) percent = 100;
    // 留 ~10% 余量:合成音在起止包络处可能瞬时超过方波满幅,削顶会产生咔哒声。
    return (int32_t)percent * 29491 / 100;   // 29491 ≈ 0.9 * 32767
}

static inline int16_t apply_gain(int16_t sample, int32_t gain_q15) {
    int32_t value = ((int32_t)sample * gain_q15) >> 15;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

static void set_pins_to_input(const gpio_num_t *pins, size_t count) {    for (size_t i = 0; i < count; i++) {
        if ((int)pins[i] < 0) continue;
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (unsigned)pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&cfg);
        if (e != ESP_OK) ESP_LOGW(TAG, "GPIO%d 配置失败: %s", (int)pins[i], esp_err_to_name(e));
    }
}

// 功放使能脚(可选):本板 BSP_I2S_PA_CTRL = -1 表示常通,这段代码不参与编译。
static void prepare_pa_ctrl(void) {
#if BSP_I2S_PA_CTRL >= 0
    const gpio_config_t pa = {
        .pin_bit_mask = 1ULL << (unsigned)BSP_I2S_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&pa);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "功放使能脚 GPIO%d 配置失败: %s", (int)BSP_I2S_PA_CTRL, esp_err_to_name(e));
        return;
    }
    gpio_set_level(BSP_I2S_PA_CTRL, 1);
#endif
}

esp_err_t bsp_audio_init(void) {
    if (s_ready) return ESP_OK;
    if (s_tx) {
        ESP_LOGE(TAG, "上次音频初始化回滚不完整，拒绝覆盖仍存活的 I2S 通道");
        return ESP_ERR_INVALID_STATE;
    }

    const i2s_chan_config_t chan = {
        .id = BSP_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = OUT_FRAMES,
        .auto_clear = true,          // 没有数据时输出静音而不是旧数据
    };
    esp_err_t e = i2s_new_channel(&chan, &s_tx, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 发送通道创建失败: %s", esp_err_to_name(e));
        s_tx = NULL;
        return e;
    }

    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_I2S_BCLK,
            .ws   = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    e = i2s_channel_init_std_mode(s_tx, &std);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 标准模式初始化失败 (BCLK=%d WS=%d DOUT=%d): %s",
                 BSP_I2S_BCLK, BSP_I2S_WS, BSP_I2S_DOUT, esp_err_to_name(e));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return e;
    }
    e = i2s_channel_enable(s_tx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 发送通道使能失败: %s", esp_err_to_name(e));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return e;
    }

    prepare_pa_ctrl();

    s_hz = 16000;
    s_ch = 1;
    s_suspended = false;
    s_ready = true;
    ESP_LOGI(TAG, "音频就绪:直连 I2S 功放(BCLK=%d WS=%d DOUT=%d,无 MCLK,16bit)",
             BSP_I2S_BCLK, BSP_I2S_WS, BSP_I2S_DOUT);
    return ESP_OK;
}

esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch) {
    if (!s_ready || !s_tx) return ESP_ERR_INVALID_STATE;
    // 本板功放固定 16bit Philips I2S;其余位宽没有对应硬件配置,明确拒绝而不是假装成功。
    if (bits != 16 || (ch != 1 && ch != 2) || hz == 0) return ESP_ERR_NOT_SUPPORTED;
    if (hz == s_hz && ch == s_ch) return ESP_OK;

    esp_err_t e = i2s_channel_disable(s_tx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 停通道失败,拒绝改采样率: %s", esp_err_to_name(e));
        return e;
    }
    const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    e = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 采样率切换失败 (%u Hz): %s", (unsigned)hz, esp_err_to_name(e));
        return e;
    }
    e = i2s_channel_enable(s_tx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 重新使能失败: %s", esp_err_to_name(e));
        return e;
    }
    s_hz = hz;
    s_ch = ch;
    s_suspended = false;
    return ESP_OK;
}

esp_err_t bsp_audio_write(const void *pcm, size_t bytes) {
    if (!s_ready || !s_tx) return ESP_ERR_INVALID_STATE;
    if (s_suspended) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || bytes == 0) return ESP_OK;

    const int16_t *src = (const int16_t *)pcm;
    size_t frames = bytes / sizeof(int16_t) / s_ch;   // 输入总帧数
    if (frames == 0) return ESP_OK;

    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > OUT_FRAMES) chunk = OUT_FRAMES;
        for (size_t i = 0; i < chunk; i++) {
            const int16_t left = src[(done + i) * s_ch];
            // 单声道源复制到两个槽位;立体声源按原样输出(这里不做混音)。
            const int16_t right = (s_ch == 1) ? left : src[(done + i) * s_ch + 1];
            s_out[i * 2]     = apply_gain(left,  s_gain_q15);
            s_out[i * 2 + 1] = apply_gain(right, s_gain_q15);
        }
        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx, s_out, chunk * 2 * sizeof(int16_t), &written,
                                        pdMS_TO_TICKS(WRITE_TIMEOUT_MS));
        if (e != ESP_OK || written != chunk * 2 * sizeof(int16_t)) {
            ESP_LOGE(TAG, "I2S 写入失败: %s (写了 %u/%u 字节)",
                     esp_err_to_name(e), (unsigned)written,
                     (unsigned)(chunk * 2 * sizeof(int16_t)));
            return e == ESP_OK ? ESP_ERR_TIMEOUT : e;
        }
        done += chunk;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_read(void *pcm, size_t bytes) {
    // 本板没有麦克风:与其返回静音冒充"录到了",不如明确告诉调用方没有这条路。
    (void)pcm; (void)bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_audio_set_volume(uint8_t percent) {
    s_gain_q15 = percent_to_gain_q15(percent);
#if BSP_I2S_PA_CTRL >= 0
    // 0% 时顺手把功放也静音,避免底噪;有使能脚时才有意义。
    gpio_set_level(BSP_I2S_PA_CTRL, percent == 0 ? 0 : 1);
#endif
}

esp_err_t bsp_audio_sleep(void) {
    if (!s_ready || !s_tx) return ESP_OK;   // 未初始化视为无需处理
    if (s_suspended) return ESP_OK;

    esp_err_t e = i2s_channel_disable(s_tx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "音频休眠失败: %s", esp_err_to_name(e));
        return e;
    }
    s_suspended = true;
    ESP_LOGI(TAG, "音频已停钟(BCLK/WS 停止输出)");
    return ESP_OK;
}

esp_err_t bsp_audio_wake(void) {
    if (!s_ready || !s_tx) return ESP_OK;
    if (!s_suspended) return ESP_OK;

    esp_err_t e = i2s_channel_enable(s_tx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "音频唤醒失败: %s", esp_err_to_name(e));
        return e;
    }
    s_suspended = false;
    return ESP_OK;
}

esp_err_t bsp_audio_prepare_deep_sleep(void) {
    esp_err_t first_error = ESP_OK;
    if (s_tx) {
        esp_err_t e = i2s_channel_disable(s_tx);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "deep sleep 前停止 I2S 失败: %s", esp_err_to_name(e));
            first_error = e;
        }
        // 删掉通道才能真正把 BCLK/WS/DOUT 从 I2S 矩阵里交还给 GPIO。
        // 本函数约定：调用后必须立即进入 deep sleep 或重启，不得再用 I2S。
        esp_err_t del = i2s_del_channel(s_tx);
        if (del != ESP_OK && first_error == ESP_OK) first_error = del;
        s_tx = NULL;
    }
    s_ready = false;
    s_suspended = true;
#if BSP_I2S_PA_CTRL >= 0
    if (first_error == ESP_OK) gpio_set_level(BSP_I2S_PA_CTRL, 0);
#endif
    const gpio_num_t pins[3] = { BSP_I2S_BCLK, BSP_I2S_WS, BSP_I2S_DOUT };
    set_pins_to_input(pins, sizeof(pins) / sizeof(pins[0]));
    return first_error;
}
