/*
 * The two battery overlays. Both are full-screen and unmissable - no
 * discreet banner, no dismissing gesture - and they share a layout, so
 * they share a builder.
 *
 * The low-battery one has a single OK button. The critical one has no
 * way out: it shows what is about to happen, then cuts power before the
 * cell's own protection drops the rail mid-write.
 */

#include "screens_text.h"
#include "esp_log.h"
#include "link_battery.h"
#include "power_key.h"
#include "ui_helpers.h"
#include "link_screens.h"
#include "link_theme.h"

static const char *TAG = "ui_battery";

/* Long enough to read, short enough not to spend what little charge is
 * left showing it. */
#define CRITICAL_DISPLAY_MS 3000

#define OK_BTN_H 56

static lv_obj_t *s_alert;
static lv_obj_t *s_critical;

static uint8_t battery_pct(void)
{
    link_battery_t bat;
    link_battery_get(&bat);

    return bat.gauge_ok ? bat.soc_pct : 0;
}

/* Icon, title, the figure itself, and a line saying what it means. */
static lv_obj_t *build_overlay(const char *title, const char *hint)
{
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(overlay);
    lv_label_set_text(icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_font(icon, LINK_FONT_HUGE, 0);
    lv_obj_set_style_text_color(icon, LINK_COLOR_ERROR, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, LINK_USABLE_Y + LINK_PAD_LG * 2);

    lv_obj_t *lbl_title = lv_label_create(overlay);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl_title, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_letter_space(lbl_title, 2, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0,
                 LINK_USABLE_Y + LINK_PAD_LG * 2 + LINK_FONT_HUGE->line_height
                     + LINK_PAD_MD);

    lv_obj_t *pct = lv_label_create(overlay);
    lv_label_set_text_fmt(pct, "%u %%", (unsigned)battery_pct());
    lv_obj_set_style_text_font(pct, LINK_FONT_HUGE, 0);
    lv_obj_set_style_text_color(pct, LINK_COLOR_ERROR, 0);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *lbl_hint = lv_label_create(overlay);
    lv_label_set_text(lbl_hint, hint);
    lv_obj_set_style_text_font(lbl_hint, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl_hint, LINK_COLOR_TEXT, 0);
    lv_obj_align(lbl_hint, LV_ALIGN_CENTER, 0,
                 LINK_FONT_HUGE->line_height / 2 + LINK_PAD_MD);

    return overlay;
}

static void alert_close_cb(void *arg)
{
    (void)arg;

    if (s_alert != NULL) {
        lv_obj_delete(s_alert);
        s_alert = NULL;
    }
}

static void on_ok_tapped(void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "low-battery alert acknowledged");

    app_ui_battery_alert_acknowledge();
    lv_async_call(alert_close_cb, NULL);
}

bool ui_screen_battery_alert_is_open(void)
{
    return s_alert != NULL;
}

void ui_screen_battery_alert_open(void)
{
    if (s_alert != NULL) {
        return;
    }

    s_alert = build_overlay(st(ST_BATTERY_LOW), st(ST_BATTERY_LOW_HINT));

    /* Full width along the bottom: one way out, impossible to miss. */
    lv_obj_t *btn = ui_touch_make_tap_button(s_alert, on_ok_tapped, NULL);
    lv_obj_set_size(btn, LINK_USABLE_W, OK_BTN_H);
    lv_obj_set_pos(btn, LINK_USABLE_X, LINK_SCREEN_H - LINK_BEZEL_MARGIN - OK_BTN_H);
    lv_obj_set_style_bg_color(btn, LINK_COLOR_ERROR, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LINK_RADIUS_BTN_BIG, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, st(ST_OK));
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_center(lbl);

    ESP_LOGW(TAG, "low battery: %u%%", (unsigned)battery_pct());
}

static void shutdown_timer_cb(lv_timer_t *timer)
{
    lv_timer_delete(timer);

    /* Cuts the rail: nothing after this runs on battery. */
    power_key_power_off("battery critical");
}

bool ui_screen_battery_critical_is_open(void)
{
    return s_critical != NULL;
}

void ui_screen_battery_critical_open(void)
{
    if (s_critical != NULL) {
        return;
    }

    s_critical = build_overlay(st(ST_BATTERY_CRITICAL), st(ST_BATTERY_CRITICAL_HINT));

    lv_timer_t *timer =
        lv_timer_create(shutdown_timer_cb, CRITICAL_DISPLAY_MS, NULL);
    lv_timer_set_repeat_count(timer, 1);

    ESP_LOGW(TAG, "critical battery: %u%%, shutdown in %d ms",
             (unsigned)battery_pct(), CRITICAL_DISPLAY_MS);
}
