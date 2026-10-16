// main/chick_run_ui.h —— 小鸡快跑的界面层(LVGL)。
//
// 职责边界:
//   chick_run_model.c  —— 纯逻辑(物理/生成/碰撞/计分),主机可测;
//   chick_run_sprites.c—— 纯像素数据;
//   chick_run_ui.c     —— 本文件实现:页面、绘制、按键语义、NVS 最高分。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 创建首页与帧定时器。需要在 BSP 显示与 LVGL 初始化成功之后调用。
// 内部会自行获取 LVGL 锁,可从任意任务调用(本应用在 app_main 里调用)。
esp_err_t chick_run_ui_start(void);

// 停止定时器并删除页面(用于启动失败回滚)。
void chick_run_ui_stop(void);

// 按键入口:运行在按键事件任务里,只做入队,不碰 LVGL。
void chick_run_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
