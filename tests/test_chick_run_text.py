#!/usr/bin/env python3
"""保持应用文案与字体清单一致(静态检查)。

字体本身是 LVGL 内置的思源黑体 16 子集,真实字形覆盖由设备启动时的
逐码点自检(见 chick_run_ui.c 的 font_check_coverage,串口打印 U+XXXX)负责;
这里守住另一头:凡是会显示出来的中文,必须出现在 main/chick_run_text.h 的
清单里,这样"改文案忘记登记"不会静默溜到设备上变成方框。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXT_HEADER = ROOT / "main" / "chick_run_text.h"
UI_SOURCES = (
    ROOT / "main" / "chick_run_ui.c",
    ROOT / "main" / "main.c",
)
LOG_PREFIXES = ("ESP_LOG",)
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    text = BLOCK_COMMENT_RE.sub("", text)
    return LINE_COMMENT_RE.sub("", text)


def displayed_codepoints(path: Path) -> set[int]:
    """该文件里会显示给用户的字符串中的非 ASCII 码点。

    日志文本不算:串口日志用中文是本仓库的既有风格,但它不经过字体渲染,
    不需要登记字形。按语句(以 ';' 切分)跳过整条日志调用,这样跨行的
    ESP_LOGE("...", "...") 参数列表也不会被误判。
    """
    found: set[int] = set()
    code = strip_comments(path.read_text(encoding="utf-8"))
    for statement in code.split(";"):
        if any(prefix in statement for prefix in LOG_PREFIXES):
            continue
        for literal in STRING_RE.findall(statement):
            found.update(ord(char) for char in literal if ord(char) > 0x7F)
    return found


def inventory_codepoints() -> set[int]:
    text = TEXT_HEADER.read_text(encoding="utf-8")
    definitions = re.findall(r'#define\s+CHICK_TEXT_\w+\s+"((?:[^"\\]|\\.)*)"', text)
    inventory: set[int] = set()
    for definition in definitions:
        inventory.update(ord(char) for char in definition if ord(char) > 0x7F)
    return inventory


def main() -> int:
    errors: list[str] = []

    inventory = inventory_codepoints()
    if len(inventory) < 30:
        errors.append(
            f"main/chick_run_text.h: 只解析出 {len(inventory)} 个非 ASCII 码点,"
            "清单解析可能已经失效"
        )

    for path in UI_SOURCES:
        if not path.exists():
            continue
        used = displayed_codepoints(path)
        for codepoint in sorted(used - inventory):
            errors.append(
                f"{path.relative_to(ROOT)}: 显示文字 U+{codepoint:04X}({chr(codepoint)})"
                " 未登记在 chick_run_text.h 的清单里"
            )

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Chick run text check: PASS ({len(inventory)} 个非 ASCII 码点已登记)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
