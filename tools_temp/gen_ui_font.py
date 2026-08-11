#!/usr/bin/env python3
"""Generate a custom LVGL font containing all CJK characters used by the UI."""
import os
import subprocess
import sys

PROJECT_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
UI_DIR = os.path.join(PROJECT_ROOT, 'components', 'ui')
LVGL_FONT_DIR = os.path.join(
    PROJECT_ROOT, 'managed_components', 'lvgl__lvgl', 'scripts', 'built_in_font'
)
OUTPUT_C = os.path.join(UI_DIR, 'ui_font_custom.c')
OUTPUT_LARGE_C = os.path.join(UI_DIR, 'ui_font_custom_large.c')
# Large font is only used for the focus timer, keep its glyph set tiny.
LARGE_CJK = '专注'
LARGE_SIZE = 40

# Extra common CJK characters for user names and additional UI text.
# These are added to the glyphs collected from the UI source files so that
# arbitrary registered names (e.g. 小胡) render correctly instead of showing
# tofu squares.
EXTRA_CJK = (
    '的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动'
    '同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自'
    '二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日'
    '那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变'
    '条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员代位入常文总'
    '次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指'
    '几九区强放决西被干做必队先常回委色气六即病叹会错离呢第千复台则让像'
    '王李张刘陈杨黄赵周吴徐孙马朱胡郭何高林罗郑梁谢宋唐许韩冯邓曹彭曾肖田董潘袁蔡蒋丁'
    '余于杜叶程苏魏吕任沈姚卢姜崔钟谭陆汪范金石廖贾夏付方白邹孟熊秦邱江尹薛阎段雷侯龙'
    '史黎贺顾毛郝龚邵万钱严覃武戴莫孔向汤常潘'
    '伟芳娜敏静丽强磊军洋勇艳杰娟涛明超秀兰霞平刚桂英华辉燕玲鹏梅鑫倩帅宇佳婷淑慧峰建'
    '丹宁琴君龙超明霞兰秀国林'
)


def collect_cjk(text):
    return ''.join(sorted({ch for ch in text if '\u4e00' <= ch <= '\u9fff'}))


def main():
    text = ''
    for root, _dirs, files in os.walk(UI_DIR):
        for f in files:
            if f.endswith(('.c', '.h')):
                with open(os.path.join(root, f), 'r', encoding='utf-8', errors='ignore') as fp:
                    text += fp.read()

    symbols = collect_cjk(text + EXTRA_CJK)
    print(f'Collected {len(symbols)} unique CJK characters for UI font')
    if not symbols:
        print('No CJK characters found, nothing to do')
        return 0

    symbols += EXTRA_CJK
    symbols = ''.join(sorted({ch for ch in symbols if '\u4e00' <= ch <= '\u9fff'}))
    print(f'Final UI font CJK glyph count: {len(symbols)}')

    lv_font_conv = os.path.join(
        PROJECT_ROOT, 'tools_temp', 'node_modules', '.bin', 'lv_font_conv.cmd'
    )
    if not os.path.exists(lv_font_conv):
        print(f'lv_font_conv not found: {lv_font_conv}')
        return 1

    # FontAwesome built-in symbols used by LVGL default themes/widgets.
    syms = (
        "61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,"
        "61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,"
        "61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,"
        "61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,"
        "61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,"
        "62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"
    )

    cmd = [
        lv_font_conv,
        '--no-compress',
        '--no-prefilter',
        '--lv-include', 'lvgl.h',
        '--bpp', '4',
        '--size', '16',
        '--font', os.path.join(LVGL_FONT_DIR, 'SourceHanSansSC-Normal.otf'),
        '-r', '0x20-0x7F,0x3000-0x303F,0xFF01-0xFF5E',
        '--symbols', symbols,
        '--font', os.path.join(LVGL_FONT_DIR, 'FontAwesome5-Solid+Brands+Regular.woff'),
        '-r', syms,
        '--format', 'lvgl',
        '-o', OUTPUT_C,
        '--force-fast-kern-format',
    ]

    print(f'Writing font to {OUTPUT_C}')
    subprocess.run(cmd, check=True)

    # Large font for the focus timer (digits/colon + the few CJK used there).
    large_cmd = [
        lv_font_conv,
        '--no-compress',
        '--no-prefilter',
        '--lv-include', 'lvgl.h',
        '--bpp', '4',
        '--size', str(LARGE_SIZE),
        '--font', os.path.join(LVGL_FONT_DIR, 'SourceHanSansSC-Normal.otf'),
        '-r', '0x30-0x3A',
        '--symbols', LARGE_CJK,
        '--format', 'lvgl',
        '-o', OUTPUT_LARGE_C,
        '--force-fast-kern-format',
    ]
    print(f'Writing large font to {OUTPUT_LARGE_C}')
    subprocess.run(large_cmd, check=True)
    print('Done')
    return 0


if __name__ == '__main__':
    sys.exit(main())
