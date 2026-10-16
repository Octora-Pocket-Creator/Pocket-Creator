#!/usr/bin/env python3
"""保持音效旋律的长度与玩法常量一致(静态检查)。

为什么需要它:音符道具吃到后小鸡头顶飘 🎵 的时长是 CHICK_RUN_NOTE_FLOAT_MS,
加速护身符的"冲刺"旋律长度是 CHICK_RUN_BOOST_MS —— "音乐停了符号/效果也结束"
这条体验约定,靠的是旋律长度和常量对上。旋律是 RTTTL 字符串,改一个音符或
一个 b(bpm)就会悄悄改变总时长,设备上表现为"音乐早停了符号还挂着"(或反过来)。
这里用与 main/rtttl_player.c 完全相同的公式把每条旋律的长度算出来并对齐常量。

音符时长公式(见 main/rtttl_player.c 的 parse_note):
    duration_ms = (240000 / bpm) / duration      # duration 是 1/4/8/16/32
    附点再乘 1.5
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI_SOURCE = ROOT / "main" / "chick_run_ui.c"
MODEL_HEADER = ROOT / "main" / "chick_run_model.h"

SFX_RE = re.compile(
    r'static const char \*const (SFX_\w+)\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', re.S
)
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
DEFAULTS_RE = re.compile(r"d=(\d+)\s*,\s*o=(\d+)\s*,\s*b=(\d+)")
NOTE_RE = re.compile(r"^(\d*)([a-gp]#?)(\d*)(\.?)$")

# 允许的长度偏差(ms):旋律是人工排的音符,不可能与常量逐毫秒相等。
TOLERANCE_MS = 150


def melodies() -> dict[str, str]:
    """main/chick_run_ui.c 里所有 SFX_* 旋律(把相邻的字符串字面量接起来)。"""
    text = UI_SOURCE.read_text(encoding="utf-8")
    found: dict[str, str] = {}
    for match in SFX_RE.finditer(text):
        found[match.group(1)] = "".join(STRING_RE.findall(match.group(2)))
    return found


def duration_ms(song: str) -> int:
    """按 rtttl_player.c 的公式算一条 RTTTL 的总时长。

    RTTTL 的形状是 `名称 : 默认值 : 音符`,所以这里按冒号切成三段再解析音符。
    """
    parts = song.split(":", 2)
    if len(parts) != 3:
        raise SystemExit(f"{song}: 不是 `名称:默认值:音符` 三段式")
    _name, defaults_text, body = parts
    defaults = DEFAULTS_RE.search(defaults_text)
    if defaults is None:
        raise SystemExit(f"{song}: 默认值段里没有 d=/o=/b=")
    duration, _octave, bpm = (int(value) for value in defaults.groups())
    total = 0
    for raw in body.split(","):
        note = raw.strip()
        match = NOTE_RE.match(note)
        if match is None:
            raise SystemExit(f"{song}: 解析不了的音符 {note!r}")
        length = int(match.group(1)) if match.group(1) else duration
        if length == 0:
            length = duration
        value = (240000 // bpm) // length
        if match.group(4):
            value += value // 2
        total += value
    return total


def model_constant(name: str) -> int:
    text = MODEL_HEADER.read_text(encoding="utf-8")
    match = re.search(rf"^#define\s+{name}\s+(\d+)", text, re.M)
    if match is None:
        raise SystemExit(f"main/chick_run_model.h: 找不到 {name}")
    return int(match.group(1))


def main() -> int:
    songs = melodies()
    required = {
        "SFX_BOOST": model_constant("CHICK_RUN_BOOST_MS"),
        "SFX_NOTE": model_constant("CHICK_RUN_NOTE_FLOAT_MS"),
        "SFX_TWINKLE": model_constant("CHICK_RUN_NOTE_FLOAT_MS"),
    }
    errors: list[str] = []
    lengths: dict[str, int] = {}
    for name, expected in required.items():
        if name not in songs:
            errors.append(f"main/chick_run_ui.c: 找不到旋律 {name}")
            continue
        lengths[name] = duration_ms(songs[name])
        delta = lengths[name] - expected
        if abs(delta) > TOLERANCE_MS:
            errors.append(
                f"{name}: 旋律 {lengths[name]} ms,对应常量 {expected} ms,"
                f"差 {delta:+d} ms(超过 ±{TOLERANCE_MS})"
            )

    # 音乐道具是两首随机二选一,两首长度必须接近,否则 🎵 的飘动时间对不上其中一首。
    if "SFX_NOTE" in lengths and "SFX_TWINKLE" in lengths:
        gap = abs(lengths["SFX_NOTE"] - lengths["SFX_TWINKLE"])
        if gap > 2 * TOLERANCE_MS:
            errors.append(
                f"两首音乐旋律长度差 {gap} ms(欢乐颂 {lengths['SFX_NOTE']} / "
                f"小星星 {lengths['SFX_TWINKLE']}),头顶 🎵 只能对齐其中一首"
            )

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    summary = ", ".join(f"{name}={value}ms" for name, value in sorted(lengths.items()))
    print(f"Chick run audio check: PASS ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
