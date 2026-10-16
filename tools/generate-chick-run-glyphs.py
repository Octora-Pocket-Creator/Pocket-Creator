#!/usr/bin/env python3
"""把 LVGL 格式的字体文件转成应用自己的字形表(零 LVGL 渲染的第一步)。

为什么还要转一道:字体裁剪仍然用 lv_font_conv(见 generate-chick-run-font.py),
但**运行时**不再链接 LVGL。本脚本从生成好的 assets/fonts/chick_run_font_*.c 里
抽出字形位图(4bpp)、字形度量(advance/box/ofs)与码点映射,输出一份不依赖任何
库的纯 C 表,交给 main/chick_ui_draw.c 自己 blit。

为什么要和 LVGL 完全一致的语义(排版公式见下),因为换引擎不能让画面变样:
    adv    = (adv_w_raw + 8) >> 4          # 1/16 px -> px(四舍五入)
    glyph.x = pen_x + ofs_x
    glyph.y = pen_y + (line_height - base_line) - box_h - ofs_y
    4bpp 位图按【连续半字节】打包,高半字节在前,行与行之间不补齐
    (见 LVGL src/font/fmt_txt/lv_font_fmt_txt.c 的 bpp==4 分支)

用法:
    python3 tools/generate-chick-run-glyphs.py

输出(入库、参与编译):
    main/chick_run_glyphs.c
    main/chick_run_glyphs.h
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
OUT_C = ROOT / "main" / "chick_run_glyphs.c"
SIZES = (14, 20, 28)

CMAKE_ATTR = 'const __attribute__((aligned(4)))'


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def parse_bytes(text: str, array_name: str) -> list[int]:
    match = re.search(
        r"const uint8_t\s+" + array_name + r"\[\]\s*=\s*\{(.*?)\n\};",
        strip_comments(text),
        flags=re.S,
    )
    if not match:
        raise SystemExit(f"{array_name} not found")
    # 注意:lv_font_conv 会写单数字的十六进制(如 0xc),不能只匹配两位。
    return [int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{1,2})\b", match.group(1))]


def parse_u16_list(text: str, array_name: str) -> list[int]:
    match = re.search(
        r"const uint16_t\s+" + array_name + r"\[\]\s*=\s*\{(.*?)\n\};",
        strip_comments(text),
        flags=re.S,
    )
    if not match:
        raise SystemExit(f"{array_name} not found")
    return [int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]+)\b", match.group(1))]


def parse_glyph_dsc(text: str) -> list[dict]:
    block = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, flags=re.S)
    if not block:
        raise SystemExit("glyph_dsc not found")
    glyphs = []
    for line in block.group(1).splitlines():
        if ".adv_w" not in line:
            continue
        fields = dict(re.findall(r"\.(\w+)\s*=\s*(-?\d+)", line))
        glyphs.append({name: int(value) for name, value in fields.items()})
    if not glyphs:
        raise SystemExit("glyph_dsc entries not found")
    return glyphs


def parse_cmaps(text: str) -> list[dict]:
    block = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};",
                      strip_comments(text), flags=re.S)
    if not block:
        raise SystemExit("cmaps not found")
    cmaps = []
    for entry in re.findall(r"\{(.*?)\}", block.group(1), flags=re.S):
        fields = dict(re.findall(r"\.(\w+)\s*=\s*([^,]+)", entry))
        cmap = {
            "range_start": int(fields["range_start"], 0),
            "range_length": int(fields["range_length"], 0),
            "glyph_id_start": int(fields["glyph_id_start"], 0),
            "type": fields["type"].strip(),
            "list_length": int(fields.get("list_length", "0"), 0),
            "unicode_list": fields.get("unicode_list", "NULL").strip(),
        }
        cmaps.append(cmap)
    return cmaps


def build_map(text: str, cmaps: list[dict], glyph_count: int) -> list[tuple[int, int]]:
    """(码点, 字形下标) 列表,按码点升序。"""
    mapping: list[tuple[int, int]] = []
    for cmap in cmaps:
        if "FORMAT0" in cmap["type"]:
            for offset in range(cmap["range_length"]):
                mapping.append((cmap["range_start"] + offset,
                                cmap["glyph_id_start"] + offset))
        elif "SPARSE" in cmap["type"]:
            sparse = parse_u16_list(text, cmap["unicode_list"])
            if len(sparse) != cmap["list_length"]:
                raise SystemExit(f"{cmap['unicode_list']} length mismatch")
            for index, offset in enumerate(sparse):
                mapping.append((cmap["range_start"] + offset,
                                cmap["glyph_id_start"] + index))
        else:
            raise SystemExit(f"unsupported cmap type {cmap['type']}")
    mapping.sort()
    for _, gid in mapping:
        if gid >= glyph_count:
            raise SystemExit(f"glyph id {gid} out of range {glyph_count}")
    return mapping


def parse_font(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    name = path.stem
    bitmap = parse_bytes(text, "glyph_bitmap")
    glyphs = parse_glyph_dsc(text)
    cmaps = parse_cmaps(text)
    mapping = build_map(text, cmaps, len(glyphs))
    line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", text).group(1))
    bpp = int(re.search(r"\.bpp\s*=\s*(\d+)", text).group(1))
    if bpp != 4:
        raise SystemExit(f"{name}: 只支持 4bpp, 实际 {bpp}")
    # 自检:每个字形的位图区间必须落在 blob 内,且 blob 正好被用到末尾。
    # 这个检查能立刻发现“少解析了一个字节”这类错误(单数字十六进制就踩过一次)。
    used = 0
    for index, glyph in enumerate(glyphs):
        size = (glyph["box_w"] * glyph["box_h"] + 1) // 2
        if glyph["bitmap_index"] + size > len(bitmap):
            raise SystemExit(f"{name}: 字形 {index} 的位图越界 "
                             f"({glyph['bitmap_index']}+{size} > {len(bitmap)})")
        used = max(used, glyph["bitmap_index"] + size)
    if used != len(bitmap):
        raise SystemExit(f"{name}: 位图长度对不上,用到 {used},实际 {len(bitmap)}")
    return {
        "name": name,
        "bitmap": bitmap,
        "glyphs": glyphs,
        "mapping": mapping,
        "line_height": line_height,
        "base_line": base_line,
    }


def emit(fonts: list[dict]) -> str:
    out = [
        "// main/chick_run_glyphs.c —— 由 tools/generate-chick-run-glyphs.py 生成,请勿手改。",
        "//",
        "// 数据来源:assets/fonts/chick_run_font_*.c(lv_font_conv 的 LVGL 输出)。",
        "// 这里只保留渲染需要的部分:4bpp 位图 + 字形度量 + 码点映射。",
        "// 排版语义与 LVGL 一致,见脚本头部注释与 main/chick_ui_draw.c。",
        "",
        '#include "chick_run_glyphs.h"',
        "",
    ]
    for font in fonts:
        key = font["name"].removeprefix("chick_run_font_")
        bitmap = font["bitmap"]
        out.append(f"// ---- {key}px:4bpp 位图 {len(bitmap)} 字节,"
                   f"字形 {len(font['glyphs'])} 个,码点 {len(font['mapping'])} 个 ----")
        out.append(f"static const uint8_t s_bitmap_{key}[] = {{")
        for index in range(0, len(bitmap), 16):
            chunk = ", ".join(f"0x{value:02X}" for value in bitmap[index:index + 16])
            out.append(f"    {chunk},")
        out.append("};")
        out.append("")
        out.append(f"static const chick_glyph_dsc_t s_glyphs_{key}[] = {{")
        for glyph in font["glyphs"]:
            out.append("    {{ .bitmap_index = {bitmap_index}, .adv_w = {adv_w}, "
                       ".box_w = {box_w}, .box_h = {box_h}, .ofs_x = {ofs_x}, "
                       ".ofs_y = {ofs_y} }},".format(**glyph))
        out.append("};")
        out.append("")
        out.append(f"static const chick_glyph_map_t s_map_{key}[] = {{")
        for code, gid in font["mapping"]:
            out.append(f"    {{ 0x{code:04X}, {gid} }},")
        out.append("};")
        out.append("")
        out.append(f"const chick_font_t chick_font_{key} = {{")
        out.append(f"    .bitmap = s_bitmap_{key},")
        out.append(f"    .bitmap_size = sizeof(s_bitmap_{key}),")
        out.append(f"    .glyphs = s_glyphs_{key},")
        out.append(f"    .glyph_count = {len(font['glyphs'])},")
        out.append(f"    .map = s_map_{key},")
        out.append(f"    .map_count = {len(font['mapping'])},")
        out.append(f"    .line_height = {font['line_height']},")
        out.append(f"    .base_line = {font['base_line']},")
        out.append("};")
        out.append("")
    return "\n".join(out) + "\n"


def main() -> int:
    fonts = []
    for size in SIZES:
        path = FONT_DIR / f"chick_run_font_{size}.c"
        if not path.is_file():
            print(f"ERROR: {path} 不存在,请先运行 tools/generate-chick-run-font.py",
                  file=sys.stderr)
            return 1
        fonts.append(parse_font(path))
    OUT_C.write_text(emit(fonts), encoding="utf-8")
    print(f"wrote {OUT_C.relative_to(ROOT)}")
    for font in fonts:
        print(f"  {font['name']}: 位图 {len(font['bitmap'])} 字节,"
              f"字形 {len(font['glyphs'])},码点 {len(font['mapping'])},"
              f"行高 {font['line_height']} 基线 {font['base_line']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
