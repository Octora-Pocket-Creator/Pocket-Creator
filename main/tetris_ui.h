// main/tetris_ui.h —— 俄罗斯方块的界面层(零 LVGL 自绘)。
//
// 职责边界:
//   tetris_model.c —— 纯逻辑(下落/旋转/消行/计分),主机可测;
//   tetris_music.h —— 全部旋律与音效(RTTTL 字符串);
//   tetris_ui.c    —— 本文件:版面、绘制、脏区推送、按键语义、最高分与设置。
//
// 渲染方式:自己往一块 RGB565 画布上画,每帧只把【变化的矩形】推给面板
// (SPI 全屏 76800 像素要 ~57ms,脏区通常只有 1~4k 像素)。变化由"与上一帧对比"
// 自动算出,不靠调用点手动登记 —— 漏登记会留下残影,那种 bug 只在实机上看得见。
//
// 本应用是设备上唯一的应用:没有游戏菜单,
// 开机直接进首页。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 冒烟用:一眼看出设备上跑的是哪一版固件(串口第一行)。
#define TETRIS_BUILD_TAG "1.2.0-s3"

// 创建渲染任务与首屏。需要在 BSP 显示初始化成功之后调用(画布/推送缓冲在这里分配)。
esp_err_t tetris_ui_start(void);

// 停止渲染任务并保存设置。
void tetris_ui_stop(void);

// 按键入口:运行在按键事件任务里,只做入队,不阻塞、不绘制。
void tetris_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
