# 《小鸡快跑》(Chick Run)应用组件。
#
# 只编译本应用真正用到的源文件:
#   main.c                 入口:板级初始化 + 按键事件入队
#   chick_run_model.c      物理/生成/碰撞/计分(纯逻辑,主机可测)
#   chick_run_scene.c      世界的脏矩形计划(纯逻辑,主机可测)
#   chick_run_layout.c     布局常量与坐标换算(纯逻辑,主机可测)
#   chick_run_sprites.c    索引色像素美术(生成物:tools/author-chick-run-sprites.py)
#   chick_run_settings.c   设置项与默认值(纯逻辑,主机可测)
#   chick_run_store.c      NVS 存档:最高分/设置(只依赖 nvs)
#   chick_run_ui.c         画面、按键语义、推屏
#   chick_color.c          屏幕色温补偿(查表,主机可测)
#   chick_dither.c         蓝噪声抖动掩码(生成物:tools/generate-dither-mask.py)
#   chick_gradient.c       按行渐变 + 抖动(主机可测)
#   共用引擎层:chick_ui_draw.c(画布原语)、chick_run_glyphs.c(中文字形表,生成物)
#   音频:rtttl_player.c + rtttl_wave.c(RTTTL 音效与循环 BGM)
#
# 方向快手(pocket_reflex_*.c)、番茄闹钟(pocket_pomodoro_*.c)以及基线硬件测试页
# (demo_*.c、ui_pixel*.c、demo_navigation.*)都留在仓库里作为参考,但不参与本应用
# 的构建 —— 这份 CMakeLists 就是"当前产品是什么"的唯一声明。
idf_component_register(
    SRCS "main.c"
         "chick_run_model.c"
         "chick_run_scene.c"
         "chick_run_layout.c"
         "chick_run_sprites.c"
         "chick_run_settings.c"
         "chick_run_store.c"
         "chick_run_ui.c"
         "chick_color.c"
         "chick_dither.c"
         "chick_gradient.c"
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
# tests/test_chick_run_text.py 会在构建门禁里拦住"显示了但字库没登记"的字符。
