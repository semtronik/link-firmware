/**
 * @file link_theme.h
 * @brief Palette, geometry and styles shared by every link screen.
 *
 * The palette is a set of variables rather than constants: light and
 * dark are swapped at runtime, and a constant could not follow. Read it
 * through the LINK_COLOR_* macros below, never by assigning to the
 * variables directly.
 *
 * Geometry does not depend on the theme and stays constant.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime palette, written by link_theme_init(). */
extern lv_color_t g_link_color_bg;
extern lv_color_t g_link_color_surface;
extern lv_color_t g_link_color_surface_elevated;
extern lv_color_t g_link_color_border;
extern lv_color_t g_link_color_text;
extern lv_color_t g_link_color_text_dim;
extern lv_color_t g_link_color_accent;
extern lv_color_t g_link_color_accent_fg;
extern lv_color_t g_link_color_success;
extern lv_color_t g_link_color_warning;
extern lv_color_t g_link_color_error;
extern lv_color_t g_link_color_text_on_accent;

#define LINK_COLOR_BG               g_link_color_bg
#define LINK_COLOR_SURFACE          g_link_color_surface
#define LINK_COLOR_SURFACE_ELEVATED g_link_color_surface_elevated
#define LINK_COLOR_BORDER           g_link_color_border
#define LINK_COLOR_TEXT             g_link_color_text
#define LINK_COLOR_TEXT_DIM         g_link_color_text_dim
#define LINK_COLOR_ACCENT           g_link_color_accent
#define LINK_COLOR_ACCENT_FG        g_link_color_accent_fg
#define LINK_COLOR_SUCCESS          g_link_color_success
#define LINK_COLOR_WARNING          g_link_color_warning
#define LINK_COLOR_ERROR            g_link_color_error
#define LINK_COLOR_TEXT_ON_ACCENT   g_link_color_text_on_accent

/* Greyed out: not a colour, the live colour faded. Same ratio both themes. */
#define LINK_OPA_INERT LV_OPA_30

/* Geometry, landscape. */
#define LINK_SCREEN_W 320
#define LINK_SCREEN_H 240

/*
 * The enclosure bezel covers the left, right and bottom edges; the top
 * one has no dead zone, which buys back 8 px. Nothing readable or
 * touchable may sit outside the usable area below.
 */
#define LINK_BEZEL_MARGIN     8
#define LINK_BEZEL_MARGIN_TOP 0

#define LINK_USABLE_X LINK_BEZEL_MARGIN
#define LINK_USABLE_Y LINK_BEZEL_MARGIN_TOP
#define LINK_USABLE_W (LINK_SCREEN_W - 2 * LINK_BEZEL_MARGIN)  /* 304 */
#define LINK_USABLE_H (LINK_SCREEN_H - LINK_BEZEL_MARGIN_TOP - LINK_BEZEL_MARGIN)  /* 232 */

/* Page indicator, pinned to the bottom of the usable area. */
#define LINK_PAGER_H        14
#define LINK_PAGER_DOT_SIZE 8
#define LINK_PAGER_DOT_GAP  12
#define LINK_PAGER_TOUCH_TGT 44

/* What a page may fill without scrolling. A page must fit: a scrollable
 * child would swallow the horizontal swipe that moves between pages. */
#define LINK_PAGE_CONTENT_W LINK_USABLE_W                    /* 304 */
#define LINK_PAGE_CONTENT_H (LINK_USABLE_H - LINK_PAGER_H)   /* 218 */

#define LINK_PAD_XS 2
#define LINK_PAD_SM 4
#define LINK_PAD_MD 8
#define LINK_PAD_LG 12
#define LINK_PAD_XL 16

/* Montserrat with Latin-1 and LVGL's symbols; how they are generated is
 * in src/fonts/README.md. */
LV_FONT_DECLARE(link_font_12)
LV_FONT_DECLARE(link_font_14)
LV_FONT_DECLARE(link_font_16)
LV_FONT_DECLARE(link_font_28)

#define LINK_FONT_CAPTION (&link_font_12)
#define LINK_FONT_BODY    (&link_font_14)
#define LINK_FONT_TITLE   (&link_font_16)
#define LINK_FONT_VALUE   LINK_FONT_TITLE
#define LINK_FONT_HUGE    (&link_font_28)

/* Material Icons (Apache 2.0). 20 px to match LINK_FONT_TITLE's symbols;
 * the lv_font_conv command is at the top of src/fonts/link_icons_20.c. */
LV_FONT_DECLARE(link_icons_20)
#define LINK_FONT_ICONS   (&link_icons_20)

#define LINK_SYMBOL_SEND  "\xEE\x85\xA3" /* U+E163 */

/*
 * Header: 42 px, and the buttons fill it.
 *
 * They can, because they have no fill of their own any more - a header
 * button is its symbol and a transparent tap area around it (see
 * link_style_header_btn). Nothing shows an edge, so there is no margin
 * to get right, and the target is free to be the whole band: 42x42,
 * which is back at the touch floor the rest of the product holds to.
 *
 * Sizing the band and sizing the mark in it stopped being the same
 * question here. This is the band; the mark is LINK_FONT_TITLE.
 */
#define LINK_HEADER_H      48
#define LINK_HEADER_BTN_SZ 42
#define LINK_CONTENT_Y     (LINK_USABLE_Y + LINK_HEADER_H)
#define LINK_CONTENT_H     (LINK_USABLE_H - LINK_HEADER_H)

/*
 * Height of a view's action bar. Same as the header, and sized the same
 * way - a 36 px button with 3 px above and below.
 *
 * The bar belongs to the view, not to the screen: it slides away with
 * the page it acts on. So it is NOT subtracted below - a view runs from
 * under the header to the bottom of the glass and puts its own bar at
 * its foot.
 */
#define LINK_FOOTER_H 42

#define LINK_MONITOR_CONTENT_H \
    (LINK_SCREEN_H - LINK_USABLE_Y - LINK_HEADER_H)

/* Same as every other button now; kept as its own name because the call
 * sites mean the signature button, not a header button that happens to
 * match. */
#define LINK_SIGNATURE_BTN_SZ LINK_HEADER_BTN_SZ

/* Rows are a fixed height so the rhythm is the same on every screen: the
 * smallest touch target the product allows. */
#define LINK_LIST_ROW_H_MIN 42
#define LINK_LIST_GAP       2

#define LINK_RADIUS_BTN     4
#define LINK_RADIUS_CARD    6
#define LINK_RADIUS_BTN_BIG 8

/* Application chrome. */
extern lv_style_t link_style_screen;
extern lv_style_t link_style_btn;
extern lv_style_t link_style_btn_pressed;
extern lv_style_t link_style_btn_accent;
extern lv_style_t link_style_btn_accent_pressed;
extern lv_style_t link_style_btn_big;
extern lv_style_t link_style_btn_signature;
extern lv_style_t link_style_list_row;
extern lv_style_t link_style_list_container;
extern lv_style_t link_style_header;
extern lv_style_t link_style_header_btn;
extern lv_style_t link_style_status_bar;
extern lv_style_t link_style_pager_dot_active;
extern lv_style_t link_style_pager_dot_inactive;
extern lv_style_t link_style_radio_active;
extern lv_style_t link_style_radio_inactive;

/* Paged layouts: a full-panel page, cards, and the text scale that goes
 * with them. */
extern lv_style_t link_style_page;
extern lv_style_t link_style_card;
extern lv_style_t link_style_title;
extern lv_style_t link_style_label;
extern lv_style_t link_style_value;
extern lv_style_t link_style_huge;
extern lv_style_t link_style_dot_active;
extern lv_style_t link_style_dot_idle;

/**
 * @brief Load the palette and build every style.
 *
 * Call once after lv_init() and before creating any screen. Calling it
 * again rebuilds the styles against the current palette, which is how a
 * theme switch takes effect - see link_theme_set_light().
 */
void link_theme_init(void);

/**
 * @brief Choose light or dark.
 *
 * Records the choice but does not rebuild anything: live objects still
 * point at the current styles, and rebuilding under them corrupts the
 * display list. The caller destroys the screens, calls link_theme_init(),
 * then builds them again.
 *
 * Storing the choice is the caller's business.
 */
void link_theme_set_light(bool light);

/** @brief True when the light palette is selected. */
bool link_theme_is_light(void);

/**
 * @brief Colour for a boolean state: success when true, muted when false.
 *
 * For pass/fail indicators, where "false" means inactive rather than
 * wrong.
 */
lv_color_t link_theme_state_color(bool on);

#ifdef __cplusplus
}
#endif
