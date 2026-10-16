// components/bsp/include/bsp_pins.h
// 硬件引脚与参数的【单一事实来源】。换板/改硬件只需改这一个文件。
// 每项都注明"为什么是这个值",便于二次开发时判断能不能改。
//
// 板级参数只有一套：ESP32-S3R8 掌机(16MB Flash + 8MB Octal PSRAM,
// 六键 GPIO 直连、可直连 I2S 功放、无 codec/电量计)。详见
// docs/hardware-design/s3r8-board.md。
// 应用与 BSP 实现只依赖宏名,不依赖具体板子。
#pragma once

#include "driver/spi_master.h"
#include "driver/i2c_types.h"
#include "hal/adc_types.h"

// ============================================================================
// S3R8 板:ESP32-S3R8(16MB Flash + 8MB Octal PSRAM)
// ============================================================================

// --- 显示:ST7789/ST7789P3 240x320,4-line SPI2 ---------------------------------
#define BSP_LCD_W            240
#define BSP_LCD_H            320
#define BSP_LCD_SPI_HOST     SPI2_HOST
#define BSP_LCD_MOSI         12
#define BSP_LCD_SCLK         11
#define BSP_LCD_CS           10
#define BSP_LCD_DC           9
// 本板复位脚接到了 MCU(GPIO47),esp_lcd_panel_reset() 走硬件复位。
#define BSP_LCD_RST          47
#define BSP_LCD_BL           13          // 背光,LEDC PWM 调光
// ⚠ 本板背光是【低电平点亮】的 PMOS/NPN 驱动:占空比越大越暗,故 BSP 内部把
//   百分比反相后再写 LEDC。若换板后"亮度越大越黑",把这个宏改成 0。
#define BSP_LCD_BL_ACTIVE_LOW 1
// 板级验证过的 SPI 时钟。想提高帧率可以再往上试(240x320x2 一帧约 150KB),
// 但必须先实测无花屏、无偏色再改这里。
#define BSP_LCD_PCLK_HZ      (40 * 1000 * 1000)
// ST7789 SCK 空闲低、上升沿采样 → SPI mode 0。
#define BSP_LCD_SPI_MODE     0
// 本批屏出厂即需反色(厂商参考例程 TFT_init() 末尾无条件发 0x21 INVON)。
// 若换屏后画面呈负片,把这里改成 0。
#define BSP_LCD_INVERT_COLOR 1

// 屏幕四角的圆角半径(像素)。0 = 直角屏,不做圆角遮罩。
// 本板是普通矩形屏,四周不需要黑色圆角。
#define BSP_LCD_CORNER_RADIUS 0

// 背光 LEDC 参数(低速定时器 0 / 通道 0)
#define BSP_BL_LEDC_TIMER    LEDC_TIMER_0
#define BSP_BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BSP_BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BSP_BL_LEDC_FREQ_HZ  5000

// --- 按键:六个独立 GPIO,内部上拉,低电平按下 ---------------------------------
// 物理丝印 → 语义键(应用的语义由 BSP 统一映射,应用只认 UP/DOWN/LEFT/RIGHT/A/B):
//   UP=GPIO38  DOWN=GPIO21  LEFT=GPIO39  RIGHT=GPIO14  A=GPIO16  B=GPIO8
// 本板没有“确定/OK”键:A 就是确认键,B 就是返回键。
// GPIO39/40/41/42 是 S3 原生 JTAG 引脚:本板把 39 用作 LEFT、40/41/42 用作 I2S,
// 因此调试必须改用 USB Serial/JTAG,不能再接外部 JTAG 排针。
#define BSP_BTN_GPIO_TABLE   {38, 21, 16, 8, 39, 14}   // 顺序 = UP,DOWN,A,B,LEFT,RIGHT
#define BSP_BTN_COUNT        6

// 按键判定时序(ms):由 BSP 显式下发给 button 组件,不依赖它的 Kconfig 默认值。
// 短按 180ms 与组件默认一致;长按组件默认 1500ms 偏迟钝,收敛到 500ms。
#define BSP_BTN_SHORT_PRESS_MS  180   // 短按(单击)判定窗口
#define BSP_BTN_LONG_PRESS_MS   500   // 长按触发时间

// --- 音频:无 codec,MCU 直接驱动 I2S 功放(PCM5102/MAX98357A 一类)-----------
// 功放要求:Philips I2S 从机、不需要 MCLK、3.3V 供电。
// 板上若没贴功放,初始化照样成功,只是没有声音 —— 不阻塞游戏启动。
#define BSP_I2S_PORT         I2S_NUM_0
#define BSP_I2S_MCLK         (-1)    // 功放不需要主时钟;接上反而可能引入噪声
#define BSP_I2S_BCLK         41
#define BSP_I2S_WS           42
#define BSP_I2S_DOUT         40      // 播放:MCU → 功放
#define BSP_I2S_DIN          (-1)    // 麦克风走独立 I2S1(GPIO 17/18/4),本 BSP 不接管;
                                     // 板上预留 ZTS6672 焊盘,启用前不归任何模块所有
// -1 = 功放使能脚未接 MCU(常通)。若你的板子接了,填对应 GPIO。
#define BSP_I2S_PA_CTRL      (-1)

// 本板没有 I2C 从设备。
// bsp_i2c_* 在该板返回"不支持",上层据此降级而不是当故障处理;
// 电量走 GPIO1 的 ADC 分压(见 bsp_battery_adc.c),不需要 I2C。

// --- 电池:VBAT -- 200K -- GPIO1(ADC1_CH0) -- 200K -- GND ---------------------
// 对半分压:引脚上的电压是电池电压的一半,满电 4.1V → 引脚 2.05V。
// ⚠ 这里有个真实误差源:戴维南源阻抗 = 200K‖200K = 100kΩ,偏高 ——
//   SAR 的采样电容充不满,单次读数会系统性偏低且抖动。所以:
//     1. 驱动里多次采样取平均(BSP_BATT_SAMPLE_TIMES);
//     2. 留了 BSP_BATT_MV_SCALE_PPM 标定系数:用万用表实测电池电压后按比例修正;
//     3. 硬件上在 GPIO1 对地并一颗 100nF 能明显改善(滤波 + 给采样电容补电荷)。
// 用 ADC1 而不是 ADC2:ADC2 在 Wi-Fi 工作时不可用。
#define BSP_BATT_ADC_UNIT         ADC_UNIT_1
#define BSP_BATT_ADC_CHANNEL      ADC_CHANNEL_0    // GPIO1
#define BSP_BATT_DIVIDER_R_TOP    200000
#define BSP_BATT_DIVIDER_R_BOTTOM 200000
#define BSP_BATT_MV_SCALE_PPM     1000000   // 1.000000 = 不修正
#define BSP_BATT_SAMPLE_TIMES     16
#define BSP_BATT_ADC_ATTEN        ADC_ATTEN_DB_12  // 量程约 0~3.1V,覆盖 2.05V 满电
// 防抖参数(实现在 bsp_battery_math.c):
//   IIR_SHIFT:一阶平滑的权重是 1/2^shift。3 ≈ 每次只采纳 1/8 的新值,
//             电量每 10s 读一次,大约 30s 内跟得上真实变化,但单次尖峰推不动它;
//   SOC_HYSTERESIS:百分比变化小于这个值就不更新。电压在曲线段边界抖动时
//             会在相邻两档百分比之间反复跳,图标跟着闪,滞回把这种跳动吃掉。
#define BSP_BATT_IIR_SHIFT        3
#define BSP_BATT_SOC_HYSTERESIS   2
// 单节锂电的合理电压区间:低于 2.5V 已彻底放空,高于 4.4V 不可能是这种电池。
// 超出区间的采样直接丢弃(不参与滤波),避免耦合干扰把电量拉高/拉低。
#define BSP_BATT_VBAT_MIN_MV      2500
#define BSP_BATT_VBAT_MAX_MV      4400

