// main/chick_run_sprites.h —— 小鸡快跑的像素美术数据(不含 LVGL / ESP-IDF)。
//
// 数据是"字符 = 像素下标"的 ASCII 图,渲染成 LVGL 画布的工作在 chick_run_ui.c。
// 把纯数据单独放一层,主机测试就能校验宽度、字符合法性,也能在电脑上先出预览图。
#pragma once

#include <stdint.h>

#define CHICK_SPRITE_PALETTE_SIZE 16

// 美术尺寸契约(与 tests/test_chick_run_sprites.c 的断言一致):
// 首页要把仙人掌按 HERO_SCALE 放大画到固定大小的画布上,画布尺寸必须在
// 编译期可知,所以这几个尺寸不能只藏在数据里。
#define CHICK_CACTUS_ART_W 20
#define CHICK_CACTUS_ART_H 40
// 命数爱心:UI 按 3 个一排画在一个固定画布里,尺寸同样要在编译期可知。
#define CHICK_HEART_ART_W  14
#define CHICK_HEART_ART_H  12
// 收集道具:美术尺寸 == 碰撞盒(与障碍同一约定),所以尺寸必须与
// main/chick_run_model.c 的道具尺寸表一一相等(tests/test_chick_run_sprites.c 核对)。
#define CHICK_COIN_ART_W   16
#define CHICK_COIN_ART_H   16
#define CHICK_GEM_ART_W    16
#define CHICK_GEM_ART_H    16
#define CHICK_NOTE_ART_W   20
#define CHICK_NOTE_ART_H   20
// 加速护身符:和金币/宝石/音符一样是"所见即所吃"的碰撞盒。
#define CHICK_BOOST_ART_W  20
#define CHICK_BOOST_ART_H  20
// 光圈:只用于渲染,不是碰撞盒。UI 按它居中画在小鸡背后(两帧一大一小)。
#define CHICK_AURA_ART_W   52
#define CHICK_AURA_ART_H   56

// 每个精灵:宽、高、指向 h 行字符串的指针(每行恰好 w 个字符)。
typedef struct {
    uint8_t w;
    uint8_t h;
    const char *const *rows;
} chick_sprite_t;

typedef enum {
    CHICK_SPRITE_CHICK_RUN_A = 0,  // 跑动第一帧
    CHICK_SPRITE_CHICK_RUN_B,      // 跑动第二帧(换腿)
    CHICK_SPRITE_CHICK_JUMP,       // 起跳(收腿)
    CHICK_SPRITE_CHICK_DUCK,       // 滑铲
    CHICK_SPRITE_FENCE,            // 障碍:木栅栏
    CHICK_SPRITE_CACTUS,           // 障碍:仙人掌
    CHICK_SPRITE_FOX,              // 障碍:小狐狸
    CHICK_SPRITE_COIN,             // 道具:金币(+10 分)
    CHICK_SPRITE_GEM,              // 道具:宝石(+30 分)
    CHICK_SPRITE_NOTE,             // 道具:发光的音符(+10 分,并播放《欢乐颂》)
    CHICK_SPRITE_NOTE_GLOW,        // 道具:音符的高亮帧(两帧交替 = 一闪一闪的发光动效)
    CHICK_SPRITE_BOOST,            // 道具:加速护身符(闪电,吃下后加速 + 无敌)
    CHICK_SPRITE_AURA_A,           // 加速状态:小鸡身上的光环(大)
    CHICK_SPRITE_AURA_B,           // 加速状态:小鸡身上的光环(小 + 四角星光)
    CHICK_SPRITE_TREE,             // 背景:树
    CHICK_SPRITE_TREE_DARK,        // 背景:深色树
    CHICK_SPRITE_FLOWER_RED,       // 背景:红花
    CHICK_SPRITE_FLOWER_WHITE,     // 背景:白花
    CHICK_SPRITE_FLOWER_YELLOW,    // 背景:黄花
    CHICK_SPRITE_TUFT,             // 近景:草丛
    CHICK_SPRITE_CLOUD_BIG,        // 天空:大云
    CHICK_SPRITE_CLOUD_SMALL,      // 天空:小云
    CHICK_SPRITE_MOUNTAINS,        // 远景:远山剪影(240x44)
    CHICK_SPRITE_HEART_FULL,       // 关卡模式命数:实心爱心
    CHICK_SPRITE_HEART_EMPTY,      // 关卡模式命数:空心爱心(已失去)
    CHICK_SPRITE_COUNT,
} chick_sprite_id_t;

// 调色板,RGB888。下标 0 固定为透明。
extern const uint32_t chick_run_palette[CHICK_SPRITE_PALETTE_SIZE];

// 取精灵数据;下标非法返回 NULL。
const chick_sprite_t *chick_run_sprite(chick_sprite_id_t id);

// 字符 -> 调色板下标('0'..'9'、'A'..'F','.' 表示透明)。非法字符返回 -1。
int chick_run_sprite_index(char glyph);

// 校验一个精灵的所有行:长度是否等于 w、字符是否合法。
// 全部合法返回 -1,否则返回第一个出错的行号(主机测试使用)。
int chick_run_sprite_invalid_row(const chick_sprite_t *sprite);
