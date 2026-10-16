#!/usr/bin/env python3
"""生成 16x16 蓝噪声抖动掩码(void-and-cluster, Ulichney 1993)。

为什么要生成而不是手写:
  * 有序抖动矩阵(比如 4x4 / 8x8 Bayer)在缓慢渐变上会露出整齐的网格纹理 ——
    实机上看就是"一块一块的颗粒"和"每 N 行一条的条纹";
  * 蓝噪声在空间上把阈值打散(低能量在低频、能量集中到高频),所以同样的量化误差
    看上去只是很细的颗粒,而眼睛对这种细颗粒的容忍度远高于规则纹理或硬边;
  * 24 号字体的做法一样:这里只提交生成脚本 + 生成结果,便于复查与复现。

算法(void-and-cluster):
  1. 以 50% 密度的随机二值图案为起点;
  2. 迭代:把"最紧的簇"(邻域能量最高的 1)移到"最大的空"(邻域能量最低的 0),
     直到找到的簇和空是同一个点 —— 此时得到原型图案;
  3. 从原型出发,不断移除最紧的簇,依次赋 rank = n/2-1 ... 0;
     再回到原型,不断往最大的空里插入,依次赋 rank = n/2 ... n-1。
  得到的 rank(0..255) 就是掩码:阈值 t 时"rank < t"的像素被点亮,
  任意阈值下点亮位置都保持蓝噪声分布。

用法:
    python3 tools/generate-dither-mask.py

输出(入库、参与编译):
    main/chick_dither.c
"""

from __future__ import annotations

import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_C = ROOT / "main" / "chick_dither.c"

N = 32                 # 掩码边长:32x32 = 1024 级阈值(占空比细分到 1/1024),周期也够长
SIGMA = 2.2            # 高斯滤波半径:越小越"点状",越大越"块状"(实测 2.2 的各方向最均衡)
SEED = 1               # 固定种子:结果可复现,评审可以比对


def kernel() -> list[list[float]]:
    """环形(上下左右相接)高斯核。"""
    k = [[0.0] * N for _ in range(N)]
    for dy in range(N):
        for dx in range(N):
            y = min(dy, N - dy)
            x = min(dx, N - dx)
            k[dy][dx] = math.exp(-(x * x + y * y) / (2.0 * SIGMA * SIGMA))
    return k


class Field:
    """二值图案 + 环形高斯能量场。插入/移除时增量更新,避免每次 O(N^4) 重算。"""

    def __init__(self, rng: random.Random) -> None:
        self.rng = rng
        self.k = kernel()
        self.grid = [[False] * N for _ in range(N)]
        self.energy = [[0.0] * N for _ in range(N)]

    def add(self, x: int, y: int, sign: float = 1.0) -> None:
        self.grid[y][x] = sign > 0
        for dy in range(N):
            for dx in range(N):
                self.energy[(y + dy) % N][(x + dx) % N] += sign * self.k[dy][dx]

    # 平局必须随机化:按行优先取"第一个最大值"会让图案带上一维方向性 ——
    # 实测那样出来的掩码在水平/垂直方向的相邻性差一大截,实机上就是细条纹。
    # 这里用极小的随机扰动代替严格平局判定(对能量排序无影响,只打散平局)。
    JITTER = 1e-9

    def __init_jitter(self) -> None:
        pass

    def tighten(self) -> tuple[int, int, float]:
        """最紧的簇:被点亮的像素里能量最高的那个。"""
        best, best_e = (-1, -1), -1.0
        for y in range(N):
            for x in range(N):
                e = self.energy[y][x] + self.rng.uniform(0.0, self.JITTER)
                if self.grid[y][x] and e > best_e:
                    best, best_e = (x, y), e
        return best[0], best[1], best_e

    def largest_void(self) -> tuple[int, int, float]:
        """最大的空:未被点亮的像素里能量最低的那个。"""
        best, best_e = (-1, -1), float("inf")
        for y in range(N):
            for x in range(N):
                e = self.energy[y][x] + self.rng.uniform(0.0, self.JITTER)
                if not self.grid[y][x] and e < best_e:
                    best, best_e = (x, y), e
        return best[0], best[1], best_e


def prototype(field: Field, rng: random.Random) -> None:
    """随机 50% 图案收敛到"无簇无空"的原型。"""
    cells = [(x, y) for y in range(N) for x in range(N)]
    rng.shuffle(cells)
    for x, y in cells[: N * N // 2]:
        field.add(x, y, +1.0)

    for _ in range(2000):                      # 上限:正常情况下几十步就稳定
        cx, cy, _ = field.tighten()
        field.add(cx, cy, -1.0)
        vx, vy, _ = field.largest_void()
        if (vx, vy) == (cx, cy):
            field.add(cx, cy, +1.0)            # 同一个点 → 已收敛
            return
        field.add(vx, vy, +1.0)
    raise SystemExit("原型未收敛:请调整 SIGMA 或换种子")


def rank_mask() -> list[int]:
    rng = random.Random(SEED)
    field = Field(rng)
    prototype(field, rng)

    ranks = [-1] * (N * N)
    snapshot = [[field.grid[y][x] for x in range(N)] for y in range(N)]

    # 下半部分:从原型不断移除最紧的簇,依次给 rank = n/2-1 ... 0
    for rank in range(N * N // 2 - 1, -1, -1):
        cx, cy, _ = field.tighten()
        field.add(cx, cy, -1.0)
        ranks[cy * N + cx] = rank

    # 回到原型,再不断往最大的空里插入,依次 rank = n/2 ... n-1
    for y in range(N):
        for x in range(N):
            field.grid[y][x] = snapshot[y][x]
    field.energy = [[0.0] * N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            if snapshot[y][x]:
                field.add(x, y, +1.0)
    for rank in range(N * N // 2, N * N):
        vx, vy, _ = field.largest_void()
        field.add(vx, vy, +1.0)
        ranks[vy * N + vx] = rank

    if sorted(ranks) != list(range(N * N)):
        raise SystemExit(f"rank 不是 0..{N * N - 1} 的排列,生成失败")
    return ranks


def report(ranks: list[int]) -> None:
    """体检指标:关键是"同时被点亮的相邻对"要明显少于独立随机(p^2)。

    注意不能用"状态相同率"当指标:低占空比时大部分像素都是 0,
    这个值天然接近 1,看不出质量。用 on-on 比率才在各档都有效。
    """
    full = N * N
    print(f"mask {N}x{N}, sigma={SIGMA}, seed={SEED}")
    on = sum(1 for r in ranks if r < full // 2)
    print(f"  50% 阈值点亮像素: {on}/{full}(应为 {full // 2})")

    for frac in (0.1, 0.25, 0.5, 0.75, 0.9):
        threshold = int(full * frac)
        p = threshold / full
        worst = 0.0
        for dx, dy in ((1, 0), (0, 1), (1, 1), (1, -1)):
            pairs = 0
            for y in range(N):
                for x in range(N):
                    if (ranks[y * N + x] < threshold and
                            ranks[((y + dy) % N) * N + ((x + dx) % N)] < threshold):
                        pairs += 1
            worst = max(worst, pairs / full)
        ratio = worst / (p * p) if p > 0 else 0.0
        print(f"  占空比 {frac:.2f}:最差方向的 on-on 比率 {ratio:.2f}(蓝噪声应 < 1)")


def emit(ranks: list[int]) -> None:
    rows = []
    for y in range(N):
        values = ", ".join(f"{ranks[y * N + x]:4d}" for x in range(N))
        rows.append(f"    {{ {values} }},")
    body = "\n".join(rows)
    OUT_C.write_text(
        f'''// main/chick_dither.c —— {N}x{N} 蓝噪声抖动掩码(生成物,勿手改)。
//
// 生成脚本:tools/generate-dither-mask.py(void-and-cluster, seed={SEED}, sigma={SIGMA})
// 用途与原因见 main/chick_dither.h —— 简短说:RGB565 只有 5/6 位,长渐变必须抖动,
// 而有规则的抖动矩阵(Bayer)在实机上会露出网格/条纹,蓝噪声只有细颗粒。
//
// 掩码值 0..{N*N-1} 各出现一次:阈值 t 时 "rank < t" 的像素点亮,任意阈值下都是蓝噪声。
#include "chick_dither.h"

#define CHICK_DITHER_SIZE {N}

static const uint16_t CHICK_DITHER[{N}][{N}] = {{
{body}
}};

uint16_t chick_dither_mask(int x, int y) {{
    // 负坐标是脏区裁剪后的正常输入,先取绝对值再取模(结果永远落在 0..{N - 1})。
    const unsigned ux = (unsigned)(x < 0 ? -x : x) & (CHICK_DITHER_SIZE - 1);
    const unsigned uy = (unsigned)(y < 0 ? -y : y) & (CHICK_DITHER_SIZE - 1);
    return CHICK_DITHER[uy][ux];
}}
''',
        encoding="utf-8",
    )
    print(f"wrote {OUT_C.relative_to(ROOT)}")


def main() -> int:
    ranks = rank_mask()
    report(ranks)
    emit(ranks)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
