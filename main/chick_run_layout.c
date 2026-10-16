#include "chick_run_layout.h"

int32_t chick_run_scale_extent(int32_t src, int32_t num, int32_t den) {
    if (src <= 0) return 0;
    if (num <= 0 || den <= 0) return src;          // 比例非法:按原尺寸,不除零
    return (int32_t)(((int64_t)src * num) / den);
}

int32_t chick_run_scale_src_index(int32_t dest, int32_t src, int32_t num, int32_t den) {
    if (src <= 0) return 0;
    if (dest < 0) return 0;
    if (num <= 0 || den <= 0) return dest >= src ? src - 1 : dest;

    int64_t index = ((int64_t)dest * den) / num;   // 最近邻:目标 -> 源
    if (index > src - 1) index = src - 1;
    return (int32_t)index;
}

int32_t chick_run_center_offset(int32_t total, int32_t item) {
    if (item <= 0 || total <= item) return 0;
    return (total - item) / 2;
}
