// main/pocket_reflex_ui.h —— 《方向快手》的画面与按键接线。
//
// 分层和 chick_run_ui 一样:规则在 pocket_reflex_model.c(纯逻辑、主机可测),
// 这里只做三件事 —— 收按键、按模型状态画一帧、把脏区推给面板。
// 画布用 chick_ui_draw 的 chick_canvas_t,字形用 chick_run_glyphs,命数爱心
// 直接用 chick_run_sprites 里已有的美术,不新增位图资产。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 冒烟用:一眼看出设备上跑的是哪一版固件(串口第一行)。
#define POCKET_REFLEX_BUILD_TAG "0.9.0-s3"

// 建画布、起渲染任务、亮背光。可重复调用。
esp_err_t pocket_reflex_ui_start(void);

// 按键事件入队。运行在 button 组件的回调里,只入队不阻塞 —— 和 main.c 的约定一致。
void pocket_reflex_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
