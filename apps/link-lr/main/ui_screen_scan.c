/*
 * Band profile - the whole band at once, one bar per channel, in dBm.
 *
 * Read it by triggering the device under test and watching which bar
 * grows. What proves a signal is the correlation with the trigger, not
 * the height of a bar: the floor and the bursts alone cannot tell a
 * transmitter from the room. The sweep runs continuously, so there is
 * nothing to synchronise with - trigger whenever, as often as wanted,
 * and every channel keeps its strongest reading until reset.
 *
 * Absolute dBm rather than an excess over each channel's own floor. A
 * per-channel excess gives every bar its own zero, so two bars of the
 * same height need not mean the same thing and the scale shifts as the
 * floors move. On one fixed scale a bar means what it says, and the
 * profile can be compared channel to channel and run to run.
 *
 * This says nothing about the modulation - only that there is energy.
 * What it narrows down is the frequency, the one parameter no amount of
 * guessing at the others can recover.
 */

#include <stdio.h>

#include "esp_log.h"
#include "link_scan.h"
#include "link_theme.h"
#include "ui_helpers.h"
#include "ui_manager.h"
#include "ui_touch.h"
#include "ui_text.h"

static const char *TAG = "ui_scan";

/*
 * Chart redraws skipped between two drawn ones.
 *
 * The measurement and the display need not run at the same rate, and
 * nothing is lost by separating them: the profile is a max hold, so a
 * burst is recorded in the sweep the instant it is seen and simply
 * shows up at the next redraw. Meanwhile a redraw is not free - every
 * changed bar invalidates part of the screen, and it competes with
 * everything else drawn on the same task.
 *
 * At the manager's 250 ms refresh this draws twice a second, against a
 * sweep passing over the band tens of times a second.
 */
#define CHART_EVERY 2

/* One row of controls above the chart, which takes the rest. */
#define ROW_H     56
#define CHART_Y   (LINK_CONTENT_Y + ROW_H)
#define CHART_H   (LINK_CONTENT_H - ROW_H)

static lv_obj_t *s_bw;
static lv_obj_t *s_peak;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_chart_cursor_t *s_marker;

/*
 * What the chart is currently showing, so a redraw can write only the
 * bars that moved. On a quiet band almost none do, and a point written
 * with the value it already holds still invalidates its column.
 */
static int32_t s_shown[LINK_SCAN_MAX_CHANNELS];
static bool s_shown_valid;

static void on_reset_tapped(void *user_data)
{
    (void)user_data;
    link_scan_reset();
    /* The cache describes bars that are about to be cleared. */
    s_shown_valid = false;
    ESP_LOGI(TAG, "measurements cleared");
}

static void on_bw_tapped(void *user_data)
{
    (void)user_data;
    link_scan_cycle_bw();
    /* Channels are renumbered, so every bar means something else. */
    s_shown_valid = false;
}

lv_obj_t *ui_screen_scan_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &link_style_screen, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, LINK_COLOR_SURFACE, 0);

    app_ui_attach_header(screen, tr(T_SCAN), true, true, on_reset_tapped,
                         NULL);

    /*
     * Placed rather than listed. Everything fits the content area, so a
     * scrolling list would only add a way to push the chart out of
     * sight - and the chart is the screen.
     */
    lv_obj_t *row = ui_touch_make_scroll_safe_button(screen, on_bw_tapped,
                                                     NULL);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LINK_USABLE_W, ROW_H);
    lv_obj_set_pos(row, LINK_USABLE_X, LINK_CONTENT_Y);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_bw = lv_label_create(row);
    lv_label_set_text(s_bw, "--");
    lv_obj_set_style_text_font(s_bw, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_bw, LINK_COLOR_ACCENT_FG, 0);
    lv_obj_align(s_bw, LV_ALIGN_LEFT_MID, 0, 0);

    /* The peak reading, which is what the chart is scanned for - given
       as a number so it need not be estimated off the bars. */
    s_peak = lv_label_create(row);
    lv_label_set_text(s_peak, "--");
    lv_obj_set_style_text_font(s_peak, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_peak, LINK_COLOR_TEXT, 0);
    lv_obj_align(s_peak, LV_ALIGN_RIGHT_MID, 0, 0);

    s_chart = lv_chart_create(screen);
    lv_obj_set_size(s_chart, LINK_USABLE_W, CHART_H);
    lv_obj_set_pos(s_chart, LINK_USABLE_X, CHART_Y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    /* Resized by the first draw, from the aperture in use. */
    lv_chart_set_point_count(s_chart, link_scan_channels(LINK_SCAN_BW_500K));
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_chart, LINK_COLOR_BG, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    /* A gap between bars, which pad_all has just zeroed. Without it the
       bars touch and read as one filled block rather than as a profile,
       and a quiet band becomes a solid rectangle. */
    lv_obj_set_style_pad_column(s_chart, 2, 0);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);

    /*
     * Fixed scale, the ends of what the receiver can report. Fixed is
     * the point of showing dBm at all: a scale that followed the data
     * would make the same bar mean something different from one moment
     * to the next, which is what an excess over a moving floor already
     * did.
     */
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, LINK_SCAN_DBM_MIN,
                       LINK_SCAN_DBM_MAX);

    s_series = lv_chart_add_series(s_chart, LINK_COLOR_ACCENT_FG,
                                   LV_CHART_AXIS_PRIMARY_Y);

    /* Marks the strongest channel, so a frequency can be placed among
       bars that otherwise look alike. A cursor rather than a second
       series: a series would halve the width of every bar. */
    s_marker = lv_chart_add_cursor(s_chart, LINK_COLOR_TEXT_DIM, LV_DIR_VER);

    ESP_LOGD(TAG, "built");

    return screen;
}

/* One bar per channel: the strongest reading seen there, dBm. */
static void draw_profile(const link_scan_state_t *st)
{
    /* Only as many columns as the aperture has channels, so the bars
       fill the width instead of crowding into part of it. */
    if (st->channels > 0
        && lv_chart_get_point_count(s_chart) != st->channels) {
        lv_chart_set_point_count(s_chart, st->channels);
        s_shown_valid = false;
    }

    for (uint16_t i = 0; i < st->channels && i < LINK_SCAN_MAX_CHANNELS; i++) {
        const int32_t v =
            st->chan_measured[i] ? (int32_t)st->chan_max[i]
                                 : LV_CHART_POINT_NONE;

        if (s_shown_valid && s_shown[i] == v) {
            continue;
        }
        lv_chart_set_value_by_id(s_chart, s_series, i, v);
        s_shown[i] = v;
    }
    s_shown_valid = true;

    if (st->have_peak) {
        lv_chart_set_cursor_point(s_chart, s_marker, s_series,
                                  st->peak_channel);
    } else {
        /* Parked off the left edge, a cursor having no hidden state. */
        lv_chart_set_cursor_pos_x(s_chart, s_marker, -1);
    }
}

void ui_screen_scan_refresh(void)
{
    link_scan_state_t st;
    link_scan_get(&st);

    char buf[40];

    /* The channel count comes along: it is what says how finely the
       band is being cut, and it changes with the aperture. */
    snprintf(buf, sizeof(buf), "%lu kHz  %u can.",
             (unsigned long)(st.bw_hz / 1000U), (unsigned)st.channels);
    lv_label_set_text(s_bw, buf);

    if (st.tune_failed) {
        /* An empty profile otherwise looks exactly like a quiet band,
           which is the one thing it must not be read as. */
        lv_label_set_text(s_peak, tr(T_SCAN_TUNE_FAILED));
    } else if (st.passes == 0) {
        lv_label_set_text(s_peak, tr(T_SCAN_MEASURING));
    } else if (st.have_peak) {
        const uint32_t hz = link_scan_channel_hz(st.bw, st.peak_channel);
        snprintf(buf, sizeof(buf), "%lu.%02lu  %d dBm",
                 (unsigned long)(hz / 1000000U),
                 (unsigned long)((hz % 1000000U) / 10000U),
                 (int)st.chan_max[st.peak_channel]);
        lv_label_set_text(s_peak, buf);
    } else {
        lv_label_set_text(s_peak, "--");
    }

    /* The labels above stay at the manager's rate - they are two
       strings - while the chart, the expensive half, is drawn every
       CHART_EVERY refreshes. */
    static uint8_t tick;
    if (tick++ % CHART_EVERY == 0) {
        draw_profile(&st);
    }
}
