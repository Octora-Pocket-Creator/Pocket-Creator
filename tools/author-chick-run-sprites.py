#!/usr/bin/env python3
"""Author the Chick Run pixel art and emit main/chick_run_sprites.c.

The sprites are drawn with geometric primitives (ellipses, triangles, rounded
rects) instead of hand-placed ASCII: that keeps silhouettes symmetric and round
at native resolution, and makes the poses reproducible. The generator also
checks the invariants the firmware depends on (row width, every palette glyph
valid, bottom row of a standing pose actually has pixels).

Run from the repository root:

    python3 tools/author-chick-run-sprites.py            # write the C file
    python3 tools/author-chick-run-sprites.py --preview  # also write a PNG sheet
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "main" / "chick_run_sprites.c"

# palette index -> RGB888 (0 is transparent, see chick_run_sprites.h)
PALETTE = {
    0: None,
    1: (0xE3, 0xB8, 0x4B),   # 小鸡描边(柔和深黄,不用黑边)
    2: (0xF5, 0xD7, 0x6E),   # 小鸡身体
    3: (0xE8, 0xC8, 0x4A),   # 小鸡翅膀 / 暗部
    4: (0xF3, 0x9C, 0x12),   # 喙、脚、狐狸头
    5: (0x2C, 0x3E, 0x50),   # 眼睛瞳孔
    6: (0xFF, 0xFF, 0xFF),   # 眼白、云、花瓣
    7: (0xE7, 0x4C, 0x3C),   # 鸡冠、红花、花瓣
    8: (0xF5, 0xA8, 0x8C),   # 腮红
    9: (0x8B, 0x5E, 0x3C),   # 树干 / 狐狸深色
    10: (0x8B, 0x45, 0x13),  # 木栅栏 / 障碍描边
    11: (0x27, 0xAE, 0x60),  # 仙人掌亮绿
    12: (0x1E, 0x84, 0x49),  # 仙人掌暗绿 / 树冠
    13: (0x7C, 0xB3, 0x42),  # 草地亮
    14: (0x5A, 0x9A, 0x2A),  # 草地暗 / 草叶
    15: (0x7C, 0xB8, 0x9A),  # 远山(比天空底色更实的青绿,避免糊成一片)
}
GLYPH = "0123456789ABCDEF"
TRANSPARENT = 0


class Grid:
    """Small indexed-canvas rasterizer with the primitives the art needs."""

    def __init__(self, width: int, height: int):
        self.w = width
        self.h = height
        self.px = [[TRANSPARENT] * width for _ in range(height)]

    def set(self, x: int, y: int, color: int) -> None:
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = color

    def get(self, x: int, y: int) -> int:
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y][x]
        return TRANSPARENT

    def ellipse(self, cx: float, cy: float, rx: float, ry: float, color: int) -> None:
        for y in range(self.h):
            for x in range(self.w):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.set(x, y, color)

    def circle(self, cx: float, cy: float, r: float, color: int) -> None:
        self.ellipse(cx, cy, r, r, color)

    def rect(self, x0: int, y0: int, x1: int, y1: int, color: int) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, color)

    def round_rect(self, x0: float, y0: float, x1: float, y1: float, r: float,
                   color: int) -> None:
        cx0, cx1 = x0 + r, x1 - r
        cy0, cy1 = y0 + r, y1 - r
        for y in range(self.h):
            for x in range(self.w):
                if not (x0 <= x + 0.5 <= x1 + 1 and y0 <= y + 0.5 <= y1 + 1):
                    continue
                px, py = x + 0.5, y + 0.5
                nx = min(max(px, cx0), cx1)
                ny = min(max(py, cy0), cy1)
                if (px - nx) ** 2 + (py - ny) ** 2 <= r * r + 0.25:
                    self.set(x, y, color)

    def polygon(self, points, color: int) -> None:
        """偶数-奇数规则的扫描线填充（用两三角拼闪电会很细，折线形状直接给顶点更清楚）。"""
        count = len(points)
        for y in range(self.h):
            yc = y + 0.5
            crossings = []
            for i in range(count):
                x0, y0 = points[i]
                x1, y1 = points[(i + 1) % count]
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    crossings.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
            crossings.sort()
            for i in range(0, len(crossings) - 1, 2):
                for x in range(self.w):
                    if crossings[i] <= x + 0.5 <= crossings[i + 1]:
                        self.set(x, y, color)

    def triangle(self, a, b, c, color: int) -> None:
        def sign(p, q, r):
            return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])

        for y in range(self.h):
            for x in range(self.w):
                p = (x + 0.5, y + 0.5)
                d1, d2, d3 = sign(p, a, b), sign(p, b, c), sign(p, c, a)
                neg = d1 < 0 or d2 < 0 or d3 < 0
                pos = d1 > 0 or d2 > 0 or d3 > 0
                if not (neg and pos):
                    self.set(x, y, color)

    def arc(self, cx: float, cy: float, r: float, start_deg: float, end_deg: float,
            color: int, width: float = 1.4) -> None:
        """Stroke an arc: used for the neck line and rounded rims."""
        steps = int(abs(end_deg - start_deg) * max(r, 1.0) * 0.6) + 8
        for index in range(steps + 1):
            angle = math.radians(start_deg + (end_deg - start_deg) * index / steps)
            for dr in (0.0, 0.5):
                x = int(round(cx + (r + dr) * math.cos(angle) - 0.5))
                y = int(round(cy + (r + dr) * math.sin(angle) - 0.5))
                self.set(x, y, color)
                if width > 1.0:
                    self.set(x + 1, y, color)

    def ellipse_ring(self, cx: float, cy: float, rx: float, ry: float,
                     color: int) -> None:
        for y in range(self.h):
            for x in range(self.w):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                outer = dx * dx + dy * dy
                if outer > 1.0:
                    continue
                inner_x = (x + 0.5 - cx) / max(rx - 1.4, 0.5)
                inner_y = (y + 0.5 - cy) / max(ry - 1.4, 0.5)
                if inner_x * inner_x + inner_y * inner_y > 1.0:
                    self.set(x, y, color)

    def outline(self, color: int, inside: set[int] | None = None) -> None:
        """Add a 1px border where a filled pixel touches transparency."""
        filled = inside or {index for index in range(1, 16)}
        add = []
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y][x] != TRANSPARENT:
                    continue
                neighbours = (
                    self.get(x - 1, y), self.get(x + 1, y),
                    self.get(x, y - 1), self.get(x, y + 1),
                )
                if any(n in filled for n in neighbours):
                    add.append((x, y))
        for x, y in add:
            self.set(x, y, color)

    def rows(self) -> list[str]:
        return ["".join(GLYPH[self.px[y][x]] for x in range(self.w)) for y in range(self.h)]

    def filled(self, x: int, y: int) -> bool:
        return self.px[y][x] != TRANSPARENT


# ---------------------------------------------------------------------------
# chick: 40x44 native. Feet sit on the last row; the body is a round blob with a
# merged head, one visible eye, an orange beak and two legs.
CHICK_W, CHICK_H = 40, 44


def draw_chick(leg_phase: int, *, height: int = CHICK_H, airborne: bool = False,
               squash: float = 1.0) -> Grid:
    """平面矢量风的小鸡(对齐参考 HTML 的画法):

      * 描边用柔和深黄而不是黑边,整体是"扁平卡通"而不是黑描边像素图;
      * 白眼球 + 深色瞳孔 + 高光,比一颗黑点可爱;
      * 头顶红色鸡冠、脸颊腮红 —— 鸡的身份特征;
      * 粗一点的腿 + 椭圆脚掌,跑动两帧左右摆腿。

    尺寸固定为 40x44(蹲下 40x24):碰撞盒与"宽容边"依赖它,改尺寸要同步改模型常量。
    """
    grid = Grid(CHICK_W, height)
    ground = float(height - 1)

    body_rx = 13.0
    body_ry = 11.5 * squash
    body_cy = ground - 3.0 - body_ry
    body_cx = 16.0
    head_r = 10.0 * (0.85 + 0.15 * squash)
    head_cx = body_cx + 10.0
    head_cy = max(body_cy - 13.0 * squash, head_r + 1.5)

    # 尾羽(在身体后面)
    grid.triangle((body_cx - 16.0, body_cy - 6.0 * squash),
                  (body_cx - 7.0, body_cy + 1.0),
                  (body_cx - 15.0, body_cy + 7.0 * squash), 2)
    grid.ellipse(body_cx, body_cy, body_rx, body_ry, 2)
    grid.circle(head_cx, head_cy, head_r, 2)
    grid.outline(1)
    grid.arc(head_cx, head_cy, head_r, 108.0, 248.0, 1)          # 颈线

    # 翅膀:带描边的椭圆
    wing_cy = body_cy + (0.0 if not airborne else -3.0 * squash)
    grid.ellipse(10.5, wing_cy, 6.2, 4.8 * squash, 3)
    grid.ellipse_ring(10.5, wing_cy, 6.2, 4.8 * squash, 1)

    # 白眼球 + 瞳孔 + 高光
    eye_x, eye_y = head_cx + 3.5, head_cy - 1.5
    grid.ellipse(eye_x, eye_y, 3.0, 3.6, 6)
    grid.ellipse(eye_x + 0.4, eye_y + 0.2, 1.7, 2.2, 5)
    grid.rect(int(eye_x) - 1, int(eye_y) - 2, int(eye_x) - 1, int(eye_y) - 1, 6)

    # 腮红
    grid.ellipse(head_cx - 1.5, head_cy + 5.0 * squash, 2.6, 1.8, 8)

    # 鸡冠(锯齿状,红)
    crown_y = head_cy - head_r
    for index, dx in enumerate((1.0, 4.0, 7.0)):
        grid.triangle((head_cx - 3.0 + dx, crown_y + 3.0),
                      (head_cx - 1.5 + dx, crown_y - 3.0 + (1.5 if index == 1 else 0.0)),
                      (head_cx + 1.0 + dx, crown_y + 3.0), 7)

    # 喙:小三角
    beak_x = head_cx + head_r - 2.0
    grid.triangle((beak_x, eye_y + 2.0), (beak_x + 5.5, eye_y + 4.0),
                  (beak_x, eye_y + 6.0), 4)

    # 腿与脚掌
    top = int(ground) - 6
    bottom = int(ground)
    if airborne:
        grid.rect(14, top - 1, 16, bottom - 3, 4)
        grid.rect(20, top - 1, 22, bottom - 3, 4)
        grid.ellipse(15.0, float(bottom) - 1.5, 2.6, 1.6, 4)
        grid.ellipse(21.0, float(bottom) - 1.5, 2.6, 1.6, 4)
    elif leg_phase == 0:
        grid.rect(13, top, 15, bottom - 1, 4)
        grid.rect(21, top - 1, 23, bottom - 1, 4)
        grid.ellipse(13.0, float(bottom), 3.2, 1.8, 4)
        grid.ellipse(23.0, float(bottom), 3.2, 1.8, 4)
    else:
        grid.rect(15, top, 17, bottom - 1, 4)
        grid.rect(19, top - 2, 21, bottom - 1, 4)
        grid.ellipse(15.0, float(bottom), 3.2, 1.8, 4)
        grid.ellipse(21.0, float(bottom), 3.2, 1.8, 4)
    return grid


# ---------------------------------------------------------------------------
# 障碍物(全部靠跳跃通过,没有必须下蹲的):栅栏 / 仙人掌 / 狐狸
# 美术尺寸 == 碰撞盒,宽度受"跳跃时间窗"约束(见 tests/test_chick_run_model.c)。
FENCE_W, FENCE_H = 20, 32


def draw_fence() -> Grid:
    grid = Grid(FENCE_W, FENCE_H)
    # 两根立柱 + 三道横杆 + 三角形尖顶(参考 HTML 的栅栏)
    for x0 in (1, 14):
        grid.rect(x0, 4, x0 + 4, FENCE_H - 1, 10)
    for y0 in (10, 19, 28):
        grid.rect(0, y0, FENCE_W - 1, y0 + 4, 10)
    grid.triangle((0.0, 6.0), (FENCE_W / 2.0, -2.0), (float(FENCE_W), 6.0), 9)
    grid.rect(0, 6, FENCE_W - 1, 8, 9)
    grid.outline(10, inside={9, 10})
    # 横杆的高光,避免整块死板
    for y0 in (10, 19, 28):
        grid.rect(0, y0, FENCE_W - 1, y0, 9)
    return grid


CACTUS_W, CACTUS_H = 20, 40


def draw_cactus() -> Grid:
    grid = Grid(CACTUS_W, CACTUS_H)
    grid.round_rect(7.0, 4.0, 13.0, 39.0, 3.0, 11)               # 主干
    for side, sx in ((-1, 1.0), (1, 12.0)):                       # 侧枝
        grid.round_rect(sx, 16.0, sx + 6.0, 20.0, 2.0, 11)
        grid.round_rect(sx if side < 0 else sx + 4.0, 12.0,
                        (sx + 4.0) if side < 0 else sx + 8.0, 22.0, 2.0, 11)
    grid.outline(10, inside={11, 12})
    for y in range(12, 38, 6):                                    # 刺
        grid.rect(5, y, 6, y, 12)
        grid.rect(13, y + 3, 14, y + 3, 12)
    grid.ellipse(10.0, 3.0, 3.2, 3.0, 7)                          # 顶部红花
    grid.rect(10, 3, 10, 3, 2)
    return grid


FOX_W, FOX_H = 26, 26


def draw_fox() -> Grid:
    grid = Grid(FOX_W, FOX_H)
    grid.ellipse(15.0, 20.0, 10.5, 6.0, 9)                        # 身体
    grid.triangle((2.0, 12.0), (9.0, 24.0), (10.0, 14.0), 9)      # 尾巴
    grid.ellipse(18.0, 13.0, 7.5, 7.0, 4)                         # 头
    grid.triangle((13.0, 8.0), (12.0, 0.0), (17.0, 6.0), 9)       # 耳朵
    grid.triangle((21.0, 7.0), (24.0, 0.0), (25.0, 8.0), 9)
    grid.outline(10, inside={4, 9})
    grid.ellipse(21.0, 12.0, 1.8, 2.0, 5)                         # 眼睛
    grid.ellipse(23.5, 15.5, 2.0, 1.6, 6)                         # 嘴部白斑
    grid.rect(0, FOX_H - 1, 5, FOX_H - 1, 4)                      # 脚
    grid.rect(20, FOX_H - 1, 25, FOX_H - 1, 4)
    return grid


# ---------------------------------------------------------------------------
# 收集道具:美术尺寸 == 碰撞盒(和障碍同一约定),全部放在空中,必须跳起来拿。
# 尺寸同时写死在 main/chick_run_sprites.h 的 CHICK_*_ART_W/H 里(模型要用),
# tests/test_chick_run_sprites.c 会核对两边一致。
COIN_W, COIN_H = 16, 16


def draw_coin() -> Grid:
    """金币:橙金外圈 + 浅金面 + 内环,左上角一点白高光。

    16px 里画不下文字/图案,靠"外圈亮、面暗、内环亮"的三层同心圆读出"硬币"。
    """
    grid = Grid(COIN_W, COIN_H)
    grid.circle(8.0, 8.0, 7.0, 4)          # 外圈
    grid.circle(8.0, 8.0, 5.6, 2)          # 币面
    grid.circle(8.0, 8.0, 4.2, 4)          # 内环
    grid.circle(8.0, 8.0, 2.4, 2)          # 中心
    grid.ellipse(5.4, 5.2, 1.3, 1.0, 6)    # 高光
    grid.outline(9, inside={4, 2, 6})      # 深棕描边:压在草地上也能看清
    return grid


GEM_W, GEM_H = 16, 16


def draw_gem() -> Grid:
    """宝石:菱形,上半亮青、下半暗绿,左上一点白色星光。

    颜色刻意避开金币的暖金:道具看颜色就能区分,不用看形状。
    """
    grid = Grid(GEM_W, GEM_H)
    grid.triangle((8.0, 0.5), (15.0, 6.5), (1.0, 6.5), 15)    # 上半(受光面)
    grid.triangle((1.0, 6.5), (15.0, 6.5), (8.0, 15.5), 12)   # 下半(背光面)
    grid.triangle((4.0, 2.5), (8.0, 0.5), (8.0, 6.5), 6)      # 左侧切面高光
    grid.rect(11, 2, 11, 3, 6)                                # 星光
    grid.rect(10, 3, 12, 3, 6)
    grid.outline(5, inside={15, 12, 6})
    return grid


NOTE_W, NOTE_H = 20, 20


# ---------------------------------------------------------------------------
# 加速护身符(道具)与光圈(小鸡身上的光环)
BOOST_W, BOOST_H = 20, 20


def draw_boost() -> Grid:
    """加速护身符:红色的闪电 + 白色光晕。

    闪电是"速度"最通用的符号,颜色刻意用红(宝石是青、金币/音符是金),
    一眼就能和另外三种道具区分。
    """
    grid = Grid(BOOST_W, BOOST_H)
    # 折线闪电:上半向左下斜、下半向右下斜,中段错开就是"之"字。
    grid.polygon([(12.0, 0.5), (1.5, 11.5), (7.5, 11.5),
                  (6.5, 19.5), (18.5, 8.5), (10.5, 8.5)], 7)
    grid.polygon([(11.5, 3.0), (6.0, 10.0), (9.5, 10.0)], 8)      # 高光面
    grid.outline(6, inside={7, 8, 6})                             # 白色光晕
    return grid


# 光圈:比小鸡美术(40x44)大一圈,画在小鸡背后。两帧交替就是"呼吸"的脉冲感。
AURA_W, AURA_H = 52, 56


def draw_aura(tight: bool) -> Grid:
    """小鸡身上的光环:白内圈 + 浅金外圈,两帧一大一小。

    只画细环(不是实心圆),否则会盖住地面和道具;两帧交替就有时明时暗的脉冲感。
    小的一帧(实参 True)额外加四角星光,脉冲更明显。
    """
    grid = Grid(AURA_W, AURA_H)
    cx, cy = AURA_W / 2.0, AURA_H / 2.0
    if tight:
        grid.ellipse_ring(cx, cy, 22.0, 24.5, 2)     # 外圈:浅金
        grid.ellipse_ring(cx, cy, 20.5, 23.0, 6)     # 内圈:白
        for ox, oy in ((2, 6), (49, 6), (2, 49), (49, 49)):   # 四角星光
            grid.set(ox, oy, 6)
    else:
        grid.ellipse_ring(cx, cy, 25.0, 27.5, 2)
        grid.ellipse_ring(cx, cy, 23.5, 26.0, 6)
    return grid


def draw_note(glow: bool = False) -> Grid:
    """音符道具(发光的 🎵):八分音符 + 光晕 + 星光。

    "发光"在 1 位掩码的精灵引擎里就是"外面再套一圈更亮的颜色":
    先是浅金(2),再是白(6),于是轮廓看着像在发亮。
    glow=True 的那一帧把光晕直接换成白色并多几点星光 —— 两帧交替就是"一闪一闪"。
    """
    grid = Grid(NOTE_W, NOTE_H)
    grid.ellipse(7.5, 15.0, 4.4, 3.6, 4)                        # 符头
    grid.rect(11, 3, 12, 15, 4)                                 # 符干
    grid.triangle((12.0, 3.0), (18.0, 8.0), (12.0, 12.0), 4)    # 符尾
    grid.ellipse(6.6, 14.2, 1.6, 1.1, 6)                        # 符头高光
    if glow:
        grid.outline(6, inside={4, 2, 6})                       # 整圈白光
        for ox, oy in ((2, 3), (16, 2), (3, 16), (17, 15)):     # 四点星光
            grid.set(ox, oy, 6)
        return grid
    grid.outline(2, inside={4, 2, 6})                           # 内圈光晕(浅金)
    grid.outline(6, inside={4, 2, 6})                           # 外圈光晕(白)
    grid.set(2, 3, 6)                                           # 星光
    grid.set(3, 3, 6)
    grid.set(2, 4, 6)
    return grid


# ---------------------------------------------------------------------------
# scenery props
TUFT_W, TUFT_H = 24, 16


def draw_tuft() -> Grid:
    grid = Grid(TUFT_W, TUFT_H)
    for x0, top, color in ((2, 5, 14), (7, 1, 13), (12, 4, 14), (17, 7, 13)):
        grid.rect(x0, top, x0 + 1, TUFT_H - 1, color)
    grid.rect(0, TUFT_H - 2, TUFT_W - 1, TUFT_H - 1, 14)
    return grid


TREE_W, TREE_H = 28, 48


def draw_tree(kind: int = 0) -> Grid:
    """背景树:树干 + 三个圆冠(参考 HTML 的画法)。"""
    grid = Grid(TREE_W, TREE_H)
    grid.rect(9, 24, 18, TREE_H - 1, 9)                 # 加宽的树干
    canopy = 12 if kind == 0 else 11
    highlight = 11 if kind == 0 else 13
    grid.ellipse(14.0, 15.0, 12.0, 11.0, canopy)
    grid.ellipse(6.0, 25.0, 7.5, 8.0, canopy)
    grid.ellipse(21.0, 25.0, 7.5, 8.0, canopy)
    grid.ellipse(10.0, 10.0, 5.0, 4.0, highlight)        # 树冠亮部
    grid.ellipse(19.0, 13.0, 3.5, 3.0, highlight)
    grid.outline(9, inside={canopy, highlight})
    return grid


FLOWER_W, FLOWER_H = 16, 20


def draw_flower(kind: int = 0) -> Grid:
    grid = Grid(FLOWER_W, FLOWER_H)
    grid.rect(7, 8, 8, FLOWER_H - 1, 14)                          # 花茎
    petals = (7, 6, 2)[kind % 3]
    for angle in range(5):
        radians = math.radians(angle * 72.0 - 90.0)
        grid.ellipse(8.0 + 3.6 * math.cos(radians), 7.0 + 3.6 * math.sin(radians),
                     3.0, 3.0, petals)
    grid.ellipse(8.0, 7.0, 2.0, 2.0, 4)                           # 花心
    return grid


CLOUD_W, CLOUD_H = 52, 22


def draw_cloud(big: bool = True) -> Grid:
    grid = Grid(CLOUD_W, CLOUD_H)
    if big:
        grid.ellipse(18.0, 13.0, 14.0, 8.0, 6)
        grid.ellipse(32.0, 10.0, 12.0, 9.0, 6)
        grid.ellipse(43.0, 14.0, 8.0, 6.0, 6)
    else:
        grid.ellipse(16.0, 13.0, 11.0, 7.0, 6)
        grid.ellipse(30.0, 11.0, 9.0, 8.0, 6)
    return grid


MOUNTAIN_W, MOUNTAIN_H = 240, 44


def draw_mountains() -> Grid:
    """远山:正弦剪影(对齐 HTML 的远山画法),静态铺在天空下方。"""
    grid = Grid(MOUNTAIN_W, MOUNTAIN_H)
    for x in range(MOUNTAIN_W):
        height = (26.0
                  + math.sin(x * 0.008 * MOUNTAIN_W / 240.0 * 8.0 + 1.0) * 12.0
                  + math.sin(x * 0.015 * MOUNTAIN_W / 240.0 * 8.0) * 7.0)
        top = max(0, int(MOUNTAIN_H - height))
        for y in range(top, MOUNTAIN_H):
            grid.set(x, y, 15)
    return grid


# ---------------------------------------------------------------------------
# 命数图标：14x12 的心形（HUD 尺寸很小，实心红心 + 空心红边两种状态）。
# 由两个圆和一个倒三角拼出来，不做描边 —— 12px 高再加边会糊成一团。
HEART_W, HEART_H = 14, 12


def draw_heart(filled: bool = True) -> Grid:
    g = Grid(HEART_W, HEART_H)
    g.circle(4.4, 4.2, 3.7, 7)
    g.circle(9.6, 4.2, 3.7, 7)
    g.triangle((0.4, 4.8), (13.6, 4.8), (7.0, 11.5), 7)
    if filled:
        g.set(3, 2, 6)          # 左上高光
        g.set(4, 2, 6)
        g.set(3, 3, 6)
        return g

    # 空心：把内部再挖一次，只留约 2px 的边，露出背景色（这里用透明）。
    inner = Grid(HEART_W, HEART_H)
    inner.circle(4.4, 4.2, 2.2, 7)
    inner.circle(9.6, 4.2, 2.2, 7)
    inner.triangle((2.4, 5.6), (11.6, 5.6), (7.0, 9.6), 7)
    for y in range(HEART_H):
        for x in range(HEART_W):
            if inner.get(x, y):
                g.set(x, y, TRANSPARENT)
    return g


SPRITES: list[tuple[str, Grid]] = []


def build() -> None:
    SPRITES.clear()
    SPRITES.extend([
        ("CHICK_RUN_A", draw_chick(0)),
        ("CHICK_RUN_B", draw_chick(1)),
        ("CHICK_JUMP", draw_chick(0, airborne=True)),
        ("CHICK_DUCK", draw_chick(0, height=24, squash=0.62)),
        ("FENCE", draw_fence()),
        ("CACTUS", draw_cactus()),
        ("FOX", draw_fox()),
        ("COIN", draw_coin()),
        ("GEM", draw_gem()),
        ("NOTE", draw_note()),
        ("NOTE_GLOW", draw_note(glow=True)),
        ("BOOST", draw_boost()),
        ("AURA_A", draw_aura(False)),
        ("AURA_B", draw_aura(True)),
        ("TREE", draw_tree(0)),
        ("TREE_DARK", draw_tree(1)),
        ("FLOWER_RED", draw_flower(0)),
        ("FLOWER_WHITE", draw_flower(1)),
        ("FLOWER_YELLOW", draw_flower(2)),
        ("TUFT", draw_tuft()),
        ("CLOUD_BIG", draw_cloud(True)),
        ("CLOUD_SMALL", draw_cloud(False)),
        ("MOUNTAINS", draw_mountains()),
        ("HEART_FULL", draw_heart(True)),
        ("HEART_EMPTY", draw_heart(False)),
    ])


HEADER = '''// main/chick_run_sprites.c —— 小鸡快跑使用的像素美术(索引色)。
//
// 本文件由 tools/author-chick-run-sprites.py 生成,请勿手改:
// 精灵是用几何图元(椭圆/三角/圆角矩形)在【原生分辨率】上画出来的,
// 再叠一圈描边,所以轮廓对称、圆润,放大到屏幕上是像素放大而不是插值糊块。
//
// 尺寸契约(改动前先看 tests/test_chick_run_sprites.c):
//   * 站立/跑动/跳跃 40x44,蹲下 40x24;碰撞盒比美术小一圈,四周留 4px"宽容边";
//   * 障碍物 美术尺寸 == 碰撞盒 —— 所见即所撞,且全部靠跳跃通过;
//   * 站立姿态最底行必须有像素,否则脚会浮在地面线以上。
//
// 画风对齐参考实现(小鸡快跑2D.html):扁平矢量感、柔和描边、鸡冠/腮红/白眼球,
// 障碍为栅栏 / 仙人掌 / 狐狸,背景为远山 + 树 + 小花 + 草地。
#include "chick_run_sprites.h"

#include <stddef.h>

const uint32_t chick_run_palette[CHICK_SPRITE_PALETTE_SIZE] = {
'''

PALETTE_COMMENTS = [
    "透明", "描边(深棕)", "小鸡身体", "小鸡暗部/翅膀", "喙 / 脚(橙)",
    "眼睛", "高光 / 云", "乌鸦身体", "乌鸦暗部", "木头",
    "木头暗部", "石头", "石头暗部", "草叶", "草叶暗部", "云朵暗部",
]

TAIL = '''
const chick_sprite_t *chick_run_sprite(chick_sprite_id_t id) {
    if (id < 0 || id >= CHICK_SPRITE_COUNT) return NULL;
    return &CHICK_RUN_SPRITES[id];
}

int chick_run_sprite_index(char glyph) {
    if (glyph == '.') return 0;
    if (glyph >= '0' && glyph <= '9') return glyph - '0';
    if (glyph >= 'A' && glyph <= 'F') return glyph - 'A' + 10;
    return -1;
}

int chick_run_sprite_invalid_row(const chick_sprite_t *sprite) {
    if (sprite == NULL || sprite->rows == NULL || sprite->w == 0 || sprite->h == 0) return 0;
    for (int y = 0; y < sprite->h; y++) {
        const char *row = sprite->rows[y];
        if (row == NULL) return y;
        int x = 0;
        for (; x < sprite->w; x++) {
            if (row[x] == '\\0') break;
            if (chick_run_sprite_index(row[x]) < 0) return y;
        }
        if (x != sprite->w || row[x] != '\\0') return y;
    }
    return -1;
}
'''


def write_c(path: Path) -> None:
    out = [HEADER]
    for index in range(16):
        rgb = PALETTE[index]
        value = 0 if rgb is None else (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]
        out.append(f"    0x{value:06X},  /* {index} {PALETTE_COMMENTS[index]} */\n")
    out.append("};\n\n")

    for name, grid in SPRITES:
        out.append(f"static const char *const SPRITE_ROWS_{name}[{grid.h}] = {{\n")
        for row in grid.rows():
            out.append(f'    "{row}",\n')
        out.append("};\n\n")

    out.append("static const chick_sprite_t CHICK_RUN_SPRITES[CHICK_SPRITE_COUNT] = {\n")
    for name, grid in SPRITES:
        out.append(f"    [CHICK_SPRITE_{name}] = "
                   f"{{ .w = {grid.w}, .h = {grid.h}, .rows = SPRITE_ROWS_{name} }},\n")
    out.append("};\n")
    out.append(TAIL)
    path.write_text("".join(out), encoding="utf-8")
    print(f"written: {path.relative_to(ROOT)}")


def write_preview(path: Path) -> None:
    from PIL import Image, ImageDraw

    scale, pad, label_h, cols = 4, 6, 12, 4
    cell_w = max(grid.w for _, grid in SPRITES) * scale + pad
    cell_h = max(grid.h for _, grid in SPRITES) * scale + pad + label_h
    rows = (len(SPRITES) + cols - 1) // cols
    image = Image.new("RGB", (cols * cell_w, rows * cell_h), (0x10, 0x14, 0x18))
    draw = ImageDraw.Draw(image)
    for index, (name, grid) in enumerate(SPRITES):
        ox = (index % cols) * cell_w + pad // 2
        oy = (index // cols) * cell_h + label_h
        draw.text((ox, oy - label_h + 1), f"{name} {grid.w}x{grid.h}",
                  fill=(0xE0, 0xE6, 0xEC))
        for y in range(grid.h):
            for x in range(grid.w):
                rgb = PALETTE[grid.get(x, y)]
                if rgb is None:
                    continue
                draw.rectangle([ox + x * scale, oy + y * scale,
                                ox + (x + 1) * scale - 1, oy + (y + 1) * scale - 1],
                               fill=rgb)
    image.save(path)
    print(f"preview written: {path}")


def report() -> None:
    for name, grid in SPRITES:
        art = grid.rows()
        used = [(x, y) for y in range(grid.h) for x in range(grid.w) if grid.filled(x, y)]
        ys = [y for _, y in used]
        xs = [x for x, _ in used]
        bottom = any(grid.filled(x, grid.h - 1) for x in range(grid.w))
        print(f"{name:14s} {grid.w:2d}x{grid.h:2d} bbox x={min(xs)}..{max(xs)} "
              f"y={min(ys)}..{max(ys)} bottom_row={bottom}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true",
                        help="also render /tmp/chick_sprites_preview.png")
    parser.add_argument("--print", action="store_true", help="print the ASCII art")
    args = parser.parse_args()

    build()
    for name, grid in SPRITES:
        assert all(len(row) == grid.w for row in grid.rows()), f"{name}: row width"
        assert all(ch in GLYPH for row in grid.rows() for ch in row), f"{name}: glyph"

    write_c(OUTPUT)
    report()
    if args.preview:
        write_preview(Path("/tmp/chick_sprites_preview.png"))
    if args.print:
        for name, grid in SPRITES:
            print(f"--- {name} {grid.w}x{grid.h}")
            print("   " + "".join(str(x % 10) for x in range(grid.w)))
            for index, row in enumerate(grid.rows()):
                print(f"{index:2d} {row}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
