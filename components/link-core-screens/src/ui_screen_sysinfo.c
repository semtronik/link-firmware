/*
 * System state - live values, refreshed by the manager's timer while
 * this screen is in front. Read-only rows.
 */

#include "screens_text.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "link_battery.h"
#include "link_power.h"
#include "ui_helpers.h"
#include "link_screens.h"
#include "link_theme.h"

static const char *TAG = "ui_sysinfo";

/* link_power_state_str() is for the logs, and does not translate. */
static const char *power_state_text(link_power_state_t state)
{
    switch (state) {
    case LINK_POWER_DISCHARGING:
        return st(ST_POWER_DISCHARGING);
    case LINK_POWER_CHARGING:
        return st(ST_POWER_CHARGING);
    case LINK_POWER_CHARGED:
        return st(ST_POWER_CHARGED);
    case LINK_POWER_NO_BATTERY:
        return st(ST_POWER_NO_BATTERY);
    default:
        return "?";
    }
}

/* Registered by the app before the screen is built, so its rows land
 * above the platform ones. */
static struct {
    ui_sysinfo_row_label_cb_t label;
    ui_sysinfo_row_update_cb_t on_refresh;
    lv_obj_t *value;
} s_app_rows[LINK_SYSINFO_APP_ROWS_MAX];

static unsigned s_app_row_count;

static lv_obj_t *s_battery;
static lv_obj_t *s_charge;
static lv_obj_t *s_uptime;

static lv_obj_t *add_row(lv_obj_t *list, const char *label)
{
    const ui_list_row_spec_t spec = {
        .kind = UI_LIST_ROW_READ,
        .label = label,
        .value = "--",
    };

    lv_obj_t *value = NULL;
    ui_helpers_add_list_row(list, &spec, NULL, NULL, &value);

    return value;
}

void ui_screen_sysinfo_add_row(ui_sysinfo_row_label_cb_t label,
                               ui_sysinfo_row_update_cb_t on_refresh)
{
    if (s_app_row_count >= LINK_SYSINFO_APP_ROWS_MAX) {
        ESP_LOGW(TAG, "no room for row '%s'", label());
        return;
    }

    s_app_rows[s_app_row_count].label = label;
    s_app_rows[s_app_row_count].on_refresh = on_refresh;
    s_app_row_count++;
}

lv_obj_t *ui_screen_sysinfo_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &link_style_screen, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, LINK_COLOR_SURFACE, 0);

    app_ui_attach_header(screen, st(ST_SYSINFO), true, false, NULL, NULL);

    lv_obj_t *list = ui_helpers_create_flex_list(screen);

    for (unsigned i = 0; i < s_app_row_count; i++) {
        s_app_rows[i].value = add_row(list, s_app_rows[i].label());
    }

    s_battery = add_row(list, st(ST_BATTERY));
    s_charge = add_row(list, st(ST_POWER));
    s_uptime = add_row(list, st(ST_UPTIME));

    ESP_LOGD(TAG, "built");

    return screen;
}

void ui_screen_sysinfo_refresh(void)
{
    for (unsigned i = 0; i < s_app_row_count; i++) {
        if (s_app_rows[i].value != NULL && s_app_rows[i].on_refresh != NULL) {
            s_app_rows[i].on_refresh(s_app_rows[i].value);
        }
    }

    link_power_t power;
    link_power_get(&power);

    const link_battery_t bat = power.battery;

    if (bat.presence != LINK_BATTERY_PRESENT) {
        lv_label_set_text(s_battery, bat.presence == LINK_BATTERY_ABSENT
                                          ? st(ST_BATTERY_ABSENT)
                                          : st(ST_BATTERY_DETECTING));
    } else if (!bat.gauge_ok) {
        lv_label_set_text(s_battery, st(ST_GAUGE_SILENT));
    } else {
        lv_label_set_text_fmt(s_battery, "%u %% (%lu mV)",
                              (unsigned)bat.soc_pct,
                              (unsigned long)bat.vcell_mv);
    }

    /* What the charger is doing, and - while it is doing something -
     * how fast. The gauge reports milli-%/h; whole %/h is as fine as
     * anyone reads it, and the sign carries the meaning. */
    if (bat.presence == LINK_BATTERY_PRESENT && bat.gauge_ok) {
        lv_label_set_text_fmt(s_charge, "%s (%+ld %%/h)",
                              power_state_text(power.state),
                              (long)(bat.crate_milli_pct_h / 1000));
    } else {
        lv_label_set_text(s_charge, power_state_text(power.state));
    }

    lv_obj_set_style_text_color(s_charge,
                                power.state == LINK_POWER_CHARGING
                                    ? LINK_COLOR_SUCCESS
                                    : LINK_COLOR_TEXT,
                                0);

    const unsigned s = (unsigned)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_uptime, "%u:%02u:%02u", s / 3600, (s / 60) % 60,
                          s % 60);
}
