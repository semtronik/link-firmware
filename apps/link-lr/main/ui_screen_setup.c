/*
 * Start-up setup, before the screens are built: language, region, then
 * the clock. The clock comes last because it may be set from the app:
 * once the user has turned to Link Studio, nothing should be left to do
 * on the device. Only the steps due are shown: all three on a first
 * start or after a factory reset, the clock alone when it was lost. Each
 * step but the first can go back.
 *
 * Everything sits on a blank cover over the splash, so the splash never
 * shows between two steps, nor while the screens are built afterwards;
 * main() deletes the splash, and all of it with it.
 *
 * No session opens meanwhile: one only opens on the Journal, which does
 * not exist yet.
 */

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "link_lvgl.h"
#include "link_profile.h"
#include "ui_helpers.h"
#include "ui_manager.h"
#include "link_theme.h"
#include "ui_text.h"

static const char *TAG = "ui_setup";

#define BTN_H   52
#define BTN_Y2  (LINK_USABLE_Y + LINK_USABLE_H - BTN_H - LINK_PAD_SM)
#define BTN_Y1  (BTN_Y2 - LINK_PAD_MD - BTN_H)

static lv_obj_t *s_parent;
static lv_obj_t *s_region;
static bool s_full;
static SemaphoreHandle_t s_done;
static lv_obj_t *s_cover_text;

static void step_language(void);
static void step_time(void);
static void step_region(void);

/* Written now rather than with the cover: the language was not chosen
 * when it was built. What shows while the screens are built. */
static void finish(void)
{
    lv_label_set_text_fmt(s_cover_text, "Link-LR\n%s", tr(T_SETUP_PREPARING));
    xSemaphoreGive(s_done);
}

static void wide_button(lv_obj_t *page, const char *text, int32_t y,
                        ui_touch_tap_cb_t on_tap, void *user_data)
{
    lv_obj_t *btn = ui_touch_make_tap_button(page, on_tap, user_data);
    lv_obj_add_style(btn, &link_style_btn, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, LINK_USABLE_W, BTN_H);
    lv_obj_set_pos(btn, LINK_USABLE_X, y);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_center(lbl);
}

/* Deferred: the page goes from inside one of its own buttons. The next
 * step opens from here, so nothing shows in between. */
static void region_leave_cb(void *arg)
{
    lv_obj_delete(s_region);
    s_region = NULL;

    if (arg != NULL) {
        step_time();
    } else {
        step_language();
    }
}

/* Written now, and again if the user comes back and picks the other. */
static void on_region(void *user_data)
{
    link_profile_setup((link_region_t)(uintptr_t)user_data);
    lv_async_call(region_leave_cb, (void *)1);
}

static void on_region_back(void *user_data)
{
    (void)user_data;
    lv_async_call(region_leave_cb, NULL);
}

static void step_region(void)
{
    s_region = lv_obj_create(s_parent);
    lv_obj_remove_style_all(s_region);
    lv_obj_set_size(s_region, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_region, 0, 0);
    lv_obj_set_style_bg_color(s_region, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_region, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_region, LV_OBJ_FLAG_SCROLLABLE);

    ui_helpers_attach_overlay_header(s_region, tr(T_REGION), on_region_back,
                                     NULL, NULL, NULL);

    lv_obj_t *hint = lv_label_create(s_region);
    lv_label_set_text(hint, tr(T_SETUP_REGION_HINT));
    lv_obj_set_style_text_font(hint, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(hint, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0,
                 LINK_CONTENT_Y
                     + (BTN_Y1 - LINK_CONTENT_Y - 2 * LINK_FONT_BODY->line_height)
                           / 2);

    wide_button(s_region, tr(T_SETUP_EU868), BTN_Y1, on_region,
                (void *)(uintptr_t)LINK_REGION_EU868);
    wide_button(s_region, tr(T_SETUP_US915), BTN_Y2, on_region,
                (void *)(uintptr_t)LINK_REGION_US915);
}

static void step_time(void)
{
    if (s_full) {
        ui_screen_time_onboarding_open(finish, step_region);
    } else {
        ui_screen_time_onboarding_open(finish, NULL);
    }
}

static void step_language(void)
{
    ui_screen_language_step_open(step_region);
}

void ui_screen_setup_run(lv_obj_t *parent, bool full)
{
    s_done = xSemaphoreCreateBinary();
    if (s_done == NULL) {
        ESP_LOGE(TAG, "no semaphore, setup skipped");
        return;
    }

    link_lvgl_lock(UINT32_MAX);

    /* Between two steps, this is what shows - never the splash. */
    lv_obj_t *cover = lv_obj_create(parent);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

    s_cover_text = lv_label_create(cover);
    lv_label_set_text(s_cover_text, "");
    lv_obj_set_style_text_font(s_cover_text, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_cover_text, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_cover_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_cover_text);

    s_parent = parent;
    s_full = full;
    if (full) {
        step_language();
    } else {
        step_time();
    }
    link_lvgl_unlock();

    ESP_LOGI(TAG, "setup: %s", full ? "language, region, clock" : "clock");
    xSemaphoreTake(s_done, portMAX_DELAY);

    vSemaphoreDelete(s_done);
    s_done = NULL;
    ESP_LOGI(TAG, "done");
}
