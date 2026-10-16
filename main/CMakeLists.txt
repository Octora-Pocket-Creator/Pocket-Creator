# main/ 组件:俄罗斯方块(设备上唯一的应用)。
#
# 只编译本应用真正需要的源文件。基线 main/ 下的硬件测试页(demo_*.c、ui_pixel*.c、
# demo_navigation.*)、小鸡快跑与两个参考应用(番茄闹钟、方向快手)保留在仓库里但不参与
# 构建 —— 派生应用的 UI 与交互全部自己实现(见 docs/development/ai-guide.md 的 UI 重做规则)。
#
# 为什么要带 chick_run_* 的几个文件:字形表、设置存储(亮度/音量/色温,NVS 命名空间
# chick_run)、背景渐变/抖动/色温补偿与 UI 绘制原语是两个游戏共用的基础设施,
# chick_run_ui.c 等界面代码才是小鸡快跑专有、不参与构建的部分。
idf_component_register(
    SRCS "main.c"
         "rtttl_player.c"
         "rtttl_wave.c"
         # 俄罗斯方块本身
         "tetris_ui.c"
         "tetris_model.c"
         "tetris_store.c"
         # 共用基础设施:设置存储(NVS)+ 绘制原语 + 字形表
         # 小鸡快跑(chick_run_ui.c / chick_run_model.c / chick_run_sprites.c /
         # chick_run_layout.c)在同一棵树里保留源码但不参与构建 —— 本应用只做俄罗斯
         # 方块(见 docs/development/ai-guide.md 的 UI 重做规则)。
         "chick_run_settings.c"
         "chick_run_store.c"
         # 字形表与绘制原语
         "chick_run_glyphs.c"
         "chick_ui_draw.c"
         "chick_run_scene.c"
         "chick_color.c"
         "chick_gradient.c"
         "chick_dither.c"   # 生成物:tools/generate-dither-mask.py
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer esp_hw_support nvs_flash
)

# 中文字形:assets/fonts 下的字体文件是**生成器的输入**
# (tools/generate-chick-run-glyphs.py),运行期用的是生成出来的
# main/chick_run_glyphs.c —— 所以这里不再编译 LVGL 格式的字体文件。

# 开发自检(默认关):自动走一遍页面 + 串口回传帧缓冲,配合 tools/fb_shot.py 出图。
option(CHICK_RUN_SHOT "开发自检:自动走页面 + 串口回传帧缓冲" OFF)
if(CHICK_RUN_SHOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CHICK_RUN_SHOT=1)
endif()

# 开发自检(默认关):按固定时间表注入按键,让设备自己打完一局(移动/旋转/硬降 ->
# 消行 -> 结束 -> 重开),用于在没有人按键时验证按键路径、音效与动画。
option(APP_KEY_SELFTEST "开发自检:注入按键把一整局游戏打完" OFF)
if(APP_KEY_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_KEY_SELFTEST=1)
endif()
