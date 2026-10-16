// components/bsp/include/bsp_button.h
// 按键的【语义键】接口:应用只认 UP/DOWN/LEFT/RIGHT/A/B,不关心物理实现。
// 物理实现与引脚见 bsp_pins.h:
//   * S3R8 板:六个独立 GPIO 按键(上/下/左/右/A/B),六个语义键全部可用。
#pragma once

#include "esp_err.h"

#include "bsp_pins.h"

// 按键的语义索引。数量用 bsp_pins.h 的 BSP_BTN_COUNT(硬件属性,归引脚表管),
// 这里不再定义尾项计数,避免出现 BSP_BTN_COUNT / BSP_BTN_COUNT_ 两个近似名字。
//
// 索引 2 固定留给"确认"键(本板丝印是 A)。按键数量是硬件属性,由 bsp_pins.h 的
// BSP_BTN_COUNT 决定;应用只用 BSP_BTN_HAS_B 判断按键能力,不为具体板子做分支。
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_A,      // 确认/跳跃(丝印 A)
    BSP_BTN_B,      // 返回/暂停(丝印 B)
    BSP_BTN_LEFT,
    BSP_BTN_RIGHT,
} bsp_btn_t;

// 兼容别名:旧代码/基线 demo 里的"确定"就是 A 键。
#define BSP_BTN_OK BSP_BTN_A

// 本板是否有独立的"返回/暂停"键(B)。按键不足 6 个的硬件没有 B 键,
// 应用必须让确认键兼任暂停(见 chick_run_ui.c)。
#define BSP_BTN_HAS_B (BSP_BTN_COUNT >= 6)

typedef enum {
    BSP_BTN_PRESS = 0,   // 按下瞬间(低延迟,适合游戏类即时响应)
    BSP_BTN_CLICK,       // 单击(按下并抬起)
    BSP_BTN_DOUBLE,      // 双击
    BSP_BTN_LONG,        // 长按
    // 松手瞬间:长按之后松手也会发这个事件(CLICK 不会),所以"按住持续生效"的
    // 交互(俄罗斯方块的左右自动重复、下键加速下落)必须用它来收尾 ——
    // 只靠 CLICK 收尾的话,按住超过长按阈值后那些持续状态就再也停不下来。
    BSP_BTN_RELEASE,
} bsp_btn_ev_t;

// 按键事件回调。运行于 button 组件使用的共享 esp_timer 任务,只能入队或执行同等级
// 的有界操作；勿在其中阻塞、访问 LVGL 或做重活。
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 成功调用可重复，并更新回调与 user；失败会回滚本次已创建的按键和资源。
// 本板按键是纯 GPIO 输入,不涉及 ADC 校准;初始化失败时修正故障后可直接重试。
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// 读当前 ADC 原始电压(mV)。本板按键是独立 GPIO、板上没有分压节点,恒返回 -1;
// 该接口保留给按键走 ADC 分压的硬件实现。读取失败或无此硬件时返回 -1。
int bsp_button_read_mv(void);
