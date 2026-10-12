/*
 * Shutdown confirmation, opened by a long press on the power button.
 *
 * Full-screen and on top of whatever else is open: it answers a
 * physical button, so it takes precedence. Back cancels, validate cuts
 * the rail. No timeout - it waits for an answer.
 */

#include "screens_text.h"
#include "esp_log.h"
#include "power_key.h"
#include "ui_helpers.h"
#include "link_screens.h"
#include "link_theme.h"

static const char *TAG = "ui_shutdown";

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

static void on_confirm(void *user_data)
{
    (void)user_data;
    ESP_LOGW(TAG, "confirmed");

    /* No teardown: the rail is going away. */
    power_key_power_off("user");
}

void ui_screen_shutdown_open(void)
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

    ui_helpers_attach_overlay_header(s_overlay, st(ST_POWER_OFF), on_cancel, NULL,
                                     on_confirm, NULL);

    lv_obj_t *icon = lv_label_create(s_overlay);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(icon, LINK_FONT_HUGE, 0);
    lv_obj_set_style_text_color(icon, LINK_COLOR_ERROR, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, LINK_PAD_MD);

    ESP_LOGI(TAG, "opened");
}
