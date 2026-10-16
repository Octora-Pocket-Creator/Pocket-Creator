# components/bsp —— 板级支持包。
#
# 公开接口只在 include/ 下,应用代码不看实现文件,也不关心后面接的是什么硬件
# (bsp_pins.h 是引脚与板级参数的单一事实来源)。
#
#   ESP32-S3R8 掌机:六键 GPIO + 直连 I2S 功放 + 无 I2C 外设
# 注意:这里用 IDF_TARGET(CMake 变量)判断,不要用 CONFIG_IDF_TARGET_ESP32S3 ——
# 组件 CMakeLists 求值时 CONFIG_* 还没展开,那个条件会静默走 else 分支。
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "bsp: 本仓库只支持 esp32s3,当前 IDF_TARGET=${IDF_TARGET}")
endif()
message(STATUS "bsp: IDF_TARGET=${IDF_TARGET} CONFIG_IDF_TARGET=${CONFIG_IDF_TARGET}")

idf_component_register(
    SRCS
        "src/bsp_display.c"
        "src/bsp_display_rounding.c"
        "src/bsp_button_gpio.c"
        "src/bsp_audio_i2s_out.c"
        "src/bsp_battery_adc.c"
        "src/bsp_battery_math.c"
        "src/bsp_none.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s esp_lcd button esp_adc
    PRIV_REQUIRES esp_timer
)
