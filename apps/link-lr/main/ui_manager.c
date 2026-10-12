#include "ui_manager.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_battery.h"
#include "link_events.h"
#include "link_power.h"
#include "link_radio.h"
#include "link_rec.h"
#include "link_scan.h"
#include "link_time.h"
#include "power_key.h"
#include "ui_helpers.h"
#include "link_theme.h"

static const char *TAG = "ui_manager";

#define REFRESH_MS 250

/* Below this the pack is low enough to warn about; below the critical
 * mark the board shuts itself down rather than waiting for the cell's
 * own cutoff to drop the rail mid-write. */
#define BATTERY_ALERT_PCT    20
#define BATTERY_CRITICAL_PCT 5

/* Re-arming higher than it fires, so a gauge hovering on the threshold
 * cannot bring the alert back the moment it has been dismissed. */
#define BATTERY_CLEAR_PCT 25

static lv_obj_t *s_screens[UI_SCREEN_COUNT];
static ui_screen_id_t s_active = UI_SCREEN_CAPTURE;

/* The alert is shown once per discharge, not once per tick. Cleared
 * when the pack comes back up, so a recharge re-arms it. */
static bool s_alert_acknowledged;

#define BACKSTACK_DEPTH 8

static ui_screen_id_t s_backstack[BACKSTACK_DEPTH];
static int s_backstack_len;

/* Consecutive duplicates are dropped: chained switches (menu, screen,
 * menu, screen) would otherwise stack the same entry twice and make
 * back take two presses to do one thing. */
static void backstack_push(ui_screen_id_t id)
{
    if (s_backstack_len > 0 && s_backstack[s_backstack_len - 1] == id) {
        return;
    }

    if (s_backstack_len < BACKSTACK_DEPTH) {
        s_backstack[s_backstack_len++] = id;
        return;
    }

    /* Full: drop the oldest. Losing the far end of the history is
     * better than refusing to record where the user just was. */
    for (int i = 1; i < BACKSTACK_DEPTH; i++) {
        s_backstack[i - 1] = s_backstack[i];
    }
    s_backstack[BACKSTACK_DEPTH - 1] = id;
}

static bool backstack_pop(ui_screen_id_t *out)
{
    if (s_backstack_len == 0) {
        return false;
    }
    *out = s_backstack[--s_backstack_len];

    return true;
}

/* Every id in the enum is a screen now: the overlays that took part in
 * the back stack are gone with the menu. Kept as a function because the
 * table can still hold a NULL, which is the case the callers guard. */
static bool screen_is_persistent(ui_screen_id_t id)
{
    return id < UI_SCREEN_COUNT;
}

/* Every entry starts at the top of its list rather than wherever it was
 * left. */
static void reset_scroll_children(lv_obj_t *screen)
{
    const uint32_t count = lv_obj_get_child_count(screen);

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE)) {
            lv_obj_scroll_to_y(child, 0, LV_ANIM_OFF);
        }
    }
}

/*
 * Only the band sweep takes the chip away.
 *
 * Starting and stopping it lives here rather than in the screen,
 * because this is the one place every navigation path goes through -
 * back, menu and direct switch alike. Anywhere else, some path would
 * leave the radio busy with nothing listening.
 *
 * It retunes continuously, which cannot be done under a receiver armed
 * on one frequency. The listen screen does not: it reads the receiver
 * that is already running, which is the point of having a single
 * receive path - what it shows is what the device is actually hearing,
 * not a second radio session that behaves slightly differently.
 */
static bool screen_owns_radio(ui_screen_id_t id)
{
    return id == UI_SCREEN_SCAN;
}

static void radio_release(ui_screen_id_t from)
{
    if (from == UI_SCREEN_SCAN) {
        link_scan_stop();
    }
}

static void radio_follow_screen(ui_screen_id_t from, ui_screen_id_t to)
{
    if (from == to) {
        return;
    }

    if (screen_owns_radio(from)) {
        radio_release(from);
        if (!screen_owns_radio(to)) {
            link_radio_resume();
        }
    } else if (screen_owns_radio(to)) {
        link_rec_close("scan");
        link_radio_suspend();
    }

    if (!screen_owns_radio(to)) {
        return;
    }

    if (link_scan_start() != ESP_OK) {
        link_radio_resume();
    }
}

/*
 * Is a capture running.
 *
 * Started at boot and stopped at shutdown: the device receives from the
 * moment it is on, so a session is not something navigation enters or
 * leaves.
 */
static bool s_session_running;

void ui_session_start(void)
{
    if (s_session_running) {
        return;
    }

    s_session_running = true;
    ESP_LOGI(TAG, "session started");
}

void ui_session_stop(void)
{
    if (!s_session_running) {
        return;
    }

    s_session_running = false;
    ESP_LOGI(TAG, "session stopped");
}

static void load_screen(ui_screen_id_t id)
{
    const ui_screen_id_t previous = s_active;

    s_active = id;
    reset_scroll_children(s_screens[id]);
    lv_screen_load(s_screens[id]);

    radio_follow_screen(previous, id);
}

static void on_back_tapped(void *user_data)
{
    (void)user_data;
    app_ui_navigate_back();
}

lv_obj_t *app_ui_attach_header(lv_obj_t *screen, const char *title,
                               bool show_back, bool show_validate,
                               void (*on_validate)(void *),
                               void *validate_user_data)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &link_style_header, 0);
    lv_obj_set_size(bar, LINK_SCREEN_W, LINK_HEADER_H);
    lv_obj_set_pos(bar, 0, LINK_USABLE_Y);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t btn_y =
        LINK_USABLE_Y + (LINK_HEADER_H - LINK_HEADER_BTN_SZ) / 2;

    /* On the screen, not on the bar: the bar would swallow the taps. */
    if (show_back) {
        lv_obj_t *back = ui_helpers_create_header_button(
            screen, LV_SYMBOL_LEFT, on_back_tapped, NULL);
        lv_obj_set_pos(back, LINK_USABLE_X, btn_y);
    }

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);

    /* Centred over a back button, left-aligned without one - a root
     * screen reads differently from one you descended into. */
    if (show_back) {
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, LINK_PAD_LG, 0);
    }

    if (show_validate && on_validate != NULL) {
        lv_obj_t *validate = ui_helpers_create_header_button(
            screen, LV_SYMBOL_OK, on_validate, validate_user_data);
        lv_obj_set_pos(validate,
                       LINK_SCREEN_W - LINK_BEZEL_MARGIN - LINK_HEADER_BTN_SZ,
                       btn_y);
        lv_obj_set_style_text_color(lv_obj_get_child(validate, 0),
                                    LINK_COLOR_ACCENT_FG, 0);
    }

    return lbl;
}

void ui_manager_switch_to(ui_screen_id_t screen)
{
    if (!screen_is_persistent(screen) || s_screens[screen] == NULL
        || screen == s_active) {
        return;
    }

    backstack_push(s_active);
    load_screen(screen);

    ESP_LOGD(TAG, "switch to %d (stack=%d)", (int)screen, s_backstack_len);
}

/*
 * The stack is emptied rather than unwound one entry at a time: the
 * capture is the root, so everything on it is below where we are going.
 * Leaving it would make the next back press walk into a history the user
 * has just left.
 */
void ui_manager_go_home(void)
{
    s_backstack_len = 0;

    if (s_active != UI_SCREEN_CAPTURE
        && s_screens[UI_SCREEN_CAPTURE] != NULL) {
        load_screen(UI_SCREEN_CAPTURE);
    }

    ESP_LOGD(TAG, "root");
}

void app_ui_navigate_back(void)
{
    ui_screen_id_t prev;

    /* An empty stack means we are at the root, or came up somewhere with
     * no recorded history - either way the capture is where back leads. */
    if (!backstack_pop(&prev)) {
        if (s_active != UI_SCREEN_CAPTURE
            && s_screens[UI_SCREEN_CAPTURE] != NULL) {
            load_screen(UI_SCREEN_CAPTURE);
        }
        return;
    }

    if (!screen_is_persistent(prev) || s_screens[prev] == NULL) {
        prev = UI_SCREEN_CAPTURE;
    }

    if (prev != s_active) {
        load_screen(prev);
    }

    ESP_LOGD(TAG, "back to %d (stack=%d)", (int)prev, s_backstack_len);
}

ui_screen_id_t ui_manager_active_screen(void)
{
    return s_active;
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* The power button is watched on its own task, which must not touch
     * lv_*; this is where its request is picked up. It goes first: it
     * answers a physical press, so it outranks the battery overlays. */
    if (power_key_take_request()) {
        ui_screen_shutdown_open();
    }

    /* Nothing here may touch the I2C bus: a device that does not answer
     * costs a 100 ms timeout per transfer, and this timer is the LVGL
     * thread - the display would stall for as long. Sampling lives on
     * the link_power task, and this only reads what it published. */
    link_power_t power;
    link_power_get(&power);
    const link_battery_t bat = power.battery;

    /* Only worth warning about while the cell is being drained: a low
     * pack on the charger is filling up, and an alert - let alone a
     * shutdown - over a device plugged in would be nonsense. */
    const bool on_battery = bat.presence == LINK_BATTERY_PRESENT
                            && bat.gauge_ok
                            && power.state == LINK_POWER_DISCHARGING;

    if (!on_battery || bat.soc_pct >= BATTERY_CLEAR_PCT) {
        s_alert_acknowledged = false;
    }

    /* Critical first and on its own: showing the low-battery alert on
     * top of a shutdown that is already under way helps nobody. */
    if (on_battery && bat.soc_pct < BATTERY_CRITICAL_PCT) {
        if (!ui_screen_battery_critical_is_open()) {
            /* Logged where it is decided rather than where the screen
             * opens: what matters afterwards is the level that tripped
             * it, and a board about to cut its own power has one write
             * left to make it count. */
            char detail[16];
            snprintf(detail, sizeof(detail), "pct=%u",
                     (unsigned)bat.soc_pct);
            link_events_append("BAT_CRITICAL", detail);

            ui_screen_battery_critical_open();
        }
    } else if (on_battery && bat.soc_pct < BATTERY_ALERT_PCT
               && !s_alert_acknowledged
               && !ui_screen_battery_alert_is_open()) {
        char detail[16];
        snprintf(detail, sizeof(detail), "pct=%u", (unsigned)bat.soc_pct);
        link_events_append("BAT_ALERT", detail);

        ui_screen_battery_alert_open();
    }

    switch (s_active) {
    case UI_SCREEN_MENU:
        ui_screen_menu_refresh();
        break;
    case UI_SCREEN_CAPTURE:
        link_rec_allow();
        ui_screen_capture_refresh();
        break;
    case UI_SCREEN_CONFIG:
        ui_screen_config_refresh();
        break;
    case UI_SCREEN_SCAN:
        ui_screen_scan_refresh();
        break;
    case UI_SCREEN_SYSINFO:
        ui_screen_sysinfo_refresh();
        break;
    case UI_SCREEN_SYSTEM:
        ui_screen_system_refresh();
        break;
    case UI_SCREEN_DATETIME:
        ui_screen_datetime_refresh();
        break;
    default:
        break;
    }
}

void app_ui_battery_alert_acknowledge(void)
{
    if (s_alert_acknowledged) {
        return;
    }
    s_alert_acknowledged = true;

    /* Recorded so a session can be read back knowing the warning was
     * seen and dismissed, rather than never shown. */
    link_events_append("BAT_ACK", NULL);
}

static void build_screens(void)
{
    s_screens[UI_SCREEN_MENU] = ui_screen_menu_create();
    s_screens[UI_SCREEN_CAPTURE] = ui_screen_capture_create();
    s_screens[UI_SCREEN_CONFIG] = ui_screen_config_create();
    s_screens[UI_SCREEN_SCAN] = ui_screen_scan_create();
    s_screens[UI_SCREEN_ABOUT] = ui_screen_about_create();
    s_screens[UI_SCREEN_SYSINFO] = ui_screen_sysinfo_create();
    s_screens[UI_SCREEN_SYSTEM] = ui_screen_system_create();
    s_screens[UI_SCREEN_DATETIME] = ui_screen_datetime_create();
}

void app_ui_rebuild(void)
{
    /* A rebuild does not go through load_screen(), so whatever the
     * active screen had running has to be stopped by hand. */
    if (screen_owns_radio(s_active)) {
        radio_release(s_active);
        link_radio_resume();
    }

    /* An empty screen to hold the display while the old ones go: LVGL
     * will not let the active screen be deleted. */
    lv_obj_t *tmp = lv_obj_create(NULL);
    lv_screen_load(tmp);

    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        if (s_screens[i] != NULL) {
            lv_obj_delete(s_screens[i]);
            s_screens[i] = NULL;
        }
    }

    /* Only now: lv_style_init() on a style some live object still
     * points at leaves that object reading freed memory. */
    link_theme_init();

    build_screens();

    s_backstack_len = 0;
    load_screen(UI_SCREEN_CAPTURE);
    lv_obj_delete(tmp);

    ESP_LOGI(TAG, "rebuilt");
}

esp_err_t ui_manager_init(void)
{
    /* Before the first build, and only here: a rebuild reuses what is
     * already registered. */
    ui_sysinfo_radio_register();

    build_screens();

    s_backstack_len = 0;
    load_screen(UI_SCREEN_CAPTURE);

    /* The device receives from the moment it is on. */
    ui_session_start();

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    /* Tasks, locks and queues need internal RAM: a low figure here makes
     * the next one fail. */
    ESP_LOGI(TAG, "internal RAM: %u KB free (min %u KB), PSRAM: %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
                        / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    return ESP_OK;
}

const char *app_ui_product_name(void)
{
    return "LINK-LR";
}
