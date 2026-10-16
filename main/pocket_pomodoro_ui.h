// main/pocket_pomodoro_ui.h —— 《番茄闹钟》的画面与按键接线。
//
// 分层和 pocket_reflex_ui 一样:计时规则在 pocket_pomodoro_model.c(纯逻辑、主机可测),
// 这里只做四件事 —— 收按键、按模型状态画一帧、把脏区推给面板、到点放提示音。
// 画布用 chick_ui_draw 的 chick_canvas_t,中文用 chick_run_glyphs 的字形表,
// 时间数字是自绘七段数码管(不占字库、任意大)。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 冒烟用:一眼看出设备上跑的是哪一版固件(串口第一行)。
#define POCKET_POMODORO_BUILD_TAG "1.0.2-s3"

// 建画布、起渲染任务、亮背光。可重复调用。
esp_err_t pocket_pomodoro_ui_start(void);

// 按键事件入队。运行在 button 组件的回调里,只入队不阻塞 —— 和 main.c 的约定一致。
void pocket_pomodoro_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
