/**
 * @file link_theme.c
 * @brief Shared palette and styles for link screens.
 */

#include "link_theme.h"

#include "esp_log.h"

static const char *TAG = "link_theme";

lv_color_t g_link_color_bg;
lv_color_t g_link_color_surface;
lv_color_t g_link_color_surface_elevated;
lv_color_t g_link_color_border;
lv_color_t g_link_color_text;
lv_color_t g_link_color_text_dim;
lv_color_t g_link_color_accent;
lv_color_t g_link_color_accent_fg;
lv_color_t g_link_color_success;
lv_color_t g_link_color_warning;
lv_color_t g_link_color_error;
lv_color_t g_link_color_text_on_accent;

static bool s_is_light;

lv_style_t link_style_screen;
lv_style_t link_style_btn;
lv_style_t link_style_btn_pressed;
lv_style_t link_style_btn_accent;
lv_style_t link_style_btn_accent_pressed;
lv_style_t link_style_btn_big;
lv_style_t link_style_btn_signature;
lv_style_t link_style_list_row;
lv_style_t link_style_list_container;
lv_style_t link_style_header;
lv_style_t link_style_header_btn;
lv_style_t link_style_status_bar;
lv_style_t link_style_pager_dot_active;
lv_style_t link_style_pager_dot_inactive;
lv_style_t link_style_radio_active;
lv_style_t link_style_radio_inactive;

lv_style_t link_style_page;
lv_style_t link_style_card;
lv_style_t link_style_title;
lv_style_t link_style_label;
lv_style_t link_style_value;
lv_style_t link_style_huge;
lv_style_t link_style_dot_active;
lv_style_t link_style_dot_idle;

/* Tailwind Slate in dark; the light side is tuned for a 2.8" panel in
 * daylight, where the Slate values washed out. */
static void load_palette(bool light)
{
    if (light) {
        /* Values are picked for where RGB565 lands them, not for their
         * sRGB spelling: a cast can appear at quantisation. */
        g_link_color_bg               = lv_color_hex(0xEFEFEF);
        g_link_color_surface          = lv_color_hex(0xFFFFFF);
        g_link_color_surface_elevated = lv_color_hex(0xEBEBEB);
        g_link_color_border           = lv_color_hex(0xB6C2D1);
        g_link_color_text             = lv_color_hex(0x0F172A);
        g_link_color_text_dim         = lv_color_hex(0x8A9AAB);
        g_link_color_success          = lv_color_hex(0x16A34A);
        g_link_color_warning          = lv_color_hex(0xEA580C);
        g_link_color_error            = lv_color_hex(0xC81E1E);
    } else {
        g_link_color_bg               = lv_color_hex(0x0F172A);  /* slate-900 */
        g_link_color_surface          = lv_color_hex(0x1E293B);  /* slate-800 */
        g_link_color_surface_elevated = lv_color_hex(0x334155);  /* slate-700 */
        g_link_color_border           = lv_color_hex(0x475569);  /* slate-600 */
        g_link_color_text             = lv_color_hex(0xF1F5F9);  /* slate-100 */
        g_link_color_text_dim         = lv_color_hex(0x94A3B8);  /* slate-400 */
        g_link_color_success          = lv_color_hex(0x4ADE80);  /* green-400 */
        g_link_color_warning          = lv_color_hex(0xFBBF24);  /* amber-400 */
        g_link_color_error            = lv_color_hex(0xF87171);  /* red-400   */
    }

    /*
     * ACCENT fills a surface, ACCENT_FG draws a stroke or a word. Both are
     * the brand blue: darkening ACCENT_FG for the light palette measured
     * better and looked worse on the panel, which is backlit and whose
     * blacks sit well above the sRGB the ratios assume.
     */
    g_link_color_accent = lv_color_hex(0x47ABFF);
    g_link_color_accent_fg = lv_color_hex(0x47ABFF);

    /* Dark on the accent either way: the brand blue is light enough that
     * white text on it fails at any size the screens use. */
    g_link_color_text_on_accent = lv_color_hex(0x0F172A);
}

static void init_all_styles(void)
{
    lv_style_init(&link_style_screen);
    lv_style_set_bg_color(&link_style_screen, LINK_COLOR_BG);
    lv_style_set_bg_opa(&link_style_screen, LV_OPA_COVER);
    lv_style_set_text_color(&link_style_screen, LINK_COLOR_TEXT);
    lv_style_set_text_font(&link_style_screen, LINK_FONT_BODY);
    lv_style_set_pad_all(&link_style_screen, 0);
    lv_style_set_border_width(&link_style_screen, 0);
    lv_style_set_radius(&link_style_screen, 0);

    lv_style_init(&link_style_btn);
    lv_style_set_bg_color(&link_style_btn, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_btn, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_btn, LINK_COLOR_BORDER);
    lv_style_set_border_width(&link_style_btn, 1);
    lv_style_set_radius(&link_style_btn, LINK_RADIUS_BTN);
    lv_style_set_text_color(&link_style_btn, LINK_COLOR_TEXT);
    lv_style_set_text_font(&link_style_btn, LINK_FONT_BODY);
    lv_style_set_pad_hor(&link_style_btn, LINK_PAD_MD);
    lv_style_set_pad_ver(&link_style_btn, LINK_PAD_SM);

    lv_style_init(&link_style_btn_pressed);
    lv_style_set_bg_color(&link_style_btn_pressed, LINK_COLOR_BORDER);
    lv_style_set_bg_opa(&link_style_btn_pressed, LV_OPA_COVER);

    lv_style_init(&link_style_btn_accent);
    lv_style_set_bg_color(&link_style_btn_accent, LINK_COLOR_ACCENT);
    lv_style_set_bg_opa(&link_style_btn_accent, LV_OPA_COVER);
    lv_style_set_border_width(&link_style_btn_accent, 0);
    lv_style_set_radius(&link_style_btn_accent, LINK_RADIUS_BTN);
    lv_style_set_text_color(&link_style_btn_accent, LINK_COLOR_TEXT_ON_ACCENT);
    lv_style_set_text_font(&link_style_btn_accent, LINK_FONT_TITLE);

    lv_style_init(&link_style_btn_accent_pressed);
    lv_style_set_bg_opa(&link_style_btn_accent_pressed, LV_OPA_70);

    lv_style_init(&link_style_btn_big);
    lv_style_set_bg_color(&link_style_btn_big, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_btn_big, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_btn_big, LINK_COLOR_ACCENT);
    lv_style_set_border_width(&link_style_btn_big, 2);
    lv_style_set_radius(&link_style_btn_big, LINK_RADIUS_BTN_BIG);
    lv_style_set_text_color(&link_style_btn_big, LINK_COLOR_ACCENT_FG);
    lv_style_set_text_font(&link_style_btn_big, LINK_FONT_HUGE);

    lv_style_init(&link_style_btn_signature);
    lv_style_set_bg_color(&link_style_btn_signature, LINK_COLOR_ACCENT);
    lv_style_set_bg_opa(&link_style_btn_signature, LV_OPA_COVER);
    lv_style_set_border_width(&link_style_btn_signature, 0);
    lv_style_set_radius(&link_style_btn_signature, LINK_RADIUS_BTN_BIG);
    lv_style_set_text_color(&link_style_btn_signature,
                            LINK_COLOR_TEXT_ON_ACCENT);
    lv_style_set_text_font(&link_style_btn_signature, LINK_FONT_TITLE);
    lv_style_set_pad_all(&link_style_btn_signature, 0);

    lv_style_init(&link_style_list_row);
    lv_style_set_bg_color(&link_style_list_row, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_list_row, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_list_row, LINK_COLOR_BORDER);
    lv_style_set_border_width(&link_style_list_row, 1);
    lv_style_set_border_side(&link_style_list_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_radius(&link_style_list_row, 0);
    lv_style_set_pad_hor(&link_style_list_row, LINK_PAD_LG);
    lv_style_set_pad_ver(&link_style_list_row, 0);
    lv_style_set_text_color(&link_style_list_row, LINK_COLOR_TEXT);
    lv_style_set_text_font(&link_style_list_row, LINK_FONT_BODY);

    lv_style_init(&link_style_list_container);
    lv_style_set_bg_color(&link_style_list_container, LINK_COLOR_BG);
    lv_style_set_bg_opa(&link_style_list_container, LV_OPA_COVER);
    lv_style_set_pad_all(&link_style_list_container, 0);
    lv_style_set_pad_left(&link_style_list_container, LINK_BEZEL_MARGIN);
    lv_style_set_border_width(&link_style_list_container, 0);

    lv_style_init(&link_style_header);
    lv_style_set_bg_color(&link_style_header, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_header, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_header, LINK_COLOR_BORDER);
    lv_style_set_border_width(&link_style_header, 1);
    lv_style_set_border_side(&link_style_header, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_radius(&link_style_header, 0);
    lv_style_set_pad_all(&link_style_header, 0);

    /*
     * No fill at rest: the symbol alone is the button, and
     * link_style_btn_pressed puts a plate back under the finger.
     *
     * A SURFACE plate on a SURFACE bar was a grey on a barely different
     * grey, and its edges were what made the button look like it filled
     * the band whatever the margin around it. Dropping it also frees the
     * target to be as large as the band without looking it - the tap
     * area is the whole 42 px, the mark in it is one glyph.
     */
    lv_style_init(&link_style_header_btn);
    lv_style_set_bg_opa(&link_style_header_btn, LV_OPA_TRANSP);
    lv_style_set_border_width(&link_style_header_btn, 0);
    lv_style_set_radius(&link_style_header_btn, LINK_RADIUS_CARD);
    lv_style_set_text_color(&link_style_header_btn, LINK_COLOR_TEXT_DIM);
    lv_style_set_text_font(&link_style_header_btn, LINK_FONT_TITLE);
    lv_style_set_pad_all(&link_style_header_btn, 0);

    lv_style_init(&link_style_status_bar);
    lv_style_set_bg_color(&link_style_status_bar, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_status_bar, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_status_bar, LINK_COLOR_BORDER);
    lv_style_set_border_width(&link_style_status_bar, 1);
    lv_style_set_border_side(&link_style_status_bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_radius(&link_style_status_bar, 0);
    lv_style_set_pad_hor(&link_style_status_bar, LINK_PAD_LG);
    lv_style_set_pad_ver(&link_style_status_bar, 0);

    /* The active dot is plain white, not the accent: the accent belongs
     * to the signature button and reads as "press me". */
    lv_style_init(&link_style_pager_dot_active);
    lv_style_set_bg_color(&link_style_pager_dot_active, LINK_COLOR_TEXT);
    lv_style_set_bg_opa(&link_style_pager_dot_active, LV_OPA_COVER);
    lv_style_set_border_width(&link_style_pager_dot_active, 0);
    lv_style_set_radius(&link_style_pager_dot_active, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&link_style_pager_dot_active, 0);

    lv_style_init(&link_style_pager_dot_inactive);
    lv_style_set_bg_color(&link_style_pager_dot_inactive, LINK_COLOR_TEXT_DIM);
    lv_style_set_bg_opa(&link_style_pager_dot_inactive, LINK_OPA_INERT);
    lv_style_set_border_width(&link_style_pager_dot_inactive, 0);
    lv_style_set_radius(&link_style_pager_dot_inactive, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&link_style_pager_dot_inactive, 0);

    lv_style_init(&link_style_radio_active);
    lv_style_set_bg_color(&link_style_radio_active, LINK_COLOR_ACCENT);
    lv_style_set_bg_opa(&link_style_radio_active, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_radio_active, LINK_COLOR_ACCENT);
    lv_style_set_border_width(&link_style_radio_active, 2);
    lv_style_set_radius(&link_style_radio_active, LV_RADIUS_CIRCLE);

    lv_style_init(&link_style_radio_inactive);
    lv_style_set_bg_color(&link_style_radio_inactive, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_radio_inactive, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_radio_inactive, LINK_COLOR_TEXT_DIM);
    lv_style_set_border_width(&link_style_radio_inactive, 2);
    lv_style_set_radius(&link_style_radio_inactive, LV_RADIUS_CIRCLE);

    /* Page: fills the panel, no scrolling of its own - the pager owns
     * horizontal movement, and a scrollable page would fight it. */
    lv_style_init(&link_style_page);
    lv_style_set_bg_color(&link_style_page, LINK_COLOR_BG);
    lv_style_set_bg_opa(&link_style_page, LV_OPA_COVER);
    lv_style_set_border_width(&link_style_page, 0);
    lv_style_set_radius(&link_style_page, 0);
    lv_style_set_pad_all(&link_style_page, 0);
    lv_style_set_text_color(&link_style_page, LINK_COLOR_TEXT);
    lv_style_set_text_font(&link_style_page, LINK_FONT_BODY);

    lv_style_init(&link_style_card);
    lv_style_set_bg_color(&link_style_card, LINK_COLOR_SURFACE);
    lv_style_set_bg_opa(&link_style_card, LV_OPA_COVER);
    lv_style_set_border_color(&link_style_card, LINK_COLOR_BORDER);
    lv_style_set_border_width(&link_style_card, 1);
    lv_style_set_radius(&link_style_card, LINK_RADIUS_CARD);
    lv_style_set_pad_all(&link_style_card, LINK_PAD_MD);

    lv_style_init(&link_style_title);
    lv_style_set_text_color(&link_style_title, LINK_COLOR_ACCENT_FG);
    lv_style_set_text_font(&link_style_title, LINK_FONT_TITLE);

    /* Row label: the field name, deliberately quieter than its value. */
    lv_style_init(&link_style_label);
    lv_style_set_text_color(&link_style_label, LINK_COLOR_TEXT_DIM);
    lv_style_set_text_font(&link_style_label, LINK_FONT_CAPTION);

    lv_style_init(&link_style_value);
    lv_style_set_text_color(&link_style_value, LINK_COLOR_TEXT);
    lv_style_set_text_font(&link_style_value, LINK_FONT_BODY);

    lv_style_init(&link_style_huge);
    lv_style_set_text_color(&link_style_huge, LINK_COLOR_TEXT);
    lv_style_set_text_font(&link_style_huge, LINK_FONT_HUGE);

    lv_style_init(&link_style_dot_active);
    lv_style_set_bg_color(&link_style_dot_active, LINK_COLOR_ACCENT);
    lv_style_set_bg_opa(&link_style_dot_active, LV_OPA_COVER);
    lv_style_set_border_width(&link_style_dot_active, 0);
    lv_style_set_radius(&link_style_dot_active, LV_RADIUS_CIRCLE);

    lv_style_init(&link_style_dot_idle);
    lv_style_set_bg_color(&link_style_dot_idle, LINK_COLOR_TEXT_DIM);
    lv_style_set_bg_opa(&link_style_dot_idle, LINK_OPA_INERT);
    lv_style_set_border_width(&link_style_dot_idle, 0);
    lv_style_set_radius(&link_style_dot_idle, LV_RADIUS_CIRCLE);
}

void link_theme_init(void)
{
    load_palette(s_is_light);
    init_all_styles();
    ESP_LOGI(TAG, "theme=%s", s_is_light ? "light" : "dark");
}

void link_theme_set_light(bool light)
{
    s_is_light = light;
}

bool link_theme_is_light(void)
{
    return s_is_light;
}

lv_color_t link_theme_state_color(bool on)
{
    return on ? LINK_COLOR_SUCCESS : LINK_COLOR_TEXT_DIM;
}
