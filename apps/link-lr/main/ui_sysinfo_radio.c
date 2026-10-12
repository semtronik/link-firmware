/*
 * The radio rows of the system-state screen. The screen itself is
 * shared and knows nothing about a module; these rows are what link-lr
 * adds to it.
 */

#include "link_radio.h"
#include "link_rec.h"
#include "link_screens.h"
#include "link_theme.h"
#include "ui_manager.h"
#include "ui_text.h"

static void refresh_state(lv_obj_t *value)
{
    link_radio_state_t radio;
    link_radio_get(&radio);

    lv_label_set_text(value, tr(radio.listening ? T_RADIO_LISTENING
                                             : T_RADIO_STOPPED));
    lv_obj_set_style_text_color(value,
                                radio.listening ? LINK_COLOR_SUCCESS
                                                : LINK_COLOR_ERROR,
                                0);
}

/* Chip resets, worst interrupt latency, stamping anomalies. */
static void refresh_health(lv_obj_t *value)
{
    link_radio_state_t radio;
    link_radio_get(&radio);

    lv_label_set_text_fmt(value, tr(T_HEALTH_FMT),
                          (unsigned long)radio.recoveries,
                          (unsigned long)radio.irq_latency_max_us,
                          (unsigned long)radio.irq_anomalies);

    const bool fault = radio.recoveries != 0 || radio.irq_anomalies != 0;
    lv_obj_set_style_text_color(value,
                                fault ? LINK_COLOR_ERROR : LINK_COLOR_TEXT, 0);
}

/* Session, lines in it, space taken; lost lines in red, if any. */
static void refresh_rec(lv_obj_t *value)
{
    link_rec_stats_t rec;
    link_rec_get_stats(&rec);

    if (!rec.open) {
        lv_label_set_text(value, tr(T_REC_STOPPED));
        lv_obj_set_style_text_color(value, LINK_COLOR_ERROR, 0);
        return;
    }

    const unsigned kb = (unsigned)((rec.total_bytes + 512) / 1024);
    if (rec.lost != 0) {
        lv_label_set_text_fmt(value, tr(T_REC_LOST_FMT),
                              (unsigned long)rec.session,
                              (unsigned long)rec.lines, kb,
                              (unsigned long)rec.lost);
    } else {
        lv_label_set_text_fmt(value, tr(T_REC_FMT),
                              (unsigned long)rec.session,
                              (unsigned long)rec.lines, kb);
    }
    lv_obj_set_style_text_color(value, rec.lost != 0 ? LINK_COLOR_ERROR
                                                     : LINK_COLOR_TEXT,
                                0);
}

static const char *label_state(void)
{
    return tr(T_SYS_RADIO);
}

static const char *label_health(void)
{
    return tr(T_SYS_HEALTH);
}

static const char *label_rec(void)
{
    return tr(T_SYS_REC);
}

void ui_sysinfo_radio_register(void)
{
    ui_screen_sysinfo_add_row(label_state, refresh_state);
    ui_screen_sysinfo_add_row(label_health, refresh_health);
    ui_screen_sysinfo_add_row(label_rec, refresh_rec);
}
