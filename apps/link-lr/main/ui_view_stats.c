/*
 * Summary - the session's report: which one, how long, then two tiles,
 * sent and received. The received ring shows at a glance how the frames
 * split by margin class and errors; each tile opens its figures.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_timer.h"
#include "link_stats.h"
#include "ui_text.h"
#include "ui_touch.h"
#include "ui_view.h"

/* Out of the green-orange-red scale: lost, not merely weak. */
#define COLOR_ERROR_BAR lv_color_hex(0x8B5CF6)

#define LINE1_Y  4
#define TILE_Y   28
#define TILE_H   118
#define TX_W     80
#define RX_X     (TX_W + 8)
#define RX_W     (VIEW_CONTENT_W - RX_X)

/* Inside a tile. */
#define TITLE_Y  8
#define RING_X   12
#define RING_Y   31
#define RING_D   78
#define RING_W   9
#define MID_W    60
#define KEY_X    100
#define KEY_HEAD 30
#define KEY_Y    46
#define KEY_H    16
/* Five digits fit: the layout never shifts as a session grows. */
#define COUNT_X  164
#define COUNT_W  40

/* A share drawn smaller than this would not be seen; degrees. */
#define ARC_MIN  8
#define ARC_GAP  1

/* The sheets. */
#define HEAD_Y   0
#define LINES_Y  20
#define LINE_H   18
#define RAIL_X   124
#define RAIL_W   80
#define RAIL_H   8

enum { PART_GOOD, PART_FAIR, PART_POOR, PART_ERR, PART_N };

typedef enum { SHEET_NONE, SHEET_RX, SHEET_TX } sheet_t;

static lv_obj_t *s_session;
static lv_obj_t *s_duration;
static lv_obj_t *s_tx_num;
static lv_obj_t *s_rx_num;
static lv_obj_t *s_arc[PART_N];
static lv_obj_t *s_count[PART_N];

/* The open sheet's labels; dropped when it goes. */
static sheet_t s_open;
static lv_obj_t *s_head;
static lv_obj_t *s_fill[7];
static lv_obj_t *s_value[7];
static int s_lines;

static lv_color_t part_color(int part)
{
    switch (part) {
    case PART_GOOD:
        return LINK_COLOR_SUCCESS;
    case PART_FAIR:
        return LINK_COLOR_WARNING;
    case PART_POOR:
        return LINK_COLOR_ERROR;
    default:
        return COLOR_ERROR_BAR;
    }
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font,
                      lv_color_t color, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *centred(lv_obj_t *parent, const lv_font_t *font,
                         lv_color_t color, int32_t x, int32_t y, int32_t w)
{
    lv_obj_t *l = text(parent, font, color, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* One decimal, rounded; never 0 when something was counted. */
static void format_share(uint32_t n, uint32_t total, char *buf, size_t size)
{
    if (total == 0) {
        buf[0] = '\0';
        return;
    }

    const uint32_t tenths =
        (uint32_t)(((uint64_t)n * 2000 + total) / (2 * (uint64_t)total));

    if (n > 0 && tenths == 0) {
        lv_snprintf(buf, size, "<0.1 %%");
    } else {
        lv_snprintf(buf, size, "%lu.%lu %%", (unsigned long)(tenths / 10),
                    (unsigned long)(tenths % 10));
    }
}

static void format_duration(int64_t ms, char *buf, size_t size)
{
    const int64_t s = ms / 1000;

    if (s < 60) {
        lv_snprintf(buf, size, tr(T_DURATION_S_FMT), (long)s);
    } else if (s < 3600) {
        lv_snprintf(buf, size, tr(T_DURATION_MIN_FMT), (long)(s / 60),
                    (long)(s % 60));
    } else {
        lv_snprintf(buf, size, tr(T_DURATION_H_FMT), (long)(s / 3600),
                    (long)((s % 3600) / 60));
    }
}

/*
 * The sheets: each line a name, a rail, the count and its share.
 */

static void sheet_forget(void)
{
    s_open = SHEET_NONE;
    s_head = NULL;
    s_lines = 0;
    for (int i = 0; i < 7; i++) {
        s_fill[i] = NULL;
        s_value[i] = NULL;
    }
}

static void sheet_line(lv_obj_t *body, int32_t y, const char *name,
                       lv_color_t color)
{
    const int i = s_lines++;

    lv_label_set_text(text(body, LINK_FONT_CAPTION, LINK_COLOR_TEXT, 0, y),
                      name);

    lv_obj_t *r = lv_obj_create(body);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, RAIL_W, RAIL_H);
    lv_obj_set_pos(r, RAIL_X, y + 4);
    lv_obj_set_style_radius(r, RAIL_H / 2, 0);
    lv_obj_set_style_clip_corner(r, true, 0);
    lv_obj_set_style_bg_color(r, LINK_COLOR_SURFACE_ELEVATED, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

    s_fill[i] = lv_obj_create(r);
    lv_obj_remove_style_all(s_fill[i]);
    lv_obj_set_size(s_fill[i], 0, RAIL_H);
    lv_obj_set_style_bg_color(s_fill[i], color, 0);
    lv_obj_set_style_bg_opa(s_fill[i], LV_OPA_COVER, 0);

    s_value[i] = text(body, LINK_FONT_CAPTION, LINK_COLOR_TEXT, 0, y);
    lv_obj_set_width(s_value[i], VIEW_CONTENT_W);
    lv_obj_set_style_text_align(s_value[i], LV_TEXT_ALIGN_RIGHT, 0);
}

static void sheet_write_line(int i, uint32_t n, uint32_t total)
{
    char share[16];

    int32_t px = total > 0 ? (int32_t)((uint64_t)n * RAIL_W / total) : 0;
    if (n > 0 && px < 2) {
        px = 2;
    }
    lv_obj_set_width(s_fill[i], px);

    format_share(n, total, share, sizeof(share));
    lv_label_set_text_fmt(s_value[i], "%lu  %s", (unsigned long)n, share);
}

static void fill_rx(lv_obj_t *body)
{
    char crit[3][16];
    lv_snprintf(crit[0], sizeof(crit[0]), "\xE2\x89\xA5 %d dB",
                LINK_RADIO_MARGIN_GOOD / 10);
    lv_snprintf(crit[1], sizeof(crit[1]), "%d\xE2\x80\x93%d dB",
                LINK_RADIO_MARGIN_FAIR / 10, LINK_RADIO_MARGIN_GOOD / 10);
    lv_snprintf(crit[2], sizeof(crit[2]), "< %d dB",
                LINK_RADIO_MARGIN_FAIR / 10);

    s_head = text(body, LINK_FONT_CAPTION, LINK_COLOR_TEXT_DIM, 0, HEAD_Y);
    lv_label_set_text(text(body, LINK_FONT_CAPTION, LINK_COLOR_TEXT_DIM, 0,
                           LINES_Y),
                      tr(T_SECTION_MARGIN));
    const int32_t top = LINES_Y + 16;
    for (int i = 0; i < 3; i++) {
        sheet_line(body, top + i * LINE_H, crit[i], part_color(i));
    }
    sheet_line(body, top + 3 * LINE_H, tr(T_ERRORS), part_color(3));

    const int32_t y = top + 4 * LINE_H + LINK_PAD_SM;
    lv_label_set_text(text(body, LINK_FONT_CAPTION, LINK_COLOR_TEXT_DIM, 0, y),
                      tr(T_ERRORS_BY_TYPE));
    sheet_line(body, y + 16, tr(T_CRC_ERROR), COLOR_ERROR_BAR);
    sheet_line(body, y + 16 + LINE_H, tr(T_HEADER_INVALID), COLOR_ERROR_BAR);
    sheet_line(body, y + 16 + 2 * LINE_H, tr(T_FRAME_INCOMPLETE),
               COLOR_ERROR_BAR);
}

static void fill_tx(lv_obj_t *body)
{
    s_head = text(body, LINK_FONT_CAPTION, LINK_COLOR_TEXT_DIM, 0, HEAD_Y);
    sheet_line(body, LINES_Y, tr(T_TX_DONE), LINK_COLOR_ACCENT);
    sheet_line(body, LINES_Y + LINE_H, tr(T_TX_TIMEOUT), LINK_COLOR_ACCENT);
    sheet_line(body, LINES_Y + 2 * LINE_H, tr(T_TX_ABORTED), LINK_COLOR_ACCENT);
}

static void sheet_write(const link_stats_t *st)
{
    const uint32_t errors = st->crc + st->hdr + st->incomplete;
    const uint32_t received = st->ok + errors;
    const uint32_t sent = st->tx_ok + st->tx_timeout + st->tx_aborted;

    if (s_open == SHEET_RX) {
        const uint32_t parts[PART_N] = {st->good, st->fair, st->poor, errors};
        const uint32_t types[3] = {st->crc, st->hdr, st->incomplete};

        lv_label_set_text_fmt(s_head, tr(T_RECEIVED_FMT),
                              (unsigned long)received);
        for (int i = 0; i < PART_N; i++) {
            sheet_write_line(i, parts[i], received);
        }
        for (int i = 0; i < 3; i++) {
            sheet_write_line(PART_N + i, types[i], errors);
        }
    } else if (s_open == SHEET_TX) {
        const uint32_t results[3] = {st->tx_ok, st->tx_timeout,
                                     st->tx_aborted};

        lv_label_set_text_fmt(s_head, tr(T_SENT_FMT), (unsigned long)sent);
        for (int i = 0; i < 3; i++) {
            sheet_write_line(i, results[i], sent);
        }
    }
}

static void on_tile_tapped(void *user_data)
{
    const sheet_t which = (sheet_t)(intptr_t)user_data;

    if (s_open != SHEET_NONE) {
        return;
    }
    s_open = which;
    s_lines = 0;
    if (!ui_view_open_sheet(tr(which == SHEET_RX ? T_RECEIVED : T_SENT),
                            which == SHEET_RX ? fill_rx : fill_tx,
                            sheet_forget)) {
        sheet_forget();
        return;
    }

    link_stats_t st;
    link_stats_get(&st);
    sheet_write(&st);
}

/*
 * The page
 */

/* As the menu's tiles; content positions are inside the border. */
static lv_obj_t *build_tile(lv_obj_t *view, sheet_t which, int32_t x,
                            int32_t w, t_id_t title)
{
    lv_obj_t *t = ui_touch_make_tap_button(view, on_tile_tapped,
                                           (void *)(intptr_t)which);
    lv_obj_set_size(t, w, TILE_H);
    lv_obj_set_pos(t, x, TILE_Y);
    lv_obj_set_style_bg_color(t, LINK_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, LINK_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_border_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_add_style(t, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    lv_label_set_text(centred(t, LINK_FONT_BODY, LINK_COLOR_TEXT, -1,
                              TITLE_Y - 1, w),
                      tr(title));
    return t;
}

/* The ring's main part only: no knob, no indicator, not a control. */
static lv_obj_t *build_arc(lv_obj_t *tile, lv_color_t color)
{
    lv_obj_t *a = lv_arc_create(tile);
    lv_obj_set_size(a, RING_D, RING_D);
    lv_obj_set_pos(a, RING_X - 1, RING_Y - 1);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(a, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, color, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
    lv_arc_set_bg_angles(a, 0, 360);
    return a;
}

lv_obj_t *ui_view_stats_create(lv_obj_t *parent)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, VIEW_CONTENT_W, VIEW_CONTENT_H);
    lv_obj_set_style_bg_color(view, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);

    /* A rebuild deletes the sheet without telling: drop its handles. */
    sheet_forget();

    s_session = text(view, LINK_FONT_BODY, LINK_COLOR_TEXT, 0, LINE1_Y);
    s_duration = text(view, LINK_FONT_BODY, LINK_COLOR_TEXT_DIM, 0, LINE1_Y);
    lv_obj_set_width(s_duration, VIEW_CONTENT_W);
    lv_obj_set_style_text_align(s_duration, LV_TEXT_ALIGN_RIGHT, 0);

    /* Sent first: what went out, then what came in. */
    lv_obj_t *tx = build_tile(view, SHEET_TX, 0, TX_W, T_SENT);
    s_tx_num = centred(tx, LINK_FONT_HUGE, LINK_COLOR_ACCENT, -1,
                       RING_Y - 1 + (RING_D - lv_font_get_line_height(
                           LINK_FONT_HUGE)) / 2, TX_W);

    lv_obj_t *rx = build_tile(view, SHEET_RX, RX_X, RX_W, T_RECEIVED);
    build_arc(rx, LINK_COLOR_SURFACE_ELEVATED);
    for (int i = 0; i < PART_N; i++) {
        s_arc[i] = build_arc(rx, part_color(i));
    }
    s_rx_num = centred(rx, LINK_FONT_HUGE, LINK_COLOR_TEXT,
                       RING_X - 1 + (RING_D - MID_W) / 2, 0, MID_W);

    char crit[3][16];
    lv_snprintf(crit[0], sizeof(crit[0]), "\xE2\x89\xA5 %d dB",
                LINK_RADIO_MARGIN_GOOD / 10);
    lv_snprintf(crit[1], sizeof(crit[1]), "%d\xE2\x80\x93%d dB",
                LINK_RADIO_MARGIN_FAIR / 10, LINK_RADIO_MARGIN_GOOD / 10);
    lv_snprintf(crit[2], sizeof(crit[2]), "< %d dB",
                LINK_RADIO_MARGIN_FAIR / 10);
    const char *const key[PART_N] = {crit[0], crit[1], crit[2], tr(T_ERRORS)};

    lv_label_set_text(text(rx, LINK_FONT_CAPTION, LINK_COLOR_TEXT_DIM,
                           KEY_X - 1, KEY_HEAD - 1),
                      tr(T_SECTION_MARGIN));

    for (int i = 0; i < PART_N; i++) {
        const int32_t y = KEY_Y - 1 + i * KEY_H;

        lv_obj_t *dot = lv_obj_create(rx);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, KEY_X - 1, y + 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, part_color(i), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

        lv_label_set_text(text(rx, LINK_FONT_CAPTION, LINK_COLOR_TEXT,
                               KEY_X + 12, y),
                          key[i]);

        s_count[i] = text(rx, LINK_FONT_CAPTION, LINK_COLOR_TEXT,
                          COUNT_X - 1, y);
        lv_obj_set_width(s_count[i], COUNT_W);
        lv_obj_set_style_text_align(s_count[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    return view;
}

/*
 * Clockwise from the top, in the order of the key. A share too thin to be
 * seen is drawn at a minimum, the difference taken from the largest.
 */
static void write_ring(const uint32_t parts[PART_N])
{
    uint32_t total = 0;
    int shown = 0;
    int largest = 0;
    int32_t sweep[PART_N] = {0};

    for (int i = 0; i < PART_N; i++) {
        total += parts[i];
        shown += parts[i] > 0;
        if (parts[i] > parts[largest]) {
            largest = i;
        }
        lv_obj_add_flag(s_arc[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (total == 0) {
        return;
    }
    if (shown == 1) {
        lv_arc_set_bg_angles(s_arc[largest], 0, 360);
        lv_obj_remove_flag(s_arc[largest], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int32_t sum = 0;
    for (int i = 0; i < PART_N; i++) {
        if (parts[i] > 0) {
            sweep[i] = (int32_t)((uint64_t)parts[i] * 360 / total);
            if (sweep[i] < ARC_MIN) {
                sweep[i] = ARC_MIN;
            }
            sum += sweep[i];
        }
    }
    sweep[largest] -= sum - 360;

    int32_t a = 270;
    for (int i = 0; i < PART_N; i++) {
        if (sweep[i] == 0) {
            continue;
        }
        lv_arc_set_bg_angles(s_arc[i], (a + ARC_GAP) % 360,
                             (a + sweep[i] - ARC_GAP) % 360);
        lv_obj_remove_flag(s_arc[i], LV_OBJ_FLAG_HIDDEN);
        a += sweep[i];
    }
}

/* Smaller from small_from on, so the count stays inside its room. */
static void write_count(lv_obj_t *label, uint32_t n, uint32_t small_from,
                        lv_color_t color)
{
    const lv_font_t *font = n < small_from ? LINK_FONT_HUGE : LINK_FONT_TITLE;

    lv_label_set_text_fmt(label, "%lu", (unsigned long)n);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, n > 0 ? color : LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_y(label, RING_Y - 1 + (RING_D
                                      - lv_font_get_line_height(font)) / 2);
}

void ui_view_stats_refresh(void)
{
    link_stats_t st;
    link_stats_get(&st);

    char buf[48];

    if (st.ready) {
        lv_label_set_text_fmt(s_session, tr(T_SESSION_FMT),
                              (unsigned long)st.session);
        format_duration(esp_timer_get_time() / 1000 - st.since_ms, buf,
                        sizeof(buf));
        lv_label_set_text(s_duration, buf);
    } else {
        lv_label_set_text(s_session, tr(T_NO_SESSION));
        lv_label_set_text(s_duration, "");
    }

    const uint32_t errors = st.crc + st.hdr + st.incomplete;
    const uint32_t sent = st.tx_ok + st.tx_timeout + st.tx_aborted;
    const uint32_t parts[PART_N] = {st.good, st.fair, st.poor, errors};

    write_count(s_tx_num, sent, 10000, LINK_COLOR_ACCENT);
    write_ring(parts);
    for (int i = 0; i < PART_N; i++) {
        lv_label_set_text_fmt(s_count[i], "%lu", (unsigned long)parts[i]);
        lv_obj_set_style_text_color(s_count[i], parts[i] > 0
                                                    ? LINK_COLOR_TEXT
                                                    : LINK_COLOR_TEXT_DIM, 0);
    }
    write_count(s_rx_num, st.ok + errors, 1000, LINK_COLOR_TEXT);

    if (s_open != SHEET_NONE && s_head != NULL) {
        sheet_write(&st);
    }
}
