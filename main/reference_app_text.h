// main/reference_app_text.h —— 参考应用(未编译)的界面文案。
//
// 为什么单独一份:字体子集按"会显示的文字"裁剪生成,清单来自
// main/chick_run_text.h 与 main/tetris_text.h(见 tools/generate-chick-run-font.py)。
// 本分支编译的是游戏菜单 + 小鸡快跑 + 俄罗斯方块,这三个应用的文案在那两个头文件里。
//
// 这里放的是同一棵树里的参考应用——《番茄闹钟》(pocket_pomodoro_ui.c)与
// 《方向快手》(pocket_reflex_ui.c)的文案。它们不参与本分支的构建,但源码要能独立
// 编译通过,所以文案留在这里,而不是混进字体清单:
//   * 直接把这两套文案放回 chick_run_text.h 会让字体子集多出十几个永远不显示的
//     汉字(生成脚本按 #define 抽取清单),清单就不再等于真正显示的文字;
//   * 哪天真要重新启用某个参考应用,把它的文案挪回对应的 *_text.h 并重跑
//     tools/generate-chick-run-font.py && tools/generate-chick-run-glyphs.py 即可。
#pragma once

// 《方向快手》的标题。上下左右四个字已经在小鸡快跑的文案里,字形清单不变。
#define POCKET_TEXT_TITLE       "上下左右"

// 《番茄闹钟》的全部文案(12 个汉字:番茄闹钟专 注休息 已完成个)。
#define POMODORO_TEXT_TITLE        "番茄闹钟"
#define POMODORO_TEXT_FOCUS        "专注"
#define POMODORO_TEXT_BREAK        "休息"
#define POMODORO_TEXT_DONE_FMT     "已完成 %u 个"
#define POMODORO_TEXT_FOCUS_END    "专注结束"
#define POMODORO_TEXT_BREAK_END    "休息结束"
#define POMODORO_TEXT_HINT_START   "A 开始  B 重置"
#define POMODORO_TEXT_HINT_PAUSE   "A 暂停  B 重置"
#define POMODORO_TEXT_HINT_ADJUST  "上 下 调整 左 右 切换"
