// main/chick_run_text.h —— 小鸡快跑会显示在屏幕上的所有文字(唯一来源)。
//
// 为什么单独放一个头文件:
//   1. 字体是按"字符清单"裁剪生成的,清单必须和真正显示的文字一致;
//   2. 生成脚本(tools/generate-chick-run-font.py)从这个文件抽取清单,
//      测试(tests/test_chick_run_text.py)也对比它和实际用到的字符;
//   3. 运行时还会用这份清单逐码点检查字形覆盖(见 chick_run_ui.c)。
// 改文案时只改这里,然后重新生成字体,否则设备上会出现方框/空白。
//
// 按键提示按【本板按键能力】取词,不是按板名:
//   * 六键板(上/下/左/右/A/B):提示写 A / B;
//   * 没有 B 键的硬件(只有确认键):提示写 OK。
// 两套文案里的字都在同一份字体清单内(ASCII 全量 + 下面这些汉字),不用重新生成字体。
#pragma once

#include "bsp_button.h"

#define CHICK_TEXT_TITLE        "小鸡快跑"

#if BSP_BTN_HAS_B
// 六键板:A 是确认/跳跃键,B 是返回/暂停键。
#define CHICK_TEXT_START_HINT   "按 A 开始"
#define CHICK_TEXT_CONTROLS     "A 跳 躲开障碍\nB 暂停 返回"
#define CHICK_TEXT_PAUSE_HINT   "A 继续"
#define CHICK_TEXT_BACK_HINT    "B 回首页"
#define CHICK_TEXT_AGAIN_HINT   "A 再玩一次"
#define CHICK_TEXT_SETTINGS_HINT "B 设置"
#define CHICK_TEXT_SETTINGS_HELP "上 下 切换\n左 右 调整 B 返回"
#define CHICK_TEXT_LEVEL_NEXT_HINT "A 下一关"
#else
// 没有 B 键时:确认键(OK)兼任确认与暂停,长按返回。
#define CHICK_TEXT_START_HINT   "按 OK 开始"
#define CHICK_TEXT_CONTROLS     "上 跳 躲开障碍\nOK 暂停 长按返回"
#define CHICK_TEXT_PAUSE_HINT   "OK 继续"
#define CHICK_TEXT_BACK_HINT    "长按 OK 回首页"
#define CHICK_TEXT_AGAIN_HINT   "OK 再玩一次"
#define CHICK_TEXT_SETTINGS_HINT "长按 OK 设置"
#define CHICK_TEXT_SETTINGS_HELP "上 下 调整\nOK 切换 长按返回"
#define CHICK_TEXT_LEVEL_NEXT_HINT "OK 下一关"
#endif

// 《方向快手》(main/pocket_reflex_ui.c)的标题。上下左右四个字已经在小鸡快跑的
// 文案里,字形清单不变 —— 复用同一份字体,不用重新生成字库。
#define POCKET_TEXT_TITLE       "上下左右"

// 《番茄闹钟》(main/pocket_pomodoro_ui.c)的全部文案。
// 这一套引入了 12 个新汉字(番茄闹钟专 注休息 已完成个),所以【字体子集必须重跑】:
//     python3 tools/generate-chick-run-font.py && python3 tools/generate-chick-run-glyphs.py
// 只改文案也照样要重跑这两条,否则设备上会出现方框/空白(经典踩坑点)。
// 时间数字不用字体:mm:ss 是自绘七段数码管(见 pocket_pomodoro_ui.c),零字库成本。
#define POMODORO_TEXT_TITLE        "番茄闹钟"
#define POMODORO_TEXT_FOCUS        "专注"
#define POMODORO_TEXT_BREAK        "休息"
#define POMODORO_TEXT_DONE_FMT     "已完成 %u 个"
#define POMODORO_TEXT_FOCUS_END    "专注结束"
#define POMODORO_TEXT_BREAK_END    "休息结束"
#define POMODORO_TEXT_HINT_START   "A 开始  B 重置"
#define POMODORO_TEXT_HINT_PAUSE   "A 暂停  B 重置"
#define POMODORO_TEXT_HINT_ADJUST  "上 下 调整 左 右 切换"

// 跑动过程中 HUD 只显示距离(米):得分是"距离 + 道具加分"的综合分,
// 只在结算页给一次(见 CHICK_TEXT_THIS_RUN_FMT / CHICK_TEXT_LEVEL_RESULT_FMT),
// 这样界面上的数字只有一个,不会被道具加分跳来跳去。
#define CHICK_TEXT_PICKUP_HINT  "跳起来收集金币"
#define CHICK_TEXT_BEST_FMT     "最高分 %u 分"
#define CHICK_TEXT_METERS_FMT   "%u 米"
#define CHICK_TEXT_PAUSE        "暂停"
#define CHICK_TEXT_GAME_OVER    "游戏结束"
#define CHICK_TEXT_THIS_RUN_FMT "本局 %u 分"
#define CHICK_TEXT_RECORD_FMT   "新纪录！\n本局 %u 分"
#define CHICK_TEXT_BATTERY_NONE "--"

// 玩法模式(首页用上下/左右键切换)。
#define CHICK_TEXT_MODE_FREE       "自由模式"
#define CHICK_TEXT_MODE_LEVEL      "关卡模式"
#define CHICK_TEXT_MODE_FMT        "上 下:%s"
#define CHICK_TEXT_LEVEL_BEST_FMT  "最高第 %u 关"
// 关卡模式的 HUD 与结算。
#define CHICK_TEXT_LEVEL_FMT       "第 %u 关"
// 结算页:第几关 + 整局总得分(道具分也含在内)。
#define CHICK_TEXT_LEVEL_RESULT_FMT "第 %u 关\n得分 %u 分"
#define CHICK_TEXT_LEVEL_CLEAR     "恭喜您，过关啦~"

// 设置页(背光/音量)。
#define CHICK_TEXT_SETTINGS        "设置"
#define CHICK_TEXT_SETTINGS_BRIGHT "亮度"
#define CHICK_TEXT_SETTINGS_VOLUME "音量"
// 色温补偿:面板/背光偏冷时的暖色校正强度(0 = 原色)。
#define CHICK_TEXT_SETTINGS_WARM   "色温"
#define CHICK_TEXT_PERCENT_FMT     "%u%%"

// 字体策略:中文与数字共用按本清单裁剪的 Noto Sans SC 子集
// (assets/fonts/chick_run_font_20.c / _28.c,生成脚本 tools/generate-chick-run-font.py)。
// 不再使用 LVGL 内置的思源黑体子集:那份是按 LVGL 自己 demo 文案裁的,
// "鸡、跑、按、开、跳、滑铲、暂停、继续、首页、纪录"等常用字都不在里面。
// 电量百分比用 Montserrat 14(纯 ASCII);数字固定用墨色(见 chick_run_ui.c)。
//
// 应用文字清单:字形覆盖检查与(后续)字体裁剪都以这份清单为准。
// 注意:这里列的是【当前板子实际生效】的那一套文案(条件编译后只剩一套)。
#define CHICK_TEXT_INVENTORY \
    CHICK_TEXT_TITLE, CHICK_TEXT_START_HINT, CHICK_TEXT_CONTROLS, \
    CHICK_TEXT_PICKUP_HINT, CHICK_TEXT_BEST_FMT, CHICK_TEXT_METERS_FMT, \
    CHICK_TEXT_PAUSE, \
    CHICK_TEXT_PAUSE_HINT, CHICK_TEXT_BACK_HINT, CHICK_TEXT_GAME_OVER, \
    CHICK_TEXT_THIS_RUN_FMT, CHICK_TEXT_RECORD_FMT, CHICK_TEXT_AGAIN_HINT, \
    CHICK_TEXT_SETTINGS, CHICK_TEXT_SETTINGS_HINT, CHICK_TEXT_SETTINGS_BRIGHT, \
    CHICK_TEXT_SETTINGS_VOLUME, CHICK_TEXT_SETTINGS_WARM, CHICK_TEXT_SETTINGS_HELP, \
    CHICK_TEXT_MODE_FREE, CHICK_TEXT_MODE_LEVEL, CHICK_TEXT_MODE_FMT, \
    CHICK_TEXT_LEVEL_BEST_FMT, CHICK_TEXT_LEVEL_FMT, CHICK_TEXT_LEVEL_CLEAR, \
    CHICK_TEXT_LEVEL_RESULT_FMT, CHICK_TEXT_LEVEL_NEXT_HINT, \
    POCKET_TEXT_TITLE, \
    POMODORO_TEXT_TITLE, POMODORO_TEXT_FOCUS, POMODORO_TEXT_BREAK, \
    POMODORO_TEXT_DONE_FMT, POMODORO_TEXT_FOCUS_END, POMODORO_TEXT_BREAK_END, \
    POMODORO_TEXT_HINT_START, POMODORO_TEXT_HINT_PAUSE, \
    POMODORO_TEXT_HINT_ADJUST
