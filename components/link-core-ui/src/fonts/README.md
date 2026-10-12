# Fonts

`link_font_12/14/16/28.c`: Montserrat Medium (OFL) with ASCII, Latin-1 and
a few typographic marks, merged with the FontAwesome glyphs behind LVGL's
`LV_SYMBOL_*`. Without that merge every symbol in the UI disappears.

Licences: Montserrat and the Font Awesome Free glyphs are under the SIL
Open Font License 1.1; Material Icons under the Apache License 2.0.

Generated with [lv_font_conv](https://github.com/lvgl/lv_font_conv), from
the font files LVGL ships in `scripts/built_in_font/`, with the options of
LVGL's own `built_in_font_gen.py` and a wider range. That script cannot
start `lv_font_conv` on Windows, so it is called directly, from that
directory:

    npm install lv_font_conv
    SYMS=61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650
    for s in 12 14 16 28; do
      node node_modules/lv_font_conv/lv_font_conv.js \
        --no-compress --no-prefilter --bpp 4 --size $s \
        --font Montserrat-Medium.ttf \
        -r 0x20-0x7F,0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2019,0x2022,0x2026,0x20AC,0x2265 \
        --font FontAwesome5-Solid+Brands+Regular.woff -r $SYMS \
        --format lvgl --lv-include lvgl.h --force-fast-kern-format \
        -o link_font_$s.c
    done

`SYMS` is the list in `built_in_font_gen.py`; take it from there if LVGL
adds symbols.

`link_icons_20.c`: Material Icons (Apache 2.0); its command is at the top
of the file.
