/*
 * Factory reset confirmation. Full-screen, what goes is spelled out, and
 * the erase is its own red button rather than the header's validate: a
 * tap that wipes the unit should not look like every other OK.
 */

#include "screens_text.h"
#include "esp_log.h"
#include "link_factory.h"
#include "ui_helpers.h"
#include "link_screens.h"
#include "link_theme.h"

static const char *TAG = "ui_factory";

#define BTN_H 52
#define BTN_Y (LINK_USABLE_Y + LINK_USABLE_H - BTN_H - LINK_PAD_SM)

/* Wider than the theme's pads: the icon reads as a mark beside the text,
 * not as its first word. */
#define ICON_GAP 24

static lv_obj_t *s_overlay;

static void close_cb(void *arg)
{
    (void)arg;

    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
}

static void on_cancel(void *user_data)
{
    (void)user_data;
    lv_async_call(close_cb, NULL);
}

static void on_erase(void *user_data)
{
    (void)user_data;
    ESP_LOGW(TAG, "confirmed");

    /* No teardown: the device restarts. */
    link_factory_reset();
}

static lv_obj_t *button(const char *text, int32_t x, int32_t w, bool danger,
                        ui_touch_tap_cb_t on_tap)
{
    lv_obj_t *btn = ui_touch_make_tap_button(s_overlay, on_tap, NULL);
    lv_obj_add_style(btn, &link_style_btn, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    if (danger) {
        lv_obj_set_style_bg_color(btn, LINK_COLOR_ERROR, 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
    }
    lv_obj_set_size(btn, w, BTN_H);
    lv_obj_set_pos(btn, x, BTN_Y);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_center(lbl);

    return btn;
}

void ui_screen_factory_reset_open(void)
{
    if (s_overlay != NULL) {
        return;
    }

    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui_helpers_attach_overlay_header(s_overlay, st(ST_FACTORY_RESET_Q),
                                     on_cancel, NULL, NULL, NULL);

    /* Icon and text side by side, both centred in the band between the
     * header and the buttons: the screen is wider than it is tall. */
    const int32_t band_h = BTN_Y - LINK_CONTENT_Y;
    lv_obj_t *band = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, LINK_USABLE_W, band_h);
    lv_obj_set_pos(band, LINK_USABLE_X, LINK_CONTENT_Y);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(band, ICON_GAP, 0);

    lv_obj_t *icon = lv_label_create(band);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, LINK_FONT_HUGE, 0);
    lv_obj_set_style_text_color(icon, LINK_COLOR_ERROR, 0);

    lv_obj_t *hint = lv_label_create(band);
    lv_label_set_text(hint, st(ST_FACTORY_RESET_HINT));
    lv_obj_set_style_text_font(hint, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(hint, LINK_COLOR_TEXT_DIM, 0);

    const int32_t half = (LINK_USABLE_W - LINK_PAD_MD) / 2;
    button(st(ST_CANCEL), LINK_USABLE_X, half, false, on_cancel);
    button(st(ST_ERASE), LINK_USABLE_X + half + LINK_PAD_MD, half, true,
           on_erase);

    ESP_LOGI(TAG, "opened");
}
