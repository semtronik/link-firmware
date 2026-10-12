/*******************************************************************************
 * Size: 20 px
 * Bpp: 4
 * Opts: --font MaterialIcons-Regular.ttf -r 0xE163 --size 20 --bpp 4 --format lvgl --lv-include lvgl.h --no-compress -o link_icons_20.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef LINK_ICONS_20
#define LINK_ICONS_20 1
#endif

#if LINK_ICONS_20

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E163 "" */
    0x22, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x5, 0xfb, 0x40, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x5f, 0xff, 0xd6, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x5, 0xff, 0xff, 0xff,
    0x92, 0x0, 0x0, 0x0, 0x0, 0x0, 0x5f, 0xff,
    0xff, 0xff, 0xfb, 0x40, 0x0, 0x0, 0x0, 0x5,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xd6, 0x0, 0x0,
    0x0, 0x16, 0x8b, 0xdf, 0xff, 0xff, 0xff, 0xff,
    0x92, 0x0, 0x0, 0x0, 0x0, 0x1, 0x35, 0x8a,
    0xce, 0xff, 0xfb, 0x40, 0x0, 0x0, 0x0, 0x13,
    0x58, 0xac, 0xef, 0xff, 0xb4, 0x1, 0x69, 0xbd,
    0xff, 0xff, 0xff, 0xff, 0xf9, 0x20, 0x0, 0x5f,
    0xff, 0xff, 0xff, 0xff, 0xfd, 0x70, 0x0, 0x0,
    0x5, 0xff, 0xff, 0xff, 0xff, 0xb4, 0x0, 0x0,
    0x0, 0x0, 0x5f, 0xff, 0xff, 0xf9, 0x20, 0x0,
    0x0, 0x0, 0x0, 0x5, 0xff, 0xfd, 0x70, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x5f, 0xb4, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x20,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 19, .box_h = 16, .ofs_x = 1, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 57699, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t link_icons_20 = {
#else
lv_font_t link_icons_20 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = -2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -5,
    .underline_thickness = 2,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LINK_ICONS_20*/

