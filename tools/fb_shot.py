#!/usr/bin/env python3
"""把串口日志里的 SHOT 块解码成 PPM 图片(开发自检用)。

用法:
    python3 tools/fb_shot.py <串口日志> <输出目录>

日志来自 CHICK_RUN_BENCH 固件:每帧先 4 倍降采样,再按“近似同色”游程编码。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path


def decode(log: str, out_dir: Path) -> int:
    count = 0
    for block in re.finditer(r"SHOT_BEGIN (\S+) (\d+) (\d+)(.*?)SHOT_END", log, re.S):
        label, w, h, body = block.group(1), int(block.group(2)), int(block.group(3)), block.group(4)
        pixels: list[int] = []
        for token in re.findall(r"([0-9A-Fa-f]+):([0-9A-Fa-f]{4})", body):
            pixels.extend([int(token[1], 16)] * int(token[0], 16))
        if len(pixels) != w * h:
            print(f"WARN {label}: 像素数 {len(pixels)} != {w*h}", file=sys.stderr)
            continue
        path = out_dir / f"{label}.ppm"
        with path.open("wb") as handle:
            handle.write(f"P6\n{w} {h}\n255\n".encode())
            for value in pixels:
                r = ((value >> 11) & 0x1F) << 3
                g = ((value >> 5) & 0x3F) << 2
                b = (value & 0x1F) << 3
                handle.write(bytes((r, g, b)))
        print(f"wrote {path} ({w}x{h})")
        count += 1
    return count


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    if decode(log, out_dir) == 0:
        print("没有找到 SHOT 块", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
