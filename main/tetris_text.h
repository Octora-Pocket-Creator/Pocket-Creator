// main/tetris_text.h —— 俄罗斯方块会显示在屏幕上的所有文字(唯一来源)。
//
// 与 main/chick_run_text.h 同样的约定:
//   1. 字体是按"字符清单"裁剪生成的(assets/fonts/*,生成脚本
//      tools/generate-chick-run-font.py 会同时读这个文件),清单必须和真正
//      显示的文字一致,否则设备上会出现方框/空白;
//   2. tools/generate-chick-run-font.py 从本文件的 #define 里抽取全部非 ASCII 字符;
//   3. 运行时逐码点检查字形覆盖(见 main/tetris_ui.c 的 font_check_coverage)。
// 改文案只改这里,然后重新生成字体。
//
// 按键提示按【本板按键能力】取词(与 chick_run_text.h 同一套判断):
//   * 六键板(上/下/左/右/A/B):提示写 A / B;
//   * 三键板(上/下/确定):确定键兼任旋转与硬降,提示写 OK。
#pragma once

#include "bsp_button.h"

#define TETRIS_TEXT_TITLE        "俄罗斯方块"

// 游戏标题(首页大标题,也是 HUD 里的游戏名)。
#define TETRIS_TEXT_TITLE        "俄罗斯方块"

#if BSP_BTN_HAS_B
#define TETRIS_TEXT_START_HINT   "按 A 开始"
// 首页的操作说明:两行,每行都在 240 像素内排得下(font 20)。
#define TETRIS_TEXT_CONTROLS     "左右 移动 A 旋转\n上 快落 下 加速"
#define TETRIS_TEXT_PAUSE_HINT   "按 B 暂停"
#define TETRIS_TEXT_SETTINGS_HINT "B 设置"
#define TETRIS_TEXT_RESUME_HINT  "A 继续"
#define TETRIS_TEXT_BACK_HINT    "B 回首页"
#define TETRIS_TEXT_AGAIN_HINT   "A 再玩一次"
#define TETRIS_TEXT_SETTINGS_HELP "上下 切换\n左右 调整 B 返回"
#else
// 三键板:没有独立的旋转键,确定键(A)负责旋转,长按进入设置/返回。
#define TETRIS_TEXT_START_HINT   "按 OK 开始"
#define TETRIS_TEXT_CONTROLS     "上下 移动 OK 旋转\n长按 OK 快落"
#define TETRIS_TEXT_PAUSE_HINT   "长按 OK 暂停"
#define TETRIS_TEXT_SETTINGS_HINT "长按 OK 设置"
#define TETRIS_TEXT_RESUME_HINT  "OK 继续"
#define TETRIS_TEXT_BACK_HINT    "长按 OK 回首页"
#define TETRIS_TEXT_AGAIN_HINT   "OK 再玩一次"
#define TETRIS_TEXT_SETTINGS_HELP "上下 调整\nOK 切换 长按返回"
#endif

// HUD 与侧栏标签。
#define TETRIS_TEXT_NEXT         "下一个"
#define TETRIS_TEXT_SCORE        "分数"
#define TETRIS_TEXT_LEVEL        "等级"
#define TETRIS_TEXT_LINES        "消行"
#define TETRIS_TEXT_BEST         "最高 %u"
#define TETRIS_TEXT_LEVEL_FMT    "等级 %u"
#define TETRIS_TEXT_LINES_FMT    "消行 %u"
#define TETRIS_TEXT_SCORE_FMT    "%u"
#define TETRIS_TEXT_PERCENT_FMT  "%u%%"

// 暂停 / 结算 / 提示。
#define TETRIS_TEXT_PAUSE        "暂停"
#define TETRIS_TEXT_GAME_OVER    "游戏结束"
// 结算页:标签与分数分两行画(弹层只有 124 像素宽,单行放不下 6 位分数)。
#define TETRIS_TEXT_THIS_RUN     "本局"
#define TETRIS_TEXT_RECORD       "新纪录！"
// 消行与升级的瞬时提示(压在棋盘上约 1 秒)。
#define TETRIS_TEXT_LEVEL_UP     "升级！"
#define TETRIS_TEXT_TETRIS       "四行消除！"

// 设置页(与 chick_run_text.h 里的三行同名项:背光/音量/色温)。
// 数值含义与实现见 main/chick_run_settings.h 与 main/chick_color.h —— 两块板共用
// 同一套 NVS 记录,用户在小鸡快跑里调好的亮度/音量/色温在这里继续有效。
#define TETRIS_TEXT_SETTINGS      "设置"
#define TETRIS_TEXT_SET_BRIGHT    "亮度"
#define TETRIS_TEXT_SET_VOLUME    "音量"
#define TETRIS_TEXT_SET_WARMTH    "色温"

// 应用文字清单:字体裁剪与运行时字形覆盖检查都以这份清单为准。
#define TETRIS_TEXT_INVENTORY \
    TETRIS_TEXT_TITLE, TETRIS_TEXT_START_HINT, TETRIS_TEXT_CONTROLS, \
    TETRIS_TEXT_PAUSE_HINT, TETRIS_TEXT_SETTINGS_HINT, TETRIS_TEXT_RESUME_HINT, \
    TETRIS_TEXT_BACK_HINT, TETRIS_TEXT_AGAIN_HINT, TETRIS_TEXT_SETTINGS_HELP, \
    TETRIS_TEXT_NEXT, TETRIS_TEXT_SCORE, TETRIS_TEXT_LEVEL, TETRIS_TEXT_LINES, \
    TETRIS_TEXT_BEST, TETRIS_TEXT_LEVEL_FMT, TETRIS_TEXT_LINES_FMT, \
    TETRIS_TEXT_SCORE_FMT, TETRIS_TEXT_PERCENT_FMT, \
    TETRIS_TEXT_PAUSE, TETRIS_TEXT_GAME_OVER, TETRIS_TEXT_THIS_RUN, \
    TETRIS_TEXT_RECORD, TETRIS_TEXT_LEVEL_UP, TETRIS_TEXT_TETRIS, \
    TETRIS_TEXT_SETTINGS, TETRIS_TEXT_SET_BRIGHT, TETRIS_TEXT_SET_VOLUME, \
    TETRIS_TEXT_SET_WARMTH
