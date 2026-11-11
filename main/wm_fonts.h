// main/wm_fonts.h —— 应用字体声明。字体由 tools/gen_wm_fonts.py 生成,见 assets/README。
//
// 回退链(缺字时依次查找):
//   wm_font_28 → wm_font_16 → wm_font_16_ext → lv_font_montserrat_14
// 覆盖范围:ASCII、常用中英文标点、GB2312 全部 6763 个汉字;16 px 另含 CJK 基本区
// (U+4E00..U+9FA5)其余 14139 字,所以生僻姓名字在大标题里会以 16 px 小字显示而不是方框。
// 不在上述范围的字符(如 emoji、日文假名)显示为 LVGL 缺字占位框。
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(wm_font_16);
LV_FONT_DECLARE(wm_font_16_ext);
LV_FONT_DECLARE(wm_font_28);
