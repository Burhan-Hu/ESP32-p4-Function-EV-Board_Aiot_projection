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


def collect_cjk(text):
    return ''.join(sorted({ch for ch in text if '\u4e00' <= ch <= '\u9fff'}))


def main():
    text = ''
    for root, _dirs, files in os.walk(UI_DIR):
        for f in files:
            if f.endswith(('.c', '.h')):
                with open(os.path.join(root, f), 'r', encoding='utf-8', errors='ignore') as fp:
                    text += fp.read()

    symbols = collect_cjk(text)
    print(f'Collected {len(symbols)} unique CJK characters for UI font')
    if not symbols:
        print('No CJK characters found, nothing to do')
        return 0

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
        '--bpp', '4',
        '--size', '16',
        '--font', os.path.join(LVGL_FONT_DIR, 'SourceHanSansSC-Normal.otf'),
        '-r', '0x20-0x7F',
        '--symbols', symbols,
        '--font', os.path.join(LVGL_FONT_DIR, 'FontAwesome5-Solid+Brands+Regular.woff'),
        '-r', syms,
        '--format', 'lvgl',
        '-o', OUTPUT_C,
        '--force-fast-kern-format',
    ]

    print(f'Writing font to {OUTPUT_C}')
    subprocess.run(cmd, check=True)
    print('Done')
    return 0


if __name__ == '__main__':
    sys.exit(main())
