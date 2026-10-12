#include "screens_text.h"
#include "link_screens.h"

#include "esp_log.h"
#include "link_time.h"
#include "ui_helpers.h"
#include "link_theme.h"

static const char *TAG = "ui_time_onb";

#define CHECK_PERIOD_MS 250

#define BTN_H   52
#define BTN_Y2  (LINK_USABLE_Y + LINK_USABLE_H - BTN_H - LINK_PAD_SM)
#define BTN_Y1  (BTN_Y2 - LINK_PAD_MD - BTN_H)

typedef enum {
    STATE_CHOICE,
    STATE_WAITING,
} state_t;

static lv_obj_t *s_overlay;
static lv_obj_t *s_choice;
static lv_obj_t *s_waiting;
static lv_obj_t *s_hint;
static lv_timer_t *s_check;

static ui_step_cb_t s_on_done;
static ui_step_cb_t s_on_back;
static ui_step_cb_t s_leaving;

/*
 * Set while the user is partway through entering the date and time.
 *
 * Writing the date alone already makes the clock "valid", so without
 * this the watcher would move on between the two steps and the board
 * would run with the hour never confirmed. It clears only on a
 * deliberate choice: a completed entry, or switching to the app.
 */
static bool s_manual_flow;

/*
 * Moves on by itself once the clock becomes valid - unless it already
 * was on opening, as when coming back to this step: then it shows the
 * time and waits for the header's validate.
 */
static bool s_auto;

/* Deferred: the step is left from inside one of its own events. The next
 * step opens from here, so no frame shows what lies underneath. */
static void leave_cb(void *arg)
{
    (void)arg;

    if (s_check != NULL) {
        lv_timer_delete(s_check);
        s_check = NULL;
    }

    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_choice = NULL;
        s_waiting = NULL;
        s_hint = NULL;
    }

    ESP_LOGI(TAG, "closed");

    const ui_step_cb_t cb = s_leaving;
    s_leaving = NULL;
    if (cb != NULL) {
        cb();
    }
}

static void leave(ui_step_cb_t cb)
{
    if (s_leaving != NULL || s_overlay == NULL) {
        return;
    }
    s_leaving = cb;
    lv_async_call(leave_cb, NULL);
}

static void show_current_time(void)
{
    struct tm now;
    link_time_get_local(&now);
    lv_label_set_text_fmt(s_hint, "%s\n%02d/%02d/%04d  %02d:%02d",
                          st(ST_ONB_CURRENT), now.tm_mday, now.tm_mon + 1,
                          now.tm_year + 1900, now.tm_hour, now.tm_min);
}

static void check_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_manual_flow || !link_time_is_valid()) {
        return;
    }

    if (s_auto) {
        leave(s_on_done);
    } else {
        show_current_time();
    }
}

static void show(state_t state)
{
    lv_obj_add_flag(s_choice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_waiting, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(state == STATE_CHOICE ? s_choice : s_waiting,
                       LV_OBJ_FLAG_HIDDEN);
}

static void on_time_committed(void)
{
    /* Both steps done: on to the next, whether the clock was set before
     * or not. */
    s_manual_flow = false;
    leave(s_on_done);
}

static void on_time_cancelled(void)
{
    /* The date was written but the hour was not confirmed. The guard
     * stays up: back to the choice, and the user picks again. */
    ESP_LOGI(TAG, "hour cancelled, still blocked");
}

static void on_date_committed(void)
{
    /* Chained: the hour on its own would have no date to attach to. */
    ui_screen_time_edit_open(on_time_committed, on_time_cancelled);
}

static void on_date_cancelled(void)
{
    ESP_LOGI(TAG, "date cancelled, still blocked");
}

/* The zone comes first, so the date and time entered after it mean what
 * the user reads on a wall clock. Picking it afterwards would move a
 * time they had just confirmed. */
static void on_zone_committed(void)
{
    ui_screen_date_edit_open(on_date_committed, on_date_cancelled);
}

static void on_manual_tapped(void *user_data)
{
    (void)user_data;

    s_manual_flow = true;
    ui_screen_zone_edit_open(on_zone_committed);
}

static void on_app_tapped(void *user_data)
{
    (void)user_data;

    /* Deliberate switch away from the manual flow, so the guard comes
     * down: if the clock is already valid the watcher moves on at once. */
    s_manual_flow = false;
    show(STATE_WAITING);
}

static void on_back_to_choice(void *user_data)
{
    (void)user_data;
    show(STATE_CHOICE);
}

static void on_back(void *user_data)
{
    (void)user_data;
    leave(s_on_back);
}

static void on_continue(void *user_data)
{
    (void)user_data;
    leave(s_on_done);
}

static lv_obj_t *full_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    return page;
}

static lv_obj_t *wide_button(lv_obj_t *parent, const char *text, int32_t y,
                             bool accent, ui_touch_tap_cb_t on_tap)
{
    lv_obj_t *btn = ui_touch_make_tap_button(parent, on_tap, NULL);
    lv_obj_add_style(btn, accent ? &link_style_btn_accent : &link_style_btn, 0);
    lv_obj_add_style(btn,
                     accent ? &link_style_btn_accent_pressed
                            : &link_style_btn_pressed,
                     LV_STATE_PRESSED);
    lv_obj_set_size(btn, LINK_USABLE_W, BTN_H);
    lv_obj_set_pos(btn, LINK_USABLE_X, y);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_center(lbl);

    return btn;
}

static void build_choice(lv_obj_t *parent, bool valid)
{
    s_choice = full_page(parent);

    /* Validate only once there is a time to keep. */
    ui_helpers_attach_overlay_header(s_choice, st(ST_ONB_TITLE),
                                     s_on_back != NULL ? on_back : NULL, NULL,
                                     valid ? on_continue : NULL, NULL);

    /* Says why the board is refusing to go further - or, coming back to
     * a clock already set, what it is set to. */
    s_hint = lv_label_create(s_choice);
    lv_obj_set_style_text_font(s_hint, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_hint, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0,
                 LINK_CONTENT_Y
                     + (BTN_Y1 - LINK_CONTENT_Y - 2 * LINK_FONT_BODY->line_height)
                           / 2);
    if (valid) {
        show_current_time();
    } else {
        lv_label_set_text(s_hint, st(ST_ONB_HINT));
    }

    wide_button(s_choice, st(ST_ONB_MANUAL), BTN_Y1, !valid, on_manual_tapped);
    wide_button(s_choice, st(ST_ONB_APP), BTN_Y2, false, on_app_tapped);
}

static void build_waiting(lv_obj_t *parent)
{
    s_waiting = full_page(parent);
    lv_obj_add_flag(s_waiting, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_waiting);
    lv_label_set_text(title, st(ST_ONB_WAITING));
    lv_obj_set_style_text_font(title, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, LINK_USABLE_Y + LINK_PAD_LG);

    lv_obj_t *hint = lv_label_create(s_waiting);
    lv_label_set_text(hint, st(ST_ONB_WAITING_HINT));
    lv_obj_set_style_text_font(hint, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(hint, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    wide_button(s_waiting, st(ST_BACK), BTN_Y2, false, on_back_to_choice);
}

bool ui_screen_time_onboarding_is_open(void)
{
    return s_overlay != NULL;
}

void ui_screen_time_onboarding_open(ui_step_cb_t on_done, ui_step_cb_t on_back)
{
    if (s_overlay != NULL) {
        return;
    }

    s_on_done = on_done;
    s_on_back = on_back;
    s_leaving = NULL;
    s_manual_flow = false;

    const bool valid = link_time_is_valid();
    s_auto = !valid;

    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    build_choice(s_overlay, valid);
    build_waiting(s_overlay);
    show(STATE_CHOICE);

    /* Polled rather than pushed: the clock can be set from the app, from
     * the editors, or from a re-sync, and all three land here. */
    s_check = lv_timer_create(check_cb, CHECK_PERIOD_MS, NULL);

    ESP_LOGI(TAG, "opened - clock %s", valid ? "set" : "not set");
}
