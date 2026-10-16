// components/bsp/include/bsp_display.h
// ST7789P3 240x320 显示:SPI 面板初始化 + 厂商专属寄存器 + LEDC 背光调光。
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>
#include <stdint.h>

#include "bsp_display_rounding.h"
#include "bsp_pins.h"

// 屏幕四周的圆角遮罩半径(像素),取值来自板级参数。
// 本板是直角屏,BSP_LCD_CORNER_RADIUS = 0,表示不做任何遮罩 —— 自绘应用在推屏前
// 调 mask_corners() 时,半径为 0 就直接跳过;圆角屏把该值设为半径像素数即可。
// 名字保留 BSP_LVGL_SCREEN_RADIUS 是因为它在 LVGL 时代就叫这个名,基线 demo 仍在用。
#define BSP_LVGL_SCREEN_RADIUS BSP_LCD_CORNER_RADIUS

// 初始化 SPI 总线、面板、厂商寄存器、背光 LEDC。成功调用可重复；失败会回滚本次
// 已创建的显示资源，修正故障后可重试。成功后屏幕已上电但内容未定。
esp_err_t bsp_display_init(void);

// 取底层面板句柄。想直接 esp_lcd_panel_draw_bitmap 画,或接 LVGL 以外的 GUI 时用。
// 未初始化返回 NULL。
esp_lcd_panel_handle_t bsp_display_panel(void);

// 取底层 panel io 句柄(LVGL 接入需要)。未初始化返回 NULL。
esp_lcd_panel_io_handle_t bsp_display_io(void);

// 背光亮度 0..100(%)。LEDC PWM,0=全灭。
void bsp_display_backlight(uint8_t percent);

// deep sleep 专用：关闭显示、让 ST7789 进入 Sleep In，停止背光 PWM，
// 将 CS/SCLK/MOSI/DC/背光设为安全电平并在 deep sleep 中保持。调用时必须
// 已阻止 LVGL 刷屏，调用后必须立即进入 deep sleep 或重启。
esp_err_t bsp_display_prepare_deep_sleep(void);

// ---------------------------------------------------------------------------
// 说明:本 BSP 不再提供 LVGL 接入层(bsp_lvgl_init/lock/unlock 已删除)。
// 应用采用零 LVGL 自绘:直接往自己的画布渲染,再用 esp_lcd_panel_draw_bitmap
// 推脏区(见 main/chick_run_ui.c 与 docs/development/render-engine-handoff*).
// ---------------------------------------------------------------------------

