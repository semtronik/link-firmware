/*
 * Date and time. Both rows open an editor; the third says whether the
 * clock is worth anything - an RTC that lost power reports a plausible
 * time that means nothing, and saying so is the whole point of reading
 * that chip.
 */

#include <time.h>

#include "screens_text.h"
#include "esp_log.h"
#include "link_time.h"
#include "link_screens.h"
#include "ui_helpers.h"
#include "link_theme.h"

static const char *TAG = "ui_datetime";

static lv_obj_t *s_date;
static lv_obj_t *s_time;
static lv_obj_t *s_zone;
static lv_obj_t *s_state;

typedef enum {
    ROW_DATE,
    ROW_TIME,
    ROW_ZONE,
} row_t;

/*
 * UTC-12 to UTC+14, the range real zones occupy. Half-hour steps: whole
 * hours would miss India and South Australia. The quarter-hour zones -
 * Nepal, Chatham - are not reachable, which is the price of a stepper
 * that takes two taps to cross Europe.
 */
#define ZONE_MIN_MINUTES (-720)
#define ZONE_MAX_MINUTES 840
#define ZONE_STEP_MINUTES 30

static void zone_format(int value, char *buf, unsigned size)
{
    const int mag = value < 0 ? -value : value;

    lv_snprintf(buf, size, "UTC%c%d:%02d", value < 0 ? '-' : '+', mag / 60,
                mag % 60);
}

/* Runs after the offset is stored, so a caller can chain the clock
 * entry behind it. */
static ui_time_edit_cb_t s_zone_on_commit;

static void zone_on_commit(int value)
{
    link_time_set_offset_minutes((int16_t)value);

    if (s_zone_on_commit != NULL) {
        const ui_time_edit_cb_t cb = s_zone_on_commit;
        s_zone_on_commit = NULL;
        cb();
    }
}

void ui_screen_zone_edit_open(ui_time_edit_cb_t on_commit)
{
    if (ui_stepper_is_open()) {
        return;
    }

    s_zone_on_commit = on_commit;

    const ui_stepper_cfg_t cfg = {
        .title = st(ST_ZONE),
        .format_cb = zone_format,
        .initial = link_time_get_offset_minutes(),
        .min = ZONE_MIN_MINUTES,
        .max = ZONE_MAX_MINUTES,
        .step = ZONE_STEP_MINUTES,
        .on_commit = zone_on_commit,
    };
    ui_stepper_open(&cfg);
}

static void on_row_tapped(void *user_data)
{
    if (ui_screen_time_edit_is_open()) {
        return;
    }

    switch ((row_t)(intptr_t)user_data) {
    case ROW_DATE:
        ui_screen_date_edit_open(NULL, NULL);
        break;
    case ROW_TIME:
        ui_screen_time_edit_open(NULL, NULL);
        break;
    case ROW_ZONE:
        ui_screen_zone_edit_open(NULL);
        break;
    }
}

static lv_obj_t *add_row(lv_obj_t *list, const char *label, bool editable,
                         row_t row)
{
    const ui_list_row_spec_t spec = {
        .kind = editable ? UI_LIST_ROW_EDIT : UI_LIST_ROW_READ,
        .label = label,
        .value = "--",
    };

    lv_obj_t *value = NULL;
    ui_helpers_add_list_row(list, &spec, editable ? on_row_tapped : NULL,
                            (void *)(intptr_t)row, &value);

    return value;
}

lv_obj_t *ui_screen_datetime_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &link_style_screen, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, LINK_COLOR_SURFACE, 0);

    app_ui_attach_header(screen, st(ST_DATE_TIME), true, false, NULL, NULL);

    lv_obj_t *list = ui_helpers_create_flex_list(screen);

    /* Zone first: it decides what the two rows under it mean, and
     * setting it afterwards moves a time the user has just entered. */
    s_zone = add_row(list, st(ST_ZONE), true, ROW_ZONE);
    s_date = add_row(list, st(ST_DATE), true, ROW_DATE);
    s_time = add_row(list, st(ST_TIME), true, ROW_TIME);
    s_state = add_row(list, st(ST_CLOCK_STATE), false, ROW_DATE);

    ESP_LOGD(TAG, "built");

    return screen;
}

void ui_screen_datetime_refresh(void)
{
    /* Independent of the clock being set: the zone is a display setting,
     * and it is the one thing still worth showing on a board whose RTC
     * has nothing to give. */
    char zone[12];
    zone_format(link_time_get_offset_minutes(), zone, sizeof(zone));
    lv_label_set_text(s_zone, zone);

    /* The system clock, not the RTC: link_time keeps the two in step
     * and owns the bus traffic, so this costs nothing. */
    if (!link_time_is_valid()) {
        lv_label_set_text(s_date, "--/--/----");
        lv_label_set_text(s_time, "--:--:--");
        lv_label_set_text(s_state, st(ST_CLOCK_NOT_SET));
        lv_obj_set_style_text_color(s_state, LINK_COLOR_WARNING, 0);
        return;
    }

    struct tm now;
    link_time_get_local(&now);

    lv_label_set_text_fmt(s_date, "%02d/%02d/%04d", now.tm_mday,
                          now.tm_mon + 1, now.tm_year + 1900);
    lv_label_set_text_fmt(s_time, "%02d:%02d:%02d", now.tm_hour, now.tm_min,
                          now.tm_sec);

    lv_label_set_text(s_state, st(ST_OK));
    lv_obj_set_style_text_color(s_state, LINK_COLOR_SUCCESS, 0);
}
