// main/chick_run_layout.h —— 界面排版用的小计算,不含 LVGL/ESP-IDF。
//
// 单独成一层是为了能在主机上算清楚:缩放贴图一旦算错,在设备上只表现为
// "图案缺一块/错位",很难靠肉眼定位;居中偏移错了则表现为"整体偏一点",
// 更容易被当成"感觉不对"而返工。
#pragma once

#include <stdint.h>

// 放大/缩小后的像素尺寸。num/den 是缩放比(3/2 = 1.5 倍);
// 比例非法(<= 0)时按原尺寸返回,避免除零。
int32_t chick_run_scale_extent(int32_t src, int32_t num, int32_t den);

// 目标像素 dest 对应的源像素下标(最近邻),结果夹取到 [0, src-1]。
int32_t chick_run_scale_src_index(int32_t dest, int32_t src, int32_t num, int32_t den);

// 让 item 在 [0, total) 内居中时的左上角坐标(总宽小于 item 时返回 0)。
int32_t chick_run_center_offset(int32_t total, int32_t item);
