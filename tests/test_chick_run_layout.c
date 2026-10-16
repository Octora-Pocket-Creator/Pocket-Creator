// tests/test_chick_run_layout.c —— 首页缩放/居中计算的主机测试。
//
// 缩放映射算错在设备上只表现为"图案缺一块",居中算错只是"看着偏一点",
// 都是靠肉眼很难定位的问题,所以在这里把边界钉死。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "chick_run_layout.h"

static void test_extent(void) {
    assert(chick_run_scale_extent(40, 3, 2) == 60);    // 标题页小鸡 1.5 倍
    assert(chick_run_scale_extent(44, 3, 2) == 66);
    assert(chick_run_scale_extent(20, 3, 2) == 30);    // 仙人掌 1.5 倍
    assert(chick_run_scale_extent(41, 3, 2) == 61);    // 奇数源尺寸向下取整
    assert(chick_run_scale_extent(40, 1, 1) == 40);    // 1 倍
    assert(chick_run_scale_extent(40, 2, 2) == 40);
    assert(chick_run_scale_extent(0, 3, 2) == 0);
    assert(chick_run_scale_extent(40, 0, 2) == 40);    // 比例非法时退化为原尺寸
    assert(chick_run_scale_extent(40, 3, 0) == 40);
}

static void test_src_index(void) {
    // 1.5 倍放大:每 3 个目标像素对应 2 个源像素。
    assert(chick_run_scale_src_index(0, 40, 3, 2) == 0);
    assert(chick_run_scale_src_index(1, 40, 3, 2) == 0);
    assert(chick_run_scale_src_index(2, 40, 3, 2) == 1);
    assert(chick_run_scale_src_index(3, 40, 3, 2) == 2);
    assert(chick_run_scale_src_index(59, 40, 3, 2) == 39);   // 最后一个目标像素落在最后一列

    // 单调不减,且始终落在源范围内。
    int32_t last = -1;
    for (int32_t dest = 0; dest < 60; dest++) {
        const int32_t src = chick_run_scale_src_index(dest, 40, 3, 2);
        assert(src >= 0 && src < 40);
        assert(src >= last);
        last = src;
    }

    // 越界与非法输入都要收敛,而不是返回野值。
    assert(chick_run_scale_src_index(-1, 40, 3, 2) == 0);
    assert(chick_run_scale_src_index(1000, 40, 3, 2) == 39);
    assert(chick_run_scale_src_index(0, 0, 3, 2) == 0);
    assert(chick_run_scale_src_index(5, 40, 0, 0) == 5);     // 非法比例:恒等映射
    assert(chick_run_scale_src_index(999, 40, 0, 0) == 39);
}

static void test_center(void) {
    assert(chick_run_center_offset(240, 60) == 90);
    assert(chick_run_center_offset(240, 240) == 0);
    assert(chick_run_center_offset(240, 300) == 0);   // 装不下就不偏移
    assert(chick_run_center_offset(180, 111) == 34);  // 奇数差向下取整
    assert(chick_run_center_offset(0, 10) == 0);
}

int main(void) {
    test_extent();
    test_src_index();
    test_center();
    printf("chick run layout tests: PASS\n");
    return 0;
}
