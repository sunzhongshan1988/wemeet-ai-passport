#!/usr/bin/env python3
"""Every non-ASCII character in the firmware's UI strings must exist in the fonts.

The UI prints fixed Chinese copy with wm_font_16 / wm_font_28. A character missing from
those fonts shows up as a placeholder box on the device, and nothing at build time would
notice. This test decodes the string literals of main/*.c and checks each code point
against the cmap tables of the generated font sources.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPE_RE = re.compile(r"\\u([0-9A-Fa-f]{4})")


def font_codepoints(name: str) -> set[int]:
    source = (ROOT / "assets" / "fonts" / f"{name}.c").read_text(encoding="utf-8")
    lists = {
        int(index): [int(value, 16) for value in re.findall(r"0x[0-9a-f]+", body)]
        for index, body in re.findall(r"unicode_list_(\d+)\[\] = \{(.*?)\};", source, re.S)
    }
    covered: set[int] = set()
    for start, length, ulist in re.findall(
        r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = \d+, \.unicode_list = (\w+),",
        source,
    ):
        if ulist == "NULL":
            covered.update(range(int(start), int(start) + int(length)))
        else:
            covered.update(int(start) + offset for offset in lists[int(ulist.rsplit("_", 1)[1])])
    return covered


def ui_codepoints() -> dict[int, str]:
    """Non-ASCII code points used in string literals of the UI sources -> first file."""
    found: dict[int, str] = {}
    for name in ("wm_ui.c", "wm_app.c"):
        source = (ROOT / "main" / name).read_text(encoding="utf-8")
        # Drop comments so prose in comments is not mistaken for UI copy.
        source = re.sub(r"//[^\n]*", "", source)
        source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
        for literal in STRING_RE.findall(source):
            literal = ESCAPE_RE.sub(lambda m: chr(int(m.group(1), 16)), literal)
            for char in literal:
                if ord(char) > 0x7E:
                    found.setdefault(ord(char), name)
    return found


class FontCoverageTest(unittest.TestCase):
    def test_ui_copy_is_covered_by_both_text_fonts(self) -> None:
        used = ui_codepoints()
        self.assertGreater(len(used), 100, "UI string extraction found suspiciously few characters")
        for font in ("wm_font_16", "wm_font_28"):
            covered = font_codepoints(font)
            missing = sorted(cp for cp in used if cp not in covered)
            self.assertEqual(
                missing, [],
                f"{font} lacks: " + ", ".join(f"U+{cp:04X} {chr(cp)} ({used[cp]})" for cp in missing),
            )

    def test_fonts_cover_gb2312_and_known_gap_is_reported(self) -> None:
        base = font_codepoints("wm_font_16")
        extension = font_codepoints("wm_font_16_ext")
        self.assertTrue(set(range(0x20, 0x7F)) <= base)
        for char in "中文孙张王李赵钱，。（）":
            self.assertTrue(ord(char) in base, char)
        # GBK-only name characters come from the extension font, not the base font.
        for char in "喆堃":
            self.assertFalse(ord(char) in base, char)
            self.assertTrue(ord(char) in extension, char)
        # Negative control: kana is outside the declared range and must not pass.
        self.assertFalse(ord("あ") in base | extension)


if __name__ == "__main__":
    unittest.main()
