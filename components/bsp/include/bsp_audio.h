// components/bsp/include/bsp_audio.h
// 音频输出:板上没有 codec,MCU 以 I2S 主机方式直接驱动功放。
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// 初始化 I2S。本板没有 codec、不需要 I2C。成功调用可重复；失败会释放本次已创建的
// I2S channel，修正故障后可重试。
esp_err_t bsp_audio_init(void);

// 设置采样格式(16 bit;单声道源按立体声双写输出,两种功放都能拿到满幅信号)。
//
// ⚠ 采样率切换必须真的改时钟，而不是只记一个变量：16kHz 播完接着播 8kHz 时，
//   时钟不变会让音调和速度整体快一倍。
// 调用方须串行化格式/休眠/唤醒操作，且切换前停止所有 PCM 读写。
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

// 进入低功耗前停止 I2S 时钟并释放发送通道。调用前必须停止所有 PCM 读写；
// 函数幂等，音频未初始化时视为无需处理并返回成功。失败后的重复调用保留失败结果。
esp_err_t bsp_audio_sleep(void);

// deep sleep 专用：确保 I2S 已停止，再将 BCLK/WS/DOUT 设为无上下拉的高阻输入。
// 调用后 I2S 不能在本次运行中恢复，必须立即进入 deep sleep 或重启；
// 不得用于 light sleep。
esp_err_t bsp_audio_prepare_deep_sleep(void);

// 从 light sleep 返回后恢复休眠前的采样格式。函数幂等；deep sleep 唤醒会重启
// 应用，应由 bsp_audio_init() 按正常启动流程重新初始化。
esp_err_t bsp_audio_wake(void);

// 播放。bytes 为字节数(16bit 单声道时 = 采样数 x 2)。
// 本板没有音频输入：bsp_audio_read() 返回 ESP_ERR_NOT_SUPPORTED。
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

// 输出音量 0..100(%)。本板是数字增益，没有硬件音量寄存器。
void bsp_audio_set_volume(uint8_t percent);
