/*
 * Language step of the first-start setup: two buttons under a title in
 * both languages, since which one the user reads is the question.
 */

#include <stdint.h>

#include "esp_log.h"
#include "link_prefs.h"
#include "link_screens.h"
#include "ui_helpers.h"
#include "link_theme.h"

static const char *TAG = "ui_language";

#define BTN_H   52
#define BTN_Y2  (LINK_USABLE_Y + LINK_USABLE_H - BTN_H - LINK_PAD_SM)
#define BTN_Y1  (BTN_Y2 - LINK_PAD_MD - BTN_H)

static lv_obj_t *s_overlay;
static ui_step_cb_t s_on_done;

/* Deferred: left from inside one of its own buttons. The next step opens
 * from here, so no frame shows what lies underneath. */
static void leave_cb(void *arg)
{
    (void)arg;

    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }

    if (s_on_done != NULL) {
        s_on_done();
    }
}

static void on_language(void *user_data)
{
    const link_lang_t lang = (link_lang_t)(uintptr_t)user_data;

    link_prefs_set_lang(lang);
    ESP_LOGI(TAG, "%s", link_prefs_lang_name(lang));
    lv_async_call(leave_cb, NULL);
}

static void wide_button(link_lang_t lang, int32_t y)
{
    lv_obj_t *btn =
        ui_touch_make_tap_button(s_overlay, on_language, (void *)(uintptr_t)lang);
    lv_obj_add_style(btn, &link_style_btn, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, LINK_USABLE_W, BTN_H);
    lv_obj_set_pos(btn, LINK_USABLE_X, y);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, link_prefs_lang_name(lang));
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_center(lbl);
}

void ui_screen_language_step_open(ui_step_cb_t on_done)
{
    if (s_overlay != NULL) {
        return;
    }

    s_on_done = on_done;

    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "LANGUE · LANGUAGE");
    lv_obj_set_style_text_font(title, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0,
                 LINK_USABLE_Y
                     + (BTN_Y1 - LINK_USABLE_Y - LINK_FONT_TITLE->line_height)
                           / 2);

    wide_button(LINK_LANG_FR, BTN_Y1);
    wide_button(LINK_LANG_EN, BTN_Y2);
}
