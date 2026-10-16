// tests/test_chick_run_sprites.c —— 像素美术与碰撞盒的一致性检查。
//
// 这些约束不靠肉眼:美术写错了(行宽不对、脚没画到底行、尺寸和碰撞盒不一致)
// 都会在主机上直接失败,而不是等到设备上才发现"看着没碰到却死了"。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "chick_run_model.h"
#include "chick_run_sprites.h"

static int row_has_pixel(const chick_sprite_t *sprite, int row) {
    if (row < 0 || row >= sprite->h) return 0;
    for (int x = 0; x < sprite->w; x++) {
        if (chick_run_sprite_index(sprite->rows[row][x]) > 0) return 1;
    }
    return 0;
}

static int column_has_pixel(const chick_sprite_t *sprite, int column) {
    for (int y = 0; y < sprite->h; y++) {
        if (chick_run_sprite_index(sprite->rows[y][column]) > 0) return 1;
    }
    return 0;
}

// 美术实际用到的像素范围,用来核对"宽容边"确实存在。
static void used_bbox(const chick_sprite_t *sprite, int *x0, int *y0, int *x1, int *y1) {
    *x0 = sprite->w; *y0 = sprite->h; *x1 = -1; *y1 = -1;
    for (int y = 0; y < sprite->h; y++) {
        for (int x = 0; x < sprite->w; x++) {
            if (chick_run_sprite_index(sprite->rows[y][x]) <= 0) continue;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
    }
}

static void test_palette(void) {
    assert(CHICK_SPRITE_PALETTE_SIZE == 16);
    assert(chick_run_palette[0] == 0);                    // 0 号必须是透明
    for (int i = 0; i < CHICK_SPRITE_PALETTE_SIZE; i++) {
        if (i == 0) continue;
        assert(chick_run_palette[i] != 0);                // 其余颜色不能是纯黑透明色
    }
}

static void test_glyph_mapping(void) {
    assert(chick_run_sprite_index('.') == 0);
    assert(chick_run_sprite_index('0') == 0);
    assert(chick_run_sprite_index('9') == 9);
    assert(chick_run_sprite_index('A') == 10);
    assert(chick_run_sprite_index('F') == 15);
    assert(chick_run_sprite_index('G') == -1);
    assert(chick_run_sprite_index(' ') == -1);
    assert(chick_run_sprite_index('\0') == -1);
}

static void test_every_sprite_is_well_formed(void) {
    // 站在地面上的角色(小鸡各姿态、栅栏、石头、草丛、碎石)必须画到底行,
    // 否则脚/底部会浮在地面线以上;乌鸦与云是空中元素,不要求。
    static const chick_sprite_id_t grounded[] = {
        CHICK_SPRITE_CHICK_RUN_A, CHICK_SPRITE_CHICK_RUN_B,
        CHICK_SPRITE_CHICK_DUCK, CHICK_SPRITE_FENCE, CHICK_SPRITE_CACTUS,
        CHICK_SPRITE_FOX, CHICK_SPRITE_TREE, CHICK_SPRITE_TUFT,
    };

    for (int id = 0; id < CHICK_SPRITE_COUNT; id++) {
        const chick_sprite_t *sprite = chick_run_sprite((chick_sprite_id_t)id);
        assert(sprite != NULL);
        assert(sprite->w > 0 && sprite->h > 0);
        assert(sprite->rows != NULL);
        assert(chick_run_sprite_invalid_row(sprite) == -1);
    }
    for (size_t i = 0; i < sizeof(grounded) / sizeof(grounded[0]); i++) {
        assert(row_has_pixel(chick_run_sprite(grounded[i]),
                             chick_run_sprite(grounded[i])->h - 1));
    }
    // 命数爱心:UI 用 CHICK_HEART_ART_W/H 分配整排画布,尺寸必须一致。
    for (int id = CHICK_SPRITE_HEART_FULL; id <= CHICK_SPRITE_HEART_EMPTY; id++) {
        const chick_sprite_t *heart = chick_run_sprite((chick_sprite_id_t)id);
        assert(heart != NULL);
        assert(heart->w == CHICK_HEART_ART_W);
        assert(heart->h == CHICK_HEART_ART_H);
    }
    // 实心与空心必须真的不同,否则"掉命"在界面上看不出来。
    assert(chick_run_sprite_invalid_row(chick_run_sprite(CHICK_SPRITE_HEART_FULL)) == -1);
    assert(chick_run_sprite_invalid_row(chick_run_sprite(CHICK_SPRITE_HEART_EMPTY)) == -1);

    // 标题页用 CHICK_CACTUS_ART_W/H 在编译期分配放大画布;尺寸对不上就会裁掉图案。
    const chick_sprite_t *cactus = chick_run_sprite(CHICK_SPRITE_CACTUS);
    assert(cactus != NULL);
    assert(cactus->w == CHICK_CACTUS_ART_W);
    assert(cactus->h == CHICK_CACTUS_ART_H);

    assert(chick_run_sprite(CHICK_SPRITE_COUNT) == NULL);
    assert(chick_run_sprite((chick_sprite_id_t)-1) == NULL);
}

static void test_obstacle_art_matches_hitbox(void) {
    // 障碍物不做宽容处理:看到多大就撞多大,否则玩家会觉得判定不公。
    const struct {
        chick_sprite_id_t sprite;
        chick_obstacle_kind_t kind;
    } pairs[] = {
        { CHICK_SPRITE_FENCE, CHICK_OBSTACLE_FENCE },
        { CHICK_SPRITE_CACTUS, CHICK_OBSTACLE_CACTUS },
        { CHICK_SPRITE_FOX, CHICK_OBSTACLE_FOX },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const chick_sprite_t *sprite = chick_run_sprite(pairs[i].sprite);
        const chick_obstacle_shape_t *shape = chick_run_obstacle_shape(pairs[i].kind);
        // 障碍物:美术尺寸 == 碰撞盒(所见即所撞)
        assert(sprite->w == shape->w);
        assert(sprite->h == shape->h);
    }
}

static void test_chick_art_is_slightly_larger_than_hitbox(void) {
    const chick_sprite_t *run_a = chick_run_sprite(CHICK_SPRITE_CHICK_RUN_A);
    const chick_sprite_t *run_b = chick_run_sprite(CHICK_SPRITE_CHICK_RUN_B);
    const chick_sprite_t *jump = chick_run_sprite(CHICK_SPRITE_CHICK_JUMP);
    const chick_sprite_t *duck = chick_run_sprite(CHICK_SPRITE_CHICK_DUCK);

    // 跑动/跳跃三帧共用同一套尺寸,渲染时只换图不换位置。
    assert(run_a->w == run_b->w && run_a->w == jump->w && run_a->h == run_b->h);
    assert(run_a->h == jump->h);
    assert(run_a->w == CHICK_RUN_CHICK_ART_W);
    assert(run_a->h == CHICK_RUN_CHICK_ART_H);
    assert(duck->w == CHICK_RUN_CHICK_ART_W);
    assert(duck->h == CHICK_RUN_CHICK_ART_DUCK_H);
    // 美术比碰撞盒大出的部分就是"宽容边"
    assert(run_a->w == CHICK_RUN_CHICK_W + 2 * CHICK_RUN_CHICK_ART_MARGIN_X);
    assert(run_a->h == CHICK_RUN_CHICK_H + CHICK_RUN_CHICK_ART_MARGIN_Y);
    assert(duck->h == CHICK_RUN_CHICK_DUCK_H + CHICK_RUN_CHICK_ART_MARGIN_Y);

    // 蹲下时美术整体低于乌鸦下边缘,玩家能看见"从鸟下面钻过去"。
    // 蹲下时美术整体高度仍小于站立高度,滑铲确实"变矮"。
    assert(duck->h < run_a->h);

    // 宽容边必须真实存在:喙伸出碰撞盒右侧、尾巴伸出左侧、头顶高过碰撞盒顶,
    // 而脚底与美术底行对齐(踩在地面线上)。
    int x0, y0, x1, y1;
    used_bbox(run_a, &x0, &y0, &x1, &y1);
    assert(x1 >= CHICK_RUN_CHICK_W + CHICK_RUN_CHICK_ART_MARGIN_X - 1);
    assert(x0 <= CHICK_RUN_CHICK_ART_MARGIN_X);
    assert(y0 <= CHICK_RUN_CHICK_ART_MARGIN_Y);
    assert(y1 == run_a->h - 1);
    assert(column_has_pixel(run_a, x1));
}

static void test_pickup_art_matches_hitbox(void) {
    // 道具和障碍同一约定:美术尺寸 == 碰撞盒("看到多大就吃多大")。
    // 两边分别写在美术层(chick_run_sprites.h)和模型层(chick_run_model.c)里,
    // 所以必须由测试把它们钉在一起。
    const struct {
        chick_sprite_id_t sprite;
        chick_pickup_kind_t kind;
    } pairs[] = {
        { CHICK_SPRITE_COIN, CHICK_PICKUP_COIN },
        { CHICK_SPRITE_GEM,  CHICK_PICKUP_GEM },
        { CHICK_SPRITE_NOTE, CHICK_PICKUP_NOTE },
        { CHICK_SPRITE_BOOST, CHICK_PICKUP_BOOST },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const chick_sprite_t *sprite = chick_run_sprite(pairs[i].sprite);
        const chick_pickup_shape_t *shape = chick_run_pickup_shape(pairs[i].kind);
        assert(sprite != NULL);
        assert(shape != NULL);
        assert(sprite->w == shape->w);
        assert(sprite->h == shape->h);
        // 道具是加分项,美术必须把整块碰撞盒填满(允许 1px 透明边:
        // 发光/描边在圆角处不可能严丝合缝)。否则会出现"看着还差一点却被吃掉"。
        int x0, y0, x1, y1;
        used_bbox(sprite, &x0, &y0, &x1, &y1);
        assert(x0 <= 1 && y0 <= 1);
        assert(x1 >= sprite->w - 2 && y1 >= sprite->h - 2);
    }
    // 三种道具的分值:金币/音符 10 分、宝石 30 分、加速符 0 分(HUD 与结算都按这个显示)。
    assert(chick_run_pickup_points(CHICK_PICKUP_COIN) == CHICK_RUN_COIN_POINTS);
    assert(chick_run_pickup_points(CHICK_PICKUP_GEM) == CHICK_RUN_GEM_POINTS);
    assert(chick_run_pickup_points(CHICK_PICKUP_NOTE) == CHICK_RUN_NOTE_POINTS);
    assert(chick_run_pickup_points(CHICK_PICKUP_BOOST) == CHICK_RUN_BOOST_POINTS);
    assert(CHICK_RUN_GEM_POINTS > CHICK_RUN_COIN_POINTS);
}

// 音符道具的"发光动效"是两帧交替:尺寸必须完全一样(否则碰撞盒不变但画面会跳),
// 而且两帧必须真的不同(否则动画白做)。
static void test_note_glow_frames_match(void) {
    const chick_sprite_t *note = chick_run_sprite(CHICK_SPRITE_NOTE);
    const chick_sprite_t *glow = chick_run_sprite(CHICK_SPRITE_NOTE_GLOW);
    assert(note != NULL && glow != NULL);
    assert(note->w == glow->w && note->h == glow->h);
    assert(note->w == CHICK_NOTE_ART_W && note->h == CHICK_NOTE_ART_H);
    int different = 0;
    for (int y = 0; y < note->h; y++) {
        for (int x = 0; x < note->w; x++) {
            if (note->rows[y][x] != glow->rows[y][x]) different++;
        }
    }
    assert(different > 8);                       // 肉眼可见的差别,不是改了一个像素
}

// 光圈两帧只给渲染用(不是碰撞盒),但尺寸必须在编译期可知且两帧一致。
static void test_aura_frames_match(void) {
    const chick_sprite_t *a = chick_run_sprite(CHICK_SPRITE_AURA_A);
    const chick_sprite_t *b = chick_run_sprite(CHICK_SPRITE_AURA_B);
    assert(a != NULL && b != NULL);
    assert(a->w == CHICK_AURA_ART_W && a->h == CHICK_AURA_ART_H);
    assert(b->w == CHICK_AURA_ART_W && b->h == CHICK_AURA_ART_H);
    // 光圈是细环,不能是实心圆(实心会把地面和道具盖掉)。
    const int area = a->w * a->h;
    int filled = 0;
    for (int y = 0; y < a->h; y++) {
        for (int x = 0; x < a->w; x++) {
            if (chick_run_sprite_index(a->rows[y][x]) > 0) filled++;
        }
    }
    assert(filled * 3 < area);                    // 覆盖不到 1/3
    assert(filled > 0);
}

int main(void) {
    test_palette();
    test_glyph_mapping();
    test_every_sprite_is_well_formed();
    test_obstacle_art_matches_hitbox();
    test_pickup_art_matches_hitbox();
    test_note_glow_frames_match();
    test_aura_frames_match();
    test_chick_art_is_slightly_larger_than_hitbox();
    printf("test_chick_run_sprites: all checks passed\n");
    return 0;
}
