# 《番茄闹钟》(Pocket Pomodoro)应用组件。
#
# 只编译本应用真正用到的源文件:入口 + 计时规则 + 画面,再加上两个共用层 ——
#   chick_ui_draw.c      画布原语(填充/描边/文字排版)
#   chick_run_glyphs.c   中文字形表(生成物:tools/generate-chick-run-glyphs.py)
# 这两层是小鸡快跑 / 方向快手 / 番茄闹钟共用的"引擎层",换应用不必碰驱动。
# chick_run_sprites.c(索引色像素美术)这一版用不上:番茄闹钟没有命数爱心那类图标,
# 时间/进度条全部是几何绘制,少链一个文件就少 45KB Flash。
#
# 小鸡快跑与方向快手自己的那套文件(chick_run_*.c / pocket_reflex_*.c)以及基线硬件
# 测试页(demo_*.c、ui_pixel*.c、demo_navigation.*)都留在仓库里作为参考,但不再参与
# 本应用的构建 —— 这份 CMakeLists 就是"当前产品是什么"的唯一声明。
idf_component_register(
    SRCS "main.c"
         "pocket_pomodoro_model.c"
         "pocket_pomodoro_ui.c"
         "chick_ui_draw.c"
         "chick_run_glyphs.c"
         "rtttl_player.c"
         "rtttl_wave.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer esp_hw_support nvs_flash
)

# 中文字形:assets/fonts 下的字体文件是**生成器的输入**
# (tools/generate-chick-run-glyphs.py),运行期用的是生成出来的
# main/chick_run_glyphs.c —— 所以这里不再编译 LVGL 格式的字体文件。
#
# ★ 改了会显示的文字就要重跑字体链,否则设备上是方框:
#     python3 tools/generate-chick-run-font.py && python3 tools/generate-chick-run-glyphs.py
# 番茄闹钟新增的 12 个汉字(番茄闹钟专 注休息已完成个)就是这么加进字库的。
