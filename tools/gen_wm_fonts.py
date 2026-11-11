#!/usr/bin/env python3
"""Generate the WeMeet device fonts (LVGL 9 fmt_txt) from Inter + Noto Sans CJK SC.

Why a custom generator instead of lv_font_conv:
  * the glyph bitmaps of ~28k CJK glyphs are several MB; emitting them as C source
    would add ~15 MB of text to the repository. This script writes the bitmaps to a
    binary blob (linked through ESP-IDF EMBED_FILES) and only the small glyph tables
    to C source;
  * Latin glyphs come from Inter and CJK glyphs from Noto Sans CJK SC in one font, so
    mixed text shares one baseline without relying on fallback metrics.

Outputs, per font <name>:
  assets/fonts/<name>.bin   packed glyph bitmaps (LV_FONT_FMT_TXT_PLAIN bit stream)
  assets/fonts/<name>.c     glyph descriptors, cmaps and the lv_font_t object

Requires Pillow and fontTools. Usage (from the repository root):
  python3 tools/gen_wm_fonts.py \
      --inter-dir /usr/share/fonts/opentype/inter \
      --noto-dir /usr/share/fonts/opentype/noto
"""

from __future__ import annotations

import argparse
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "fonts"
NOTO_SC_INDEX = 2  # face order inside the .ttc: JP, KR, SC, TC, HK

# Non-ASCII punctuation and symbols the UI and typical names / titles need.
PUNCT = (
    " ¥©®°±·×÷"
    "–—‘’“”•…‰′″※"
    "€℃←↑→↓✓✕●○★☆"
    "　、。〈〉《》「」『』【】"
    "〔〕〜"
)
FULLWIDTH = "".join(chr(c) for c in range(0xFF01, 0xFF5F)) + "￥"


def gb2312_hanzi() -> list[int]:
    """All 6763 GB2312 level-1 and level-2 Chinese characters, as code points."""
    result = []
    for row in range(16, 88):
        for col in range(1, 95):
            try:
                result.append(ord(bytes([0xA0 + row, 0xA0 + col]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return sorted(set(result))


class BitWriter:
    """MSB-first bit stream; LVGL's plain format does not align glyph rows to bytes."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.acc = 0
        self.bits = 0

    def write(self, value: int, width: int) -> None:
        self.acc = (self.acc << width) | value
        self.bits += width
        while self.bits >= 8:
            self.bits -= 8
            self.data.append((self.acc >> self.bits) & 0xFF)
        self.acc &= (1 << self.bits) - 1

    def flush(self) -> None:
        if self.bits:
            self.data.append((self.acc << (8 - self.bits)) & 0xFF)
        self.acc = 0
        self.bits = 0


def render(font: ImageFont.FreeTypeFont, char: str, size: int, bpp: int):
    """Rasterize one glyph. Returns (adv_w_x16, box_w, box_h, ofs_x, ofs_y, pixels)."""
    pad = size
    canvas = Image.new("L", (size * 3, size * 3), 0)
    ImageDraw.Draw(canvas).text((pad, pad * 2), char, font=font, fill=255, anchor="ls")
    adv = round(font.getlength(char) * 16)
    box = canvas.getbbox()
    if box is None:  # whitespace
        return adv, 0, 0, 0, 0, []
    left, top, right, bottom = box
    levels = (1 << bpp) - 1
    pixels = [
        (canvas.getpixel((x, y)) * levels + 127) // 255
        for y in range(top, bottom)
        for x in range(left, right)
    ]
    # ofs_y is the distance from the baseline up to the bottom edge of the box.
    return adv, right - left, bottom - top, left - pad, pad * 2 - bottom, pixels


def coverage(path: str, index: int = 0) -> frozenset[int]:
    """Code points the face maps to a real glyph, read from its cmap table."""
    face = TTFont(path, fontNumber=index, lazy=True)
    return frozenset(face.getBestCmap())


def build(name: str, size: int, bpp: int, groups, fallback: str | None, comment: str) -> dict:
    """groups: list of ((font, coverage), sorted code points, cmap_kind), ranges not overlapping."""
    writer = BitWriter()
    glyphs = [(0, 0, 0, 0, 0, 0)]  # glyph id 0 is reserved by LVGL
    cmaps = []
    lists = []
    missing = []
    for (font, covered), codepoints, kind in groups:
        present = []
        for cp in codepoints:
            char = chr(cp)
            if cp not in covered:
                missing.append(cp)
                continue
            adv, w, h, ox, oy, pixels = render(font, char, size, bpp)
            offset = len(writer.data)
            for value in pixels:
                writer.write(value, bpp)
            writer.flush()
            glyphs.append((offset, adv, w, h, ox, oy))
            present.append(cp)
        if not present:
            continue
        start_id = len(glyphs) - len(present)
        if kind == "dense":
            assert present == list(range(present[0], present[-1] + 1)), "dense range has holes"
            cmaps.append((present[0], len(present), start_id, None, 0, "FORMAT0_TINY"))
        else:
            base = present[0]
            assert present[-1] - base < 65536
            lists.append([cp - base for cp in present])
            cmaps.append((base, present[-1] - base + 1, start_id, len(lists) - 1, len(present),
                          "SPARSE_TINY"))

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"{name}.bin").write_bytes(bytes(writer.data))

    line_height = round(size * 1.3)
    base_line = round(size * 0.27)
    large = len(writer.data) >= (1 << 20)
    lines = [
        f"// {name}: {comment}",
        f"// Generated by tools/gen_wm_fonts.py -- do not edit. {len(glyphs) - 1} glyphs, {size} px, {bpp} bpp.",
        f"// Glyph bitmaps live in {name}.bin and are linked through EMBED_FILES.",
        '#include "lvgl.h"',
        "",
    ]
    if large:
        lines += [
            "#if !LV_FONT_FMT_TXT_LARGE",
            f'#error "{name} needs CONFIG_LV_FONT_FMT_TXT_LARGE=y (bitmap > 1 MB). '
            'Delete the stale sdkconfig so sdkconfig.defaults is applied again."',
            "#endif",
            "",
        ]
    lines += [
        f'extern const uint8_t {name}_bitmap[] asm("_binary_{name}_bin_start");',
        "",
        "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {",
    ]
    for offset, adv, w, h, ox, oy in glyphs:
        lines.append(
            f"    {{.bitmap_index = {offset}, .adv_w = {adv}, .box_w = {w}, .box_h = {h}, "
            f".ofs_x = {ox}, .ofs_y = {oy}}},"
        )
    lines += ["};", ""]
    for index, values in enumerate(lists):
        lines.append(f"static const uint16_t unicode_list_{index}[] = {{")
        for at in range(0, len(values), 16):
            lines.append("    " + ", ".join(f"0x{v:x}" for v in values[at:at + 16]) + ",")
        lines += ["};", ""]
    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    for start, length, start_id, list_index, list_len, kind in cmaps:
        ulist = "NULL" if list_index is None else f"unicode_list_{list_index}"
        lines.append(
            f"    {{.range_start = {start}, .range_length = {length}, .glyph_id_start = {start_id}, "
            f".unicode_list = {ulist}, .glyph_id_ofs_list = NULL, .list_length = {list_len}, "
            f".type = LV_FONT_FMT_TXT_CMAP_{kind}}},"
        )
    lines += [
        "};",
        "",
        "static const lv_font_fmt_txt_dsc_t font_dsc = {",
        f"    .glyph_bitmap = {name}_bitmap,",
        "    .glyph_dsc = glyph_dsc,",
        "    .cmaps = cmaps,",
        "    .kern_dsc = NULL,",
        "    .kern_scale = 0,",
        f"    .cmap_num = {len(cmaps)},",
        f"    .bpp = {bpp},",
        "    .kern_classes = 0,",
        "    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,",
        "};",
        "",
    ]
    if fallback:
        lines += [f"extern const lv_font_t {fallback};", ""]
    lines += [
        f"const lv_font_t {name} = {{",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {line_height},",
        f"    .base_line = {base_line},",
        "    .subpx = LV_FONT_SUBPX_NONE,",
        "    .underline_position = -2,",
        "    .underline_thickness = 1,",
        "    .dsc = &font_dsc,",
        f"    .fallback = {'&' + fallback if fallback else 'NULL'},",
        "};",
        "",
    ]
    (OUT / f"{name}.c").write_text("\n".join(lines), encoding="utf-8")
    return {"name": name, "glyphs": len(glyphs) - 1, "bitmap_bytes": len(writer.data),
            "missing": missing, "large": large}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--inter-dir", default="/usr/share/fonts/opentype/inter")
    parser.add_argument("--noto-dir", default="/usr/share/fonts/opentype/noto")
    args = parser.parse_args()

    def inter(weight: str, size: int):
        path = f"{args.inter_dir}/Inter-{weight}.otf"
        return ImageFont.truetype(path, size), coverage(path)

    def noto(weight: str, size: int):
        path = f"{args.noto_dir}/NotoSansCJK-{weight}.ttc"
        return ImageFont.truetype(path, size, index=NOTO_SC_INDEX), coverage(path, NOTO_SC_INDEX)

    ascii_range = list(range(0x20, 0x7F))
    punct = sorted({ord(c) for c in PUNCT})
    fullwidth = sorted({ord(c) for c in FULLWIDTH})
    gb = gb2312_hanzi()
    gb_set = set(gb)
    rest = [cp for cp in range(0x4E00, 0x9FA6) if cp not in gb_set]

    reports = [
        build("wm_font_16", 16, 4,
              [(inter("Medium", 16), ascii_range, "dense"),
               (noto("Medium", 16), punct, "sparse"),
               (noto("Medium", 16), gb, "sparse"),
               (noto("Medium", 16), fullwidth, "sparse")],
              "wm_font_16_ext",
              "UI text. ASCII (Inter Medium) + punctuation + GB2312 (Noto Sans CJK SC Medium)."),
        build("wm_font_16_ext", 16, 2,
              [(noto("Medium", 16), rest, "sparse")],
              "lv_font_montserrat_14",
              "Fallback for rare name characters: CJK U+4E00..U+9FA5 outside GB2312."),
        build("wm_font_28", 28, 2,
              [(inter("SemiBold", 28), ascii_range, "dense"),
               (noto("Bold", 28), punct, "sparse"),
               (noto("Bold", 28), gb, "sparse"),
               (noto("Bold", 28), fullwidth, "sparse")],
              "wm_font_16",
              "Headlines and attendee names. ASCII (Inter SemiBold) + GB2312 (Noto Sans CJK SC Bold)."),
    ]
    for report in reports:
        print(f"{report['name']}: {report['glyphs']} glyphs, {report['bitmap_bytes']} bitmap bytes, "
              f"large={report['large']}, missing={[hex(cp) for cp in report['missing']]}")


if __name__ == "__main__":
    main()
