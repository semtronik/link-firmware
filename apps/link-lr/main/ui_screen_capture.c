/*
 * Capture - what the radio is hearing.
 *
 *   status bar   (48)   [<] Capture   o o   date time [batt]
 *   content     (150)   the page, full bleed
 *   action bar   (42)   [profile] [Filtre] [Effacer]
 *
 * Entered from the session screen, never from the home screen: a session
 * is started there and this is what it shows. The chevron goes back to it
 * without ending anything - stopping is a decision taken on the session
 * screen, not a side effect of leaving this one.
 *
 * THE STRUCTURE
 *
 * Three bands, and each one answers a different question. The header says
 * WHERE YOU ARE - a title, the page pips, the clock - and nothing else:
 * it is identical in every mode, so it is learnt once. The action bar says
 * WHAT YOU CAN DO, and it belongs to the mode rather than to the page, so
 * it does not change as you swipe. Between them the page has the glass to
 * itself.
 *
 * That is why the pages carry no heading of their own. A band naming the
 * page, repeating the frame count, holding the filter - all of it was
 * inside each view, and all of it was either said by the header already or
 * belongs in the bar. Taking it out is what lets a page be full bleed.
 *
 * THE PAGES
 *
 * The swipe is not a choice between rival views, it is one mode with
 * several faces, all over the same session:
 *
 *   Journal    the chronology, full bleed, lines rather than columns
 *   Stats      the session in counts, since the last clear
 *
 * Every widget is built once - a refresh writes text, it never allocates.
 */

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "link_events.h"
#include "link_profile.h"
#include "link_radio.h"
#include "link_rec.h"
#include "link_time.h"
#include "ui_helpers.h"
#include "ui_manager.h"
#include "ui_param_edit.h"
#include "ui_view.h"
#include "link_theme.h"
#include "ui_text.h"

static const char *TAG = "ui_capture";

/* A field test's worth of frames - long enough to cover a complete run
 * without scrolling past the interesting part. The monitor is a
 * magnifier, not an archive. */
#define HIST_ROWS 20

/* Development aid: fills the journal with worst-case rows so the layout
 * can be judged on the glass. */
#define CAPTURE_WORST_CASE 0

#define ANIM_MS 250

/*
 * The action bar, at the foot of the SCREEN.
 *
 * It was inside each view for a while, sliding away with the page on the
 * grounds that the actions were the page's actions. With the pages turned
 * into faces of one mode that reasoning inverts: clearing the capture and
 * choosing a profile belong to the capture, not to whichever face is in
 * front, and a control that moves when nothing about it changed is a
 * control you have to find again.
 *
 * Fixed here, it also stops being drawn over anything - the pages are
 * simply the height that is left.
 *
 * Symbols, each in the same slot on every page; the centre is the
 * journal's alone.
 */
#define ACTIONS_H        42
#define ACTIONS_SIDE_W   84
#define ACTIONS_CENTRE_W (LINK_USABLE_W - 2 * ACTIONS_SIDE_W)

/* Between two buttons, and above and below them; none at the two outer
 * edges, so the bar lines up with the rules across the usable width. */
#define ACTIONS_GAP 8

#define ACTIONS_Y (LINK_SCREEN_H - ACTIONS_H)

/* The journal is what boot lands on. */
#define VIEW_JOURNAL   0
#define VIEW_STATS     1
#define VIEW_COUNT     2

static const t_id_t VIEW_TITLES[VIEW_COUNT] = {
    T_VIEW_JOURNAL, T_VIEW_STATS,
};

/* Indexed by page, so the slide and the parking need no cascade of ifs as
 * pages are added. Filled as each one is built. */
static lv_obj_t *s_views[VIEW_COUNT];

static lv_obj_t *s_screen;
static ui_status_bar_handles_t s_status;

/* The detail sheet, NULL when closed. Declared up here because the
 * gesture handler, well above it, has to know it is open. */
static lv_obj_t *s_panel;

static int s_active_view;
static bool s_anim_running;


/* The radio cell's glyph in the action bar. */
static lv_obj_t *s_config_lbl;
static lv_obj_t *s_actions;

static void fill_action_bar(void);

static int filter_options_build(void);

/* Defined with the configure sheet, well below; the panel teardown, the
 * tick and the frame detail all sit above it and need them. The frame
 * detail rises on the same frame and the same animation - two panels that
 * come up from the bottom of one screen are left the same way. */
static void sheet_refresh_rows(void);
static void sheet_forget_rows(void);
static lv_obj_t *sheet_frame(const char *title, const char *symbol,
                             lv_obj_t **out_title);
static void sheet_raise(void);
static void panel_swipe(int step);

/*
 * The journal
 *
 * Two lines per frame, both full width, instead of a row of columns.
 *
 *   14:32:04 +42s  32o  RSSI -78  SNR 8
 *   a1b2c3d4  4A 8B 01 F0 2C 1D 44 90
 *
 * Line 1 is what the receiver measured, the same whatever the protocol.
 * Line 2 is what decoding makes of the frame - the id today, a frame type
 * and named fields once there is a parser - so the line that will change
 * is the one with room to change in.
 *
 * Lines rather than columns is not a matter of taste: a column is cut to
 * its widest value, and in the table the id was cut to four bytes and the
 * payload to five. Here both are whole. A line is also free not to be a
 * frame at all, which is what a table forbids and what this page will need
 * the day a profile change or a reset has to appear in the chronology.
 *
 * The units are dropped from RSSI and SNR: the labels name them, the units
 * never vary, and the widest line needs the 38 px they cost. They are
 * written out in full in the detail sheet.
 */
typedef struct {
    /** Transparent tap area under the labels; opens the panel. */
    lv_obj_t *target;

    /** The left block, two lines: when it arrived and how long after the
     * frame before it, then the payload. */
    lv_obj_t *lbl_time;
    lv_obj_t *lbl_payload;

    /** The three numeric columns, right-aligned and centred on the pair of
     * lines beside them. */
    lv_obj_t *lbl_oct;
    lv_obj_t *lbl_rssi;
    lv_obj_t *lbl_snr;
    lv_obj_t *mark;
    lv_obj_t *chevron;

    /** The rule above the row; NULL on the first, which has none. */
    lv_obj_t *rule;
} log_row_t;

static log_row_t s_log_rows[HIST_ROWS];
static link_radio_frame_t s_hist[HIST_ROWS];

/* LOST and HDR_ERR rows carry only their header's stamps. */
typedef enum {
    HIST_RX,
    HIST_TX,
    HIST_CRC,
    HIST_LOST,
    HIST_HDR_ERR,
} hist_kind_t;

static hist_kind_t s_hist_kind[HIST_ROWS];
static int s_hist_count;
static lv_obj_t *s_log_scroll;
static lv_obj_t *s_log_tail;
static lv_obj_t *s_log_pending;

/* The empty state, and the settings it recalls. */
static lv_obj_t *s_log_empty;
static lv_obj_t *s_log_empty_cfg;

/* The recording session the journal shows; a new one clears it. */
static uint32_t s_session;

/*
 * Which frame each visible row holds, and how many rows are in use.
 *
 * Without a selection this is the identity and could be skipped; with one
 * the rows are packed from the top while the frames they show are
 * scattered through s_hist, and a tap has no other way back to its frame.
 */
static int s_log_row_frame[HIST_ROWS];
static int s_log_shown;

/* Which reception the history has already taken, so one is pushed once
 * rather than on every refresh. */
static uint32_t s_last_entry_seq;

/* The same, for the frames we sent. */
static uint32_t s_last_tx_seq;

/* Set by a push or a reset. The stamps are frozen at capture, so the rows
 * only need rewriting when the content itself changed - not four times a
 * second. */
static bool s_hist_dirty = true;

/* Instant page */

/*
 * What counts as a frame's identity.
 *
 * Where the id sits is the protocol's to say - offset and width - because
 * no one position is right for every protocol. In a
 * Meshtastic header, for one, the first four bytes are the destination and
 * the source is the four after. Whoever is listening knows their own
 * protocol, so the field is off until they say where it is.
 *
 * This is a slice of the payload, not a parsed field. It groups frames
 * whose bytes agree at that offset, which is what the eye already matches
 * rows on, and it means "sender" only insofar as the setting points at a
 * sender.
 *
 * What is fixed here is the ceiling: the width the buffers below are cut
 * for.
 */
#define HIST_ID_BYTES 8

/* The protocol's id field: off until a decoding profile sets it. */
static uint8_t s_id_offset;
static uint8_t s_id_len;

/*
 * The id selector.
 *
 * Empty means "All": every frame is listed. Otherwise it holds the id the
 * user picked, and rows whose id differs are hidden - the frames are still
 * captured and still counted, only the page is narrowed.
 *
 * A string rather than the bytes, because that is what the picker deals in
 * and what the cell shows; the comparison is on the same text the row
 * displays, so what is on the glass is what is being matched.
 */
static char s_filter_id[2 * HIST_ID_BYTES + 1];

/* The field the selection was made against, to notice it moving. */
static uint8_t s_filter_id_offset;
static uint8_t s_filter_id_len;

/**
 * What the selector shows when no id is selected.
 *
 * Masculine plural, agreeing with the ids it stands among: every other
 * entry in this list is one, so the neutral entry answers the same
 * question - which id? - rather than counting frames.
 */
#define FILTER_ALL_LABEL tr(T_FILTER_ALL)

/* The selector's rows. The options string is held rather than built on the
 * stack: lv_dropdown_set_options_static() keeps the pointer. */
static char s_filter_ids[HIST_ROWS][2 * HIST_ID_BYTES + 1];
static char s_filter_options[(HIST_ROWS + 1) * (2 * HIST_ID_BYTES + 2)];
static int s_filter_id_count;

/* This screen is the root, so its left slot opens the menu rather than
 * going back. Reception carries on regardless. */
static void on_menu_tapped(void *user_data)
{
    (void)user_data;
    ui_manager_switch_to(UI_SCREEN_MENU);
}

static void anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x(obj, v);
}

static void anim_done_cb(lv_anim_t *a)
{
    (void)a;

    /*
     * Park every page that is not the active one, so a stray gesture
     * cannot scroll one back into sight.
     *
     * Each is parked on the side it lives on relative to the one in front,
     * so a page two places away is waiting on the correct edge for the
     * next swipe to bring it in from there.
     */
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (s_views[i] == NULL || i == s_active_view) {
            continue;
        }

        lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_views[i],
                     i < s_active_view ? -LINK_USABLE_W : LINK_USABLE_W);
    }

    s_anim_running = false;
}

static void slide(lv_obj_t *out_view, int32_t out_to, lv_obj_t *in_view,
                  int32_t in_from)
{
    lv_obj_remove_flag(in_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(in_view, in_from);

    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, out_view);
    lv_anim_set_exec_cb(&out, anim_x_cb);
    lv_anim_set_values(&out, 0, out_to);
    lv_anim_set_duration(&out, ANIM_MS);
    lv_anim_set_path_cb(&out, lv_anim_path_ease_out);
    lv_anim_start(&out);

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, in_view);
    lv_anim_set_exec_cb(&in, anim_x_cb);
    lv_anim_set_values(&in, in_from, 0);
    lv_anim_set_duration(&in, ANIM_MS);
    lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&in, anim_done_cb);
    lv_anim_start(&in);

    s_anim_running = true;
}

static void switch_view(int index)
{
    if (s_anim_running || index == s_active_view || index < 0
        || index >= VIEW_COUNT) {
        return;
    }

    lv_obj_t *const out_view = s_views[s_active_view];
    lv_obj_t *const in_view = s_views[index];

    if (out_view == NULL || in_view == NULL) {
        return;
    }

    /* Which way the pages travel: a higher index arrives from the right
     * and pushes the current one out to the left. */
    const bool forward = index > s_active_view;

    s_active_view = index;
    ui_helpers_status_bar_set_page(&s_status, index);
    lv_label_set_text(s_status.lbl_title, tr(VIEW_TITLES[index]));
    fill_action_bar();

    /* The journal is only refreshed while it is in front, so frames that
     * landed behind it have to be painted on arrival. */
    s_hist_dirty = true;

    slide(out_view, forward ? -LINK_USABLE_W : LINK_USABLE_W, in_view,
          forward ? LINK_USABLE_W : -LINK_USABLE_W);
}

/* Set the flag on every descendant, rather than remembering it at each of
 * the several dozen creation sites and getting one wrong. */
static void bubble_gestures(lv_obj_t *obj)
{
    const uint32_t count = lv_obj_get_child_count(obj);

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        lv_obj_add_flag(child, LV_OBJ_FLAG_GESTURE_BUBBLE);
        bubble_gestures(child);
    }
}

static void on_gesture(lv_event_t *e)
{
    (void)e;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    /* The panel covers the pages, so a swipe over it turns the panel's own
     * pages, never the journal's behind it. */
    if (s_panel != NULL) {
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            panel_swipe(dir == LV_DIR_LEFT ? 1 : -1);
            lv_indev_wait_release(indev);
        }
        return;
    }

    /* Vertical gestures are left entirely alone. At 8 px the detector
     * fires before LVGL has picked a scroll object, so the start of a
     * scroll down the journal arrives here too - swallowing the contact
     * would make the list impossible to scroll. */
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) {
        return;
    }

    /* One step, not a jump to an end: a swipe moves to the neighbour, and
     * the ends simply do not move. */
    const int next = s_active_view + (dir == LV_DIR_LEFT ? 1 : -1);

    if (next >= 0 && next < VIEW_COUNT) {
        switch_view(next);
    }

    /* Otherwise the same gesture keeps firing as the finger travels. */
    lv_indev_wait_release(indev);
}

/* Gap since the previous frame, at the precision that gap deserves: a
 * beacon's period reads as "+42s", two retransmissions as "+420ms", a link
 * gone quiet as "+3h". Empty for the first frame, which has nothing to be
 * measured from. */
static void format_delta(int64_t delta_ms, char *buf, size_t size)
{
    if (delta_ms < 0) {
        buf[0] = '\0';
    } else if (delta_ms < 1000) {
        lv_snprintf(buf, size, "+%dms", (int)delta_ms);
    } else if (delta_ms < 10000) {
        lv_snprintf(buf, size, "+%d.%ds", (int)(delta_ms / 1000),
                    (int)((delta_ms % 1000) / 100));
    } else if (delta_ms < 60000) {
        lv_snprintf(buf, size, "+%ds", (int)(delta_ms / 1000));
    } else if (delta_ms < 3600000) {
        lv_snprintf(buf, size, "+%dmin", (int)(delta_ms / 60000));
    } else {
        /* Capped, so the line has a width. Minutes stopped at an hour
         * because "+1439min" is both wider than anything else here and
         * read by counting its digits; past that the hour is the unit
         * anyone waiting on a link thinks in. */
        int64_t h = delta_ms / 3600000;
        if (h > 999) {
            h = 999;
        }
        lv_snprintf(buf, size, "+%dh", (int)h);
    }
}

/*
 * When the frame arrived: the day and the time.
 *
 * A stamp of 0 means the clock was never set.
 */
static void format_stamp(int64_t epoch_ms, char *buf, size_t size)
{
    if (epoch_ms <= 0 || !link_time_is_valid()) {
        lv_snprintf(buf, size, "--/--/-- --:--:--");
        return;
    }

    /* Local time, so it matches the status bar's clock. */
    const time_t secs = (time_t)(epoch_ms / 1000)
                        + (time_t)link_time_get_offset_minutes() * 60;
    struct tm t;
    gmtime_r(&secs, &t);

    lv_snprintf(buf, size, "%02u/%02u/%02u %02u:%02u:%02u",
                (unsigned)t.tm_mday, (unsigned)(t.tm_mon + 1),
                (unsigned)(t.tm_year % 100), (unsigned)t.tm_hour,
                (unsigned)t.tm_min, (unsigned)t.tm_sec);
}

/* The time alone, for a journal row. */
static void format_clock(int64_t epoch_ms, char *buf, size_t size)
{
    if (epoch_ms <= 0 || !link_time_is_valid()) {
        lv_snprintf(buf, size, "--:--:--");
        return;
    }

    const time_t secs = (time_t)(epoch_ms / 1000)
                        + (time_t)link_time_get_offset_minutes() * 60;
    struct tm t;
    gmtime_r(&secs, &t);

    lv_snprintf(buf, size, "%02u:%02u:%02u", (unsigned)t.tm_hour,
                (unsigned)t.tm_min, (unsigned)t.tm_sec);
}

/*
 * The configured bytes run together, as an identifier rather than as data
 * - see HIST_ID_BYTES for what the field means.
 *
 * Empty when the id is turned off, and empty when the frame stops short of
 * the field: nothing is padded, because a padded id would claim bytes that
 * were never received.
 */
static void format_id_n(const link_radio_frame_t *f, int max_bytes, char *buf,
                        size_t size)
{
    buf[0] = '\0';

    if (s_id_len == 0) {
        return;
    }

    /* The bytes have to have arrived. A frame too short for the configured
     * field has no id rather than a truncated one, which would read as a
     * different transmitter. */
    if ((unsigned)s_id_offset + s_id_len > f->kept) {
        return;
    }

    const int n = s_id_len < max_bytes ? s_id_len : max_bytes;
    size_t used = 0;

    /* Lower case, the way Meshtastic and the tools around it write a node
     * number. */
    for (int i = 0; i < n && used + 2 < size; i++) {
        used += lv_snprintf(buf + used, size - used, "%02x",
                            f->payload[s_id_offset + i]);
    }
}

/*
 * What the rows and the selector deal in.
 *
 * The full configured width, because the journal's line 2 has room for it
 * - the table's 76 px column had to cut it to four bytes, and the
 * selector then had to match on the cut value.
 */
static void format_id(const link_radio_frame_t *f, char *buf, size_t size)
{
    format_id_n(f, HIST_ID_BYTES, buf, size);
}

static void format_payload(const link_radio_frame_t *f, int max_bytes,
                           char *buf, size_t size)
{
    if (f->kept == 0) {
        lv_snprintf(buf, size, "%s", tr(T_EMPTY_PAYLOAD));
        return;
    }

    const int n = f->kept < max_bytes ? f->kept : max_bytes;
    size_t used = 0;

    for (int i = 0; i < n && used + 2 < size; i++) {
        used += lv_snprintf(buf + used, size - used, "%02X", f->payload[i]);
    }

    if (f->len > n && used + 4 < size) {
        lv_snprintf(buf + used, size - used, " ...");
    }
}

/* ------------------------------------------------------------------ */
/* The journal page                                                    */
/* ------------------------------------------------------------------ */

/*
 * A grid: a heading naming the columns, a rule, then the rows under it.
 *
 *   TRAME (147)              OCT  RSSI  SNR
 *   ----------------------------------------
 *   14:32:04 +42s             32   -78    8
 *   FE 88 12 00 AA FF 1C 9D
 *
 * Columns, because the point of a list of frames is to compare one against
 * the next: right-aligned numbers stack under each other, and a value that
 * has moved is seen without being read. A flow of labelled lines reads one
 * frame well and a column of frames badly, which is the wrong way round
 * here.
 *
 * The left block takes two lines - when it arrived and how long after the
 * frame before it, then the payload. The three numbers take one, centred
 * on the pair.
 *
 * WHAT IS SHOWN, AND WHY ALL OF IT
 *
 * Time, delta, length, RSSI, SNR, payload. Not a field to trim: the delta
 * is how a beacon's period or a retransmission shows itself, the length is
 * how a truncated frame does, and RSSI without SNR says how loud a signal
 * is without saying whether it decodes. Diagnosing needs the set.
 *
 * The id had a column of its own and lost it. It cost 76 px to show four
 * bytes of the eight it holds - the worst trade on the row - and those
 * 76 px are what lets the time fit at M16 and the payload run to eight
 * bytes. It will come back on the payload line, whole, beside the frame
 * type, when there is a parser to name one.
 *
 * SIZES
 *
 * Time and the three numbers in M16, column headings in M12, payload in
 * body text. The payload is a signature to match against the row above
 * rather than something read digit by digit, and in M16 it would run to
 * six bytes where it now runs to eight.
 */

/*
 * The band above the rows:
 *
 *    8  air
 *   16  column headings, M14
 *    2  air
 *    1  rule
 *    3  air
 *
 * 30 px, and they earn it: centred numbers are only readable under a
 * heading that names them, and the left one carries the legend of the
 * rows' marks.
 *
 * At 2 px from the top the headings sat all but touching the screen
 * header; 8 lets the band read as filled rather than pushed up.
 */
#define VC_HEADER_Y 8
#define VC_HEADER_H 16
#define VC_SEP_Y    (VC_HEADER_Y + VC_HEADER_H + 2)

#define VC_FIRST_ROW_Y (VC_SEP_Y + 1 + 3)

/*
 * Exactly three rows, with a rule between rows as in every other list of
 * the product: a row is a cell that opens, and a row cut by the action bar
 * read as unfinished. 40 px each, the two body lines centred in it.
 */
#define VC_SCROLL_H     (VIEW_CONTENT_H - VC_FIRST_ROW_Y)
#define VC_VISIBLE_ROWS 3
#define VC_ROW_H        (VC_SCROLL_H / VC_VISIBLE_ROWS)

/* Line 2 sits a full body box below line 1, so the boxes touch and nothing
 * is added between them. */
#define VC_LINE1_DY ((VC_ROW_H - 32) / 2)
#define VC_LINE2_DY (VC_LINE1_DY + 16)

/*
 * The three numeric columns.
 *
 * One width for all three, so they read as a rule rather than as three
 * fields each trimmed to its own contents. 34 holds the widest value any
 * of them takes ("-120", 32 px) and the widest heading.
 */
#define VC_COL_NUM_W 34
#define VC_COL_GAP   2

/*
 * The scrollbar's lane, at the right of the usable width. The bar is drawn
 * inside the scrolling area, over the content, so the rows stop short of
 * it; the heading's rule runs on over it to the usable edge.
 */
#define VC_SCROLLBAR_W LINK_PAD_SM

/* The chevron that says a row opens, at the end of the row where lists put
 * it. Centred in a slot wider than the glyph, so it does not sit pressed
 * against the scrollbar's lane. */
#define VC_CHEVRON_W 20

#define VC_CHEVRON_X (LINK_USABLE_W - VC_SCROLLBAR_W - VC_CHEVRON_W)
#define VC_SNR_X  (VC_CHEVRON_X - VC_COL_NUM_W)
#define VC_RSSI_X (VC_SNR_X - VC_COL_GAP - VC_COL_NUM_W)
#define VC_OCT_X  (VC_RSSI_X - VC_COL_GAP - VC_COL_NUM_W)

/*
 * Everything the columns leave: 166 px, and both lines are measured
 * against it. Eight bytes and " ..." take about 157 in practice; the
 * widest digits run past it, and line 2 ends in dots then.
 *
 * Line 1 carries the time without the date: it hardly changes within a
 * session, and the detail sheet gives it.
 *
 * The direction is a bar down the left edge of the row rather than a
 * word here, which would need another 24 px this line does not have.
 */
/* After the direction mark, which starts where the rules start. */
#define VC_LEFT_X (VC_MARK_W + 3)
#define VC_LEFT_W (VC_OCT_X - VC_LEFT_X - VC_COL_GAP)

/* In the margin the text already leaves, so it costs the line nothing. */
#define VC_MARK_W      3
#define VC_MARK_INSET  VC_LINE1_DY

/* The header's legend squares, in the colours of the marks. */
#define VC_MARK_SZ  6
#define VC_MARK_GAP 3

/* Between the legend's items, added to VC_MARK_GAP. */
#define VC_LEGEND_GAP 4

/* What the payload line previews, now that the left block has the width
 * for it: enough to tell one kind of frame from another, which is all the
 * raw bytes are for. */
#define LOG_PREVIEW_BYTES 8

static void on_log_row_tapped(void *user_data);

/* One field on line 1 or 2: left-aligned in its slot, clipped rather than
 * wrapped so a long value cannot push the row apart. */
static lv_obj_t *log_field(lv_obj_t *parent, int32_t x, int32_t w, int32_t y,
                           const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    /* One line high: with the height left to its content, a text too long
     * wraps onto a second line instead of being clipped or dotted. */
    lv_obj_set_size(lbl, w, font->line_height);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(lbl, x, y);

    return lbl;
}

/* One column heading: on the same axis as the values under it. */
static void column_header(lv_obj_t *parent, const char *text, int32_t x)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, VC_COL_NUM_W);
    lv_obj_set_pos(lbl, x, VC_HEADER_Y);
}

/*
 * One numeric cell: centred in its column, and centred on the two lines of
 * the frame block beside it.
 *
 * Centred rather than right-aligned, which is the usual rule for numbers
 * read down a column: the trade is that the digits no longer stack - a
 * value's own width decides how far in it sits, so "9" and "255" end on
 * different verticals. Worth looking at on the glass before it is settled,
 * because the three columns carry values of near-equal width in practice
 * and the drift may be smaller than the rule suggests.
 */
static lv_obj_t *column_cell(lv_obj_t *parent, int32_t x, int32_t row_y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, LINK_FONT_VALUE, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, VC_COL_NUM_W);
    lv_obj_set_pos(lbl, x, row_y + (VC_ROW_H - 18) / 2);

    return lbl;
}

/* A coloured square and what it marks. */
static void header_legend(lv_obj_t *row, lv_color_t color, int32_t gap_before,
                          const char *text)
{
    lv_obj_t *sq = lv_obj_create(row);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, VC_MARK_SZ, VC_MARK_SZ);
    lv_obj_set_style_margin_left(sq, gap_before, 0);
    lv_obj_set_style_bg_color(sq, color, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_add_flag(sq, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT_DIM, 0);
}

static lv_obj_t *build_view_journal(lv_obj_t *parent)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, LINK_USABLE_W, VIEW_CONTENT_H);
    lv_obj_set_pos(view, 0, 0);
    lv_obj_set_style_bg_color(view, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* The legend of the rows' marks, in the left heading cell. */
    lv_obj_t *hdr = lv_obj_create(view);
    lv_obj_remove_style_all(hdr);
    /* Starts where the rule under it starts, so the square does not
     * overhang it. */
    lv_obj_set_size(hdr, VC_LEFT_X + VC_LEFT_W, VC_HEADER_H);
    lv_obj_set_pos(hdr, 0, VC_HEADER_Y);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, VC_MARK_GAP, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_GESTURE_BUBBLE);

    header_legend(hdr, LINK_COLOR_ACCENT_FG, 0, "TX");
    header_legend(hdr, LINK_COLOR_TEXT_DIM, VC_LEGEND_GAP, "RX");

    /* The time since the newest frame, right after the legend, as far
     * from it as RX is from TX. In the heading rather than over the rows:
     * it is a state, not a frame, and it stays in sight while the list is
     * scrolled. */
    s_log_pending = lv_label_create(hdr);
    lv_label_set_text(s_log_pending, "");
    lv_obj_set_style_text_font(s_log_pending, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_log_pending, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_margin_left(s_log_pending, VC_LEGEND_GAP, 0);
    lv_obj_add_flag(s_log_pending, LV_OBJ_FLAG_HIDDEN);

    column_header(view, "LEN", VC_OCT_X);
    column_header(view, "RSSI", VC_RSSI_X);
    column_header(view, "SNR", VC_SNR_X);

    lv_obj_t *sep = lv_obj_create(view);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LINK_USABLE_W, 1);
    lv_obj_set_pos(sep, 0, VC_SEP_Y);
    lv_obj_set_style_bg_color(sep, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sep, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Only the rows scroll; the heading and its rule stay put, so the
     * columns are still named halfway down the grid. Vertical only: a
     * horizontal drag is left to bubble up as a page change. */
    s_log_scroll = lv_obj_create(view);
    lv_obj_remove_style_all(s_log_scroll);
    /* Sized to whole rows, so nothing is shown half-written: a row that
     * fits shows both its lines or is not there at all. */
    lv_obj_set_size(s_log_scroll, LINK_USABLE_W, VC_SCROLL_H);
    lv_obj_set_pos(s_log_scroll, 0, VC_FIRST_ROW_Y);
    lv_obj_set_scroll_dir(s_log_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_log_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_log_scroll, LINK_COLOR_TEXT_DIM,
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_log_scroll, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_log_scroll, LINK_PAD_SM, LV_PART_SCROLLBAR);
    lv_obj_add_flag(s_log_scroll, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Shown once the list is full, where the user goes looking for the
     * older frames that are no longer there. */
    s_log_tail = lv_label_create(s_log_scroll);
    lv_label_set_text_fmt(s_log_tail, tr(T_LOG_TAIL_FMT),
                          HIST_ROWS);
    lv_obj_set_style_text_font(s_log_tail, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_log_tail, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_log_tail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_log_tail, LINK_USABLE_W - 2 * LINK_PAD_SM);
    lv_obj_set_pos(s_log_tail, LINK_PAD_SM, HIST_ROWS * VC_ROW_H + LINK_PAD_SM);
    lv_obj_add_flag(s_log_tail, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < HIST_ROWS; i++) {
        const int32_t y = i * VC_ROW_H;
        const int32_t y1 = y + VC_LINE1_DY;
        const int32_t y2 = y + VC_LINE2_DY;

        /*
         * A transparent target under the labels, so the row can be tapped.
         * First child at this y, so the labels that follow are drawn over
         * it; it takes the touch and they take the pixels.
         */
        s_log_rows[i].target = ui_touch_make_scroll_safe_button(
            s_log_scroll, on_log_row_tapped, (void *)(intptr_t)i);
        lv_obj_remove_style_all(s_log_rows[i].target);
        lv_obj_set_size(s_log_rows[i].target, LINK_USABLE_W, VC_ROW_H);
        lv_obj_set_pos(s_log_rows[i].target, 0, y);
        lv_obj_set_style_bg_opa(s_log_rows[i].target, LV_OPA_TRANSP, 0);
        lv_obj_add_style(s_log_rows[i].target, &link_style_btn_pressed,
                         LV_STATE_PRESSED);
        lv_obj_remove_flag(s_log_rows[i].target, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_log_rows[i].target, LV_OBJ_FLAG_GESTURE_BUBBLE);

        /* The left block, line 1: the day, the time, and how long after the
         * frame before it. */
        s_log_rows[i].lbl_time =
            log_field(s_log_scroll, VC_LEFT_X, VC_LEFT_W, y1,
                      LINK_FONT_BODY, LINK_COLOR_TEXT);

        /* Line 2: the payload. A signature to match against the row above
         * rather than something read digit by digit. */
        s_log_rows[i].lbl_payload =
            log_field(s_log_scroll, VC_LEFT_X, VC_LEFT_W, y2,
                      LINK_FONT_BODY, LINK_COLOR_TEXT_DIM);
        /* Eight bytes fit in practice, not always: wide hex digits run
         * past the edge, and a dotted end still says "more follows". */
        lv_label_set_long_mode(s_log_rows[i].lbl_payload, LV_LABEL_LONG_DOT);

        /* The direction, in the margin the text leaves: a word on line 1
         * would need 24 px it does not have. */
        lv_obj_t *mark = lv_obj_create(s_log_scroll);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark, VC_MARK_W, VC_ROW_H - 2 * VC_MARK_INSET);
        lv_obj_set_pos(mark, 0, y + VC_MARK_INSET);
        lv_obj_set_style_radius(mark, VC_MARK_W / 2, 0);
        lv_obj_set_style_bg_color(mark, LINK_COLOR_ACCENT_FG, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_GESTURE_BUBBLE);
        s_log_rows[i].mark = mark;

        s_log_rows[i].lbl_oct = column_cell(s_log_scroll, VC_OCT_X, y);
        s_log_rows[i].lbl_rssi = column_cell(s_log_scroll, VC_RSSI_X, y);
        s_log_rows[i].lbl_snr = column_cell(s_log_scroll, VC_SNR_X, y);

        lv_obj_t *chev = lv_label_create(s_log_scroll);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(chev, LINK_FONT_BODY, 0);
        lv_obj_set_style_text_color(chev, LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_align(chev, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(chev, VC_CHEVRON_W);
        lv_obj_set_pos(chev, VC_CHEVRON_X,
                       y + (VC_ROW_H - LINK_FONT_BODY->line_height) / 2);
        lv_obj_add_flag(chev, LV_OBJ_FLAG_GESTURE_BUBBLE);
        s_log_rows[i].chevron = chev;

        s_log_rows[i].rule = NULL;
        if (i > 0) {
            lv_obj_t *rule = lv_obj_create(s_log_scroll);
            lv_obj_remove_style_all(rule);
            /* Stops at the scrollbar's lane, which the rows leave free. */
            lv_obj_set_size(rule, LINK_USABLE_W - VC_SCROLLBAR_W, 1);
            lv_obj_set_pos(rule, 0, y);
            lv_obj_set_style_bg_color(rule, LINK_COLOR_BORDER, 0);
            lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
            lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(rule, LV_OBJ_FLAG_GESTURE_BUBBLE);
            s_log_rows[i].rule = rule;
        }
    }

    /* Over the row area, centred in the page. With nothing received, the
     * settings in force are the first thing to check against the sender. */
    s_log_empty = lv_obj_create(view);
    lv_obj_remove_style_all(s_log_empty);
    lv_obj_set_size(s_log_empty, LINK_USABLE_W, VIEW_CONTENT_H);
    lv_obj_set_pos(s_log_empty, 0, 0);
    lv_obj_set_flex_flow(s_log_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_log_empty, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_log_empty, LINK_PAD_SM, 0);
    lv_obj_remove_flag(s_log_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_log_empty, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *empty = lv_label_create(s_log_empty);
    lv_label_set_text(empty, tr(T_NO_FRAME));
    lv_obj_set_style_text_font(empty, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(empty, LINK_COLOR_TEXT_DIM, 0);

    s_log_empty_cfg = lv_label_create(s_log_empty);
    lv_label_set_text(s_log_empty_cfg, "");
    lv_obj_set_style_text_font(s_log_empty_cfg, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_log_empty_cfg, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_log_empty_cfg, LV_TEXT_ALIGN_CENTER, 0);

    return view;
}

/* ------------------------------------------------------------------ */
/* Panel                                                              */
/* ------------------------------------------------------------------ */

/*
 * A sheet that rises from the bottom and covers the screen: a fixed title
 * and date band, then the sections in one scroll.
 *
 * The screen underneath stays live - frames keep arriving and the list
 * keeps being repainted. The frame on display is frozen at open: the list
 * pushes rows down as it grows, and a detail that changed under the eye
 * would be worse than one that is plainly a snapshot.
 *
 * It is not on the back stack, like the other overlays. That means it has
 * to be closed by hand when the screen changes, or it would outlive what
 * it was opened over.
 */
#define PANEL_Y       0
#define PANEL_H       LINK_SCREEN_H
#define PANEL_TITLE_H LINK_HEADER_H
#define PANEL_ANIM_MS 200

#define PANEL_BODY_Y (PANEL_TITLE_H)
#define PANEL_BODY_H (PANEL_H - PANEL_TITLE_H)

/* Row of the history the panel is showing, frozen at open. */
static link_radio_frame_t s_panel_frame;
static hist_kind_t s_panel_kind;

/* Two pages, swiped like the journal's: what the frame holds, then how
 * it was received or sent - so the payload is never under the diagnosis. */
#define PANEL_PAGES 2
static lv_obj_t *s_panel_pages[PANEL_PAGES];
static int s_panel_page;
static bool s_panel_sliding;
static ui_status_bar_handles_t s_panel_bar;
static lv_obj_t *s_panel_help;

/* Written once at open. */
static lv_obj_t *s_panel_body;
static lv_obj_t *s_panel_stamp;
static lv_obj_t *s_panel_sect;
static lv_obj_t *s_panel_crc;
static lv_obj_t *s_panel_bytes;

static void panel_close(void);
static void capture_send(void);
#if CAPTURE_WORST_CASE
static void fill_worst_case(void);
#endif

static bool s_send_pending;

/* Told when a page's sheet has gone, so the page drops its handles. */
static void (*s_sheet_closed)(void);

static void on_panel_close_tapped(void *user_data)
{
    (void)user_data;
    panel_close();
}

#define PANEL_BAND_H 30

static lv_obj_t *panel_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static lv_obj_t *panel_label(lv_obj_t *parent, const lv_font_t *font,
                             lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

static lv_obj_t *panel_section(lv_obj_t *body, const char *title,
                               lv_obj_t **out_title)
{
    lv_obj_t *sect = panel_box(body);
    lv_obj_set_flex_flow(sect, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(sect, LINK_PAD_LG, 0);
    lv_obj_set_style_pad_top(sect, LINK_PAD_MD + LINK_PAD_XS, 0);
    lv_obj_set_style_pad_row(sect, LINK_PAD_XS, 0);

    lv_obj_t *lbl = panel_label(sect, LINK_FONT_CAPTION, LINK_COLOR_ACCENT_FG);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    if (out_title != NULL) {
        *out_title = lbl;
    }

    return sect;
}

/* Returns the value label. */
static lv_obj_t *panel_row(lv_obj_t *sect, const char *name)
{
    lv_obj_t *row = panel_box(sect);

    lv_obj_t *lbl = panel_label(row, LINK_FONT_BODY, LINK_COLOR_TEXT_DIM);
    lv_label_set_text(lbl, name);

    lv_obj_t *value = panel_label(row, LINK_FONT_BODY, LINK_COLOR_TEXT);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, 0, 0);

    return value;
}


static void panel_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

/* The panel's own labels, given up with it. */
static void panel_forget(void)
{
    s_panel_body = NULL;
    s_panel_stamp = NULL;
    for (int i = 0; i < PANEL_PAGES; i++) {
        s_panel_pages[i] = NULL;
    }
    s_panel_page = 0;
    s_panel_sliding = false;
    s_panel_bar = (ui_status_bar_handles_t){0};
    s_panel_help = NULL;
    s_panel_sect = NULL;
    s_panel_crc = NULL;
    s_panel_bytes = NULL;
}

static void panel_deleted_cb(lv_anim_t *a)
{
    (void)a;

    if (s_panel != NULL) {
        lv_obj_delete(s_panel);
        s_panel = NULL;
    }

    /*
     * The sheet's labels went with the panel. They are written on the tick,
     * which does not know the sheet has gone - so they are cleared here,
     * and the tick's own null check does the rest.
     */
    sheet_forget_rows();
    panel_forget();

    if (s_sheet_closed != NULL) {
        void (*closed)(void) = s_sheet_closed;
        s_sheet_closed = NULL;
        closed();
    }

    /* Sent once the sheet is gone, so the new row is not drawn under it. */
    if (s_send_pending) {
        s_send_pending = false;
        capture_send();
    }
}

static void panel_close(void)
{
    if (s_panel == NULL) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, panel_anim_y_cb);
    lv_anim_set_values(&a, PANEL_Y, LINK_SCREEN_H);
    lv_anim_set_duration(&a, PANEL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, panel_deleted_cb);
    lv_anim_start(&a);
}

static lv_color_t margin_color(int32_t tenths)
{
    if (tenths >= LINK_RADIO_MARGIN_GOOD) {
        return LINK_COLOR_SUCCESS;
    }
    return tenths >= LINK_RADIO_MARGIN_FAIR ? LINK_COLOR_WARNING
                                            : LINK_COLOR_ERROR;
}

/* Whole dB, rounded rather than truncated. */
static int snr_db_rounded(const link_radio_frame_t *f)
{
    const int32_t t = link_radio_snr_tenths(f);
    return (int)((t >= 0 ? t + 5 : t - 5) / 10);
}

static void format_tenths(int32_t t, const char *unit, char *buf, size_t size)
{
    const int32_t mag = t < 0 ? -t : t;
    snprintf(buf, size, "%s%ld.%ld %s", t < 0 ? "-" : (t > 0 ? "+" : ""),
             (long)(mag / 10), (long)(mag % 10), unit);
}

#define HELP_W     280
#define HELP_BTN_W 96
#define HELP_Q_SZ  22

static void help_close_cb(void *arg)
{
    (void)arg;
    if (s_panel_help != NULL) {
        lv_obj_delete(s_panel_help);
        s_panel_help = NULL;
    }
}

/* Deferred: the tap that closes it is still being handled by its button. */
static void on_help_close(void *user_data)
{
    (void)user_data;
    lv_async_call(help_close_cb, NULL);
}

/* A card over a dimmed panel, closed by OK or by a tap beside it. */
static void on_margin_help(void *user_data)
{
    (void)user_data;
    if (s_panel == NULL || s_panel_help != NULL) {
        return;
    }

    lv_obj_t *dim = ui_touch_make_tap_button(s_panel, on_help_close, NULL);
    lv_obj_set_size(dim, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    s_panel_help = dim;

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, HELP_W, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, LINK_COLOR_SURFACE_ELEVATED, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, LINK_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, LINK_PAD_LG, 0);
    lv_obj_set_style_pad_row(card, LINK_PAD_MD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = panel_label(card, LINK_FONT_TITLE, LINK_COLOR_TEXT);
    lv_label_set_text(title, tr(T_SECTION_MARGIN));

    lv_obj_t *text = panel_label(card, LINK_FONT_BODY, LINK_COLOR_TEXT);
    lv_obj_set_width(text, LV_PCT(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(text, tr(T_MARGIN_EXPLAIN));

    /* The colours' criteria, as the stats legend gives them. */
    char crit[3][16];
    snprintf(crit[0], sizeof(crit[0]), "\xE2\x89\xA5 %d dB", LINK_RADIO_MARGIN_GOOD / 10);
    snprintf(crit[1], sizeof(crit[1]), "%d\xE2\x80\x93%d dB",
             LINK_RADIO_MARGIN_FAIR / 10, LINK_RADIO_MARGIN_GOOD / 10);
    snprintf(crit[2], sizeof(crit[2]), "< %d dB", LINK_RADIO_MARGIN_FAIR / 10);
    const int32_t at[3] = {LINK_RADIO_MARGIN_GOOD, LINK_RADIO_MARGIN_FAIR,
                           LINK_RADIO_MARGIN_FAIR - 1};

    lv_obj_t *key = lv_obj_create(card);
    lv_obj_remove_style_all(key);
    lv_obj_set_size(key, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(key, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(key, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(key, LINK_PAD_SM, 0);
    lv_obj_remove_flag(key, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(key);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, margin_color(at[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        if (i > 0) {
            lv_obj_set_style_margin_left(dot, LINK_PAD_LG, 0);
        }

        lv_obj_t *lbl = panel_label(key, LINK_FONT_BODY, LINK_COLOR_TEXT);
        lv_label_set_text(lbl, crit[i]);
    }

    lv_obj_t *ok = ui_touch_make_tap_button(card, on_help_close, NULL);
    lv_obj_set_size(ok, HELP_BTN_W, LINK_HEADER_BTN_SZ);
    lv_obj_set_style_bg_color(ok, LINK_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ok, LINK_RADIUS_BTN, 0);
    lv_obj_t *ok_lbl = panel_label(ok, LINK_FONT_BODY, LINK_COLOR_TEXT_ON_ACCENT);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_center(ok_lbl);
}

/*
 * A round "?" after a row's name, with a target of a finger's size.
 *
 * The row is laid out as a flex line so it grows to the button's height:
 * name, button, a spacer, then the value pushed to the right.
 */
static void panel_help_button(lv_obj_t *value)
{
    lv_obj_t *row = lv_obj_get_parent(value);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LINK_PAD_MD, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *btn = ui_touch_make_tap_button(row, on_margin_help, NULL);
    lv_obj_set_size(btn, HELP_Q_SZ, HELP_Q_SZ);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(btn, LINK_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_ext_click_area(btn, (LINK_HEADER_BTN_SZ - HELP_Q_SZ) / 2);
    lv_obj_move_to_index(btn, 1);

    lv_obj_t *spacer = panel_box(row);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_move_to_index(spacer, 2);

    lv_obj_t *q = panel_label(btn, LINK_FONT_CAPTION, LINK_COLOR_ACCENT_FG);
    lv_label_set_text(q, "?");
    lv_obj_center(q);
}

/*
 * The margin as a sum on the glass: the SNR, the limit of the SF, their
 * difference and its class, with the thresholds that class comes from.
 */
static void panel_build_margin(lv_obj_t *body)
{
    const link_radio_frame_t *f = &s_panel_frame;
    char name[24];
    char buf[128];

    lv_obj_t *sect = panel_section(body, tr(T_SECTION_MARGIN), NULL);

    format_tenths(link_radio_snr_tenths(f), "dB", buf, sizeof(buf));
    lv_label_set_text(panel_row(sect, tr(T_SNR_MEASURED)), buf);

    snprintf(name, sizeof(name), tr(T_SNR_LIMIT_FMT), (unsigned)f->radio.sf);
    format_tenths(link_radio_snr_limit_tenths(f->radio.sf), "dB", buf,
                  sizeof(buf));
    lv_label_set_text(panel_row(sect, name), buf);

    const int32_t m = link_radio_margin_tenths(f);
    char value[24];
    format_tenths(m, "dB", value, sizeof(value));
    lv_obj_t *margin = panel_row(sect, tr(T_MARGIN));
    lv_label_set_text(margin, value);
    lv_obj_set_style_text_color(margin, margin_color(m), 0);
    panel_help_button(margin);
}

/* Readings only: what they mean is for the reader to judge. */
static void panel_build_channel(lv_obj_t *body, bool measured)
{
    const link_radio_frame_t *f = &s_panel_frame;
    if (!measured && f->noise_n == 0 && f->inst_n == 0) {
        return;
    }

    char name[24];
    char buf[32];
    lv_obj_t *sect = panel_section(body, tr(T_SECTION_CHANNEL), NULL);

    if (measured) {
        lv_label_set_text_fmt(panel_row(sect, tr(T_SIGNAL)), "%d dBm",
                              (int)f->signal_dbm);
    }
    if (f->noise_n > 0) {
        lv_label_set_text_fmt(panel_row(sect, tr(T_NOISE_BEFORE)), "%d dBm",
                              (int)f->noise_dbm);
    }
    if (f->inst_n > 0) {
        snprintf(name, sizeof(name), tr(T_READINGS_FMT), (unsigned)f->inst_n);
        snprintf(buf, sizeof(buf), tr(T_RANGE_DBM_FMT), (int)f->inst_min_dbm,
                 (int)f->inst_max_dbm);
        lv_label_set_text(panel_row(sect, name), buf);
    }
}

/* What came with the frame. Coding rate and CRC are the header's, so a
 * sender set otherwise than the profile shows here. */
static void panel_build_frame(lv_obj_t *body, bool measured)
{
    const link_radio_frame_t *f = &s_panel_frame;
    link_profile_cfg_t profile;
    link_profile_get_current(&profile);
    const sx1262_radio_cfg_t *cfg = &profile.radio;
    char a[16];
    char b[16];
    char buf[40];

    lv_obj_t *sect = panel_section(body, tr(T_SECTION_FRAME), NULL);

    lv_obj_t *toa = panel_row(sect, tr(T_TIME_ON_AIR));
    if (f->toa_us != 0) {
        lv_label_set_text_fmt(toa, "%lu ms", (unsigned long)(f->toa_us / 1000));
    } else {
        lv_label_set_text(toa, "--");
    }

    lv_obj_t *cr = panel_row(sect, tr(T_CODING_RATE));
    lv_obj_t *crc = panel_row(sect, "CRC");
    if (f->radio.cr == 0) {
        lv_label_set_text(cr, "--");
        lv_label_set_text(crc, "--");
    } else {
        snprintf(a, sizeof(a), "4/%u", (unsigned)f->radio.cr);
        snprintf(b, sizeof(b), "4/%u", (unsigned)cfg->cr);
        if (f->radio.cr != cfg->cr) {
            snprintf(buf, sizeof(buf), tr(T_PROFILE_DIFFERS_FMT), a, b);
            lv_label_set_text(cr, buf);
        } else {
            lv_label_set_text(cr, a);
        }

        snprintf(a, sizeof(a), "%s", tr(f->radio.crc_on ? T_YES : T_NO));
        snprintf(b, sizeof(b), "%s", tr(cfg->crc_on ? T_YES : T_NO));
        if (f->radio.crc_on != cfg->crc_on) {
            snprintf(buf, sizeof(buf), tr(T_PROFILE_DIFFERS_FMT), a, b);
            lv_label_set_text(crc, buf);
        } else {
            lv_label_set_text(crc, a);
        }
    }

    if (measured) {
        lv_label_set_text_fmt(panel_row(sect, tr(T_FREQ_OFFSET)), "%ld Hz",
                              (long)f->freq_error_hz);
    }
}

static void panel_slide_done_cb(lv_anim_t *a)
{
    (void)a;
    for (int i = 0; i < PANEL_PAGES; i++) {
        if (s_panel_pages[i] != NULL && i != s_panel_page) {
            lv_obj_add_flag(s_panel_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_panel_sliding = false;
}

static void panel_slide(lv_obj_t *obj, int32_t from, int32_t to, bool last)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (last) {
        lv_anim_set_completed_cb(&a, panel_slide_done_cb);
    }
    lv_anim_start(&a);
}

static void panel_switch(int page)
{
    if (s_panel_sliding || page == s_panel_page || page < 0
        || page >= PANEL_PAGES) {
        return;
    }

    lv_obj_t *out = s_panel_pages[s_panel_page];
    lv_obj_t *in = s_panel_pages[page];
    const int32_t w = LINK_SCREEN_W;
    const bool forward = page > s_panel_page;

    s_panel_page = page;
    ui_helpers_status_bar_set_page(&s_panel_bar, page);

    lv_obj_remove_flag(in, LV_OBJ_FLAG_HIDDEN);
    s_panel_sliding = true;
    panel_slide(out, 0, forward ? -w : w, false);
    panel_slide(in, forward ? w : -w, 0, true);
}

static void panel_swipe(int step)
{
    /* The configure sheet shares the panel slot and has no pages. */
    if (s_panel_pages[0] == NULL) {
        return;
    }
    panel_switch(s_panel_page + step);
}

/* One page of the panel: under the date, scrolling on its own. */
static lv_obj_t *panel_page(lv_obj_t *panel)
{
    lv_obj_t *page = lv_obj_create(panel);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LINK_SCREEN_W, PANEL_BODY_H - PANEL_BAND_H);
    lv_obj_set_pos(page, 0, PANEL_BODY_Y + PANEL_BAND_H);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(page, LINK_PAD_LG, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    /* remove_style_all took the theme's scrollbar with it. */
    lv_obj_set_style_bg_color(page, LINK_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, LINK_PAD_SM, LV_PART_SCROLLBAR);
    return page;
}

/* Write the frame into the panel, at open. Another frame is reached by
 * leaving and tapping that one. */
static void panel_write(void)
{
    char stamp[24];

    format_stamp(s_panel_frame.epoch_ms, stamp, sizeof(stamp));
    lv_label_set_text(s_panel_stamp, stamp);

    /*
     * A failed CRC means one or more bits are wrong and nothing says
     * which, so the bytes are shown - they are what the capture is for -
     * but no parser is run over them: named fields read off corrupt bytes
     * are worse than none.
     */
    const bool unread =
        s_panel_kind == HIST_LOST || s_panel_kind == HIST_HDR_ERR;
    const bool failed = s_panel_frame.crc_error || unread;

    if (unread) {
        lv_label_set_text(s_panel_sect, tr(T_DATA));
    } else if (s_panel_frame.len == 1) {
        lv_label_set_text(s_panel_sect, tr(T_DATA_ONE));
    } else {
        lv_label_set_text_fmt(s_panel_sect, tr(T_DATA_FMT),
                              (unsigned)s_panel_frame.len);
    }
    lv_obj_set_style_text_color(s_panel_sect,
                                failed ? LINK_COLOR_ERROR : LINK_COLOR_ACCENT_FG,
                                0);

    if (s_panel_kind == HIST_LOST) {
        lv_label_set_text(s_panel_crc, tr(T_FRAME_INCOMPLETE_LONG));
        lv_obj_remove_flag(s_panel_crc, LV_OBJ_FLAG_HIDDEN);
    } else if (s_panel_kind == HIST_HDR_ERR) {
        lv_label_set_text(s_panel_crc, tr(T_HEADER_INVALID_LONG));
        lv_obj_remove_flag(s_panel_crc, LV_OBJ_FLAG_HIDDEN);
    } else if (s_panel_frame.crc_error) {
        lv_label_set_text(s_panel_crc, tr(T_CRC_FAILED_LONG));
        lv_obj_remove_flag(s_panel_crc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_panel_crc, LV_OBJ_FLAG_HIDDEN);
    }

    if (unread) {
        lv_obj_add_flag(s_panel_bytes, LV_OBJ_FLAG_HIDDEN);
    } else {
        char payload[3 * 64];
        format_payload(&s_panel_frame, 64, payload, sizeof(payload));
        lv_label_set_text(s_panel_bytes, payload);
        lv_obj_remove_flag(s_panel_bytes, LV_OBJ_FLAG_HIDDEN);
    }

    /* Back to the top: a shorter payload after a longer one would leave
     * the view scrolled past its own content. */
    lv_obj_scroll_to_y(s_panel_body, 0, LV_ANIM_OFF);
}

static void panel_open(const link_radio_frame_t *f)
{
    if (s_panel != NULL) {
        return;
    }

    s_panel_frame = *f;

    /*
     * The same frame as the configure sheet: back on the left, the title
     * centred, nothing on the right.
     *
     * This used to build its own bar with a cross in the corner. Two
     * panels that rise from the bottom of the same screen and are left the
     * same way have no business closing by different glyphs, and the
     * duplicate drifted from the sheet as soon as the sheet's header
     * changed.
     */
    lv_obj_t *title = NULL;
    lv_obj_t *panel = sheet_frame(tr(T_FRAME_DETAILS), LV_SYMBOL_LEFT, &title);

    /* The date stays put; everything under it scrolls. */
    lv_obj_t *band = panel_box(panel);
    lv_obj_set_size(band, LINK_SCREEN_W, PANEL_BAND_H);
    lv_obj_set_pos(band, 0, PANEL_BODY_Y);
    lv_obj_set_style_bg_color(band, LINK_COLOR_SURFACE_ELEVATED, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(band, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(band, 1, 0);
    lv_obj_set_style_border_color(band, LINK_COLOR_BORDER, 0);

    s_panel_stamp = panel_label(band, LINK_FONT_TITLE, LINK_COLOR_TEXT);
    lv_obj_center(s_panel_stamp);

    lv_obj_t *body = panel_page(panel);
    s_panel_body = body;
    s_panel_pages[0] = body;

    lv_obj_t *oct = panel_section(body, "", &s_panel_sect);

    s_panel_crc = panel_label(oct, LINK_FONT_CAPTION, LINK_COLOR_ERROR);
    lv_obj_add_flag(s_panel_crc, LV_OBJ_FLAG_HIDDEN);

    s_panel_bytes = panel_label(oct, LINK_FONT_BODY, LINK_COLOR_TEXT);
    lv_obj_set_width(s_panel_bytes, LV_PCT(100));
    lv_label_set_long_mode(s_panel_bytes, LV_LABEL_LONG_WRAP);

    /* Reception measures only for what was received: a transmission has
     * none, and a frame whose header failed has no signal or SNR. */
    lv_obj_t *diag = panel_page(panel);
    s_panel_pages[1] = diag;
    const bool measured = s_panel_kind == HIST_RX || s_panel_kind == HIST_CRC;
    if (measured) {
        panel_build_margin(diag);
    }
    if (s_panel_kind != HIST_TX) {
        panel_build_channel(diag, measured);
    }
    panel_build_frame(diag, measured);
    lv_obj_add_flag(diag, LV_OBJ_FLAG_HIDDEN);

    s_panel_bar.bar = lv_obj_get_child(panel, 0);
    s_panel_bar.lbl_title = title;
    ui_helpers_status_bar_add_dots(&s_panel_bar, PANEL_PAGES, 0);

    /* Up to the screen's handler, the way the journal's pages go. */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    bubble_gestures(panel);

    panel_write();
    sheet_raise();
}

/* Through the map, not straight into s_hist: under a selection the nth row
 * is not the nth frame. */
static void on_log_row_tapped(void *user_data)
{
    const int row = (int)(intptr_t)user_data;

    if (row < 0 || row >= s_log_shown) {
        return;
    }

    s_panel_kind = s_hist_kind[s_log_row_frame[row]];
    panel_open(&s_hist[s_log_row_frame[row]]);
}

/* ------------------------------------------------------------------ */
/* The id selector                                                     */
/* ------------------------------------------------------------------ */

/*
 * A selector, not a filter dialog: the cell shows which id is being looked
 * at and opens a list of the ones on offer. That is why the label is a
 * value with a chevron rather than the word "Filter" - a verb would
 * promise an action, where this holds a state the user changes by picking
 * from a list.
 *
 * It lives in the action bar. It used to be the heading over the table's
 * id column - the column was where an id was read, so it was where one was
 * picked. With the table gone and the journal carrying no heading band,
 * narrowing the list is simply one of the things this mode can do, and the
 * bar is where those live.
 */

static void filter_set(const char *id)
{
    if (id == NULL) {
        s_filter_id[0] = '\0';
    } else {
        lv_snprintf(s_filter_id, sizeof(s_filter_id), "%s", id);
    }

    /* The rows themselves decide what to show, so ask for a rewrite. */
    s_hist_dirty = true;
}

/** True when the frame passes the current selection. */
static bool filter_accepts(const link_radio_frame_t *f)
{
    if (s_filter_id[0] == '\0') {
        return true;
    }

    char id[2 * HIST_ID_BYTES + 1];
    format_id(f, id, sizeof(id));

    return strcmp(id, s_filter_id) == 0;
}

/*
 * Rebuild the list from what is in the history right now, and say which
 * entry is the current selection.
 *
 * Called just before the list unrolls rather than kept up to date: an id
 * that arrives while it is open would shift the indices under the choice
 * being made. Freezing it costs nothing, since the list is opened to
 * change the selection anyway.
 */
/* Unused while the filter row is greyed - it is the list the row will open
 * once a parser JSON says where a frame's id sits. Kept rather than deleted
 * and rewritten: nothing about it is wrong except that there is currently no
 * honest field to match on. */
__attribute__((unused)) static int filter_options_build(void)
{
    size_t used = 0;
    int selected = 0;

    s_filter_id_count = 0;
    used += lv_snprintf(s_filter_options, sizeof(s_filter_options), "%s",
                        FILTER_ALL_LABEL);

    for (int i = 0; i < s_hist_count; i++) {
        char id[2 * HIST_ID_BYTES + 1];
        format_id(&s_hist[i], id, sizeof(id));

        if (id[0] == '\0') {
            continue;
        }

        bool seen = false;
        for (int j = 0; j < s_filter_id_count; j++) {
            if (strcmp(s_filter_ids[j], id) == 0) {
                seen = true;
                break;
            }
        }
        if (seen || s_filter_id_count >= HIST_ROWS) {
            continue;
        }

        lv_snprintf(s_filter_ids[s_filter_id_count],
                    sizeof(s_filter_ids[s_filter_id_count]), "%s", id);
        used += lv_snprintf(s_filter_options + used,
                            sizeof(s_filter_options) - used, "\n%s", id);

        s_filter_id_count++;

        if (strcmp(id, s_filter_id) == 0) {
            selected = s_filter_id_count;
        }
    }

    /*
     * The selected id can have aged out of the history. It is kept as the
     * selection all the same, so the list has to offer it - otherwise the
     * dropdown would open on "All" and silently drop the filter the rows
     * are still being narrowed by.
     */
    if (s_filter_id[0] != '\0' && selected == 0
        && s_filter_id_count < HIST_ROWS) {
        lv_snprintf(s_filter_ids[s_filter_id_count],
                    sizeof(s_filter_ids[s_filter_id_count]), "%s",
                    s_filter_id);
        used += lv_snprintf(s_filter_options + used,
                            sizeof(s_filter_options) - used, "\n%s",
                            s_filter_id);

        s_filter_id_count++;
        selected = s_filter_id_count;
    }

    return selected;
}

/* ------------------------------------------------------------------ */
/* The action bar                                                      */
/* ------------------------------------------------------------------ */

static void on_raz_tapped(void *user_data)
{
    (void)user_data;

    s_hist_count = 0;
    memset(s_hist, 0, sizeof(s_hist));
    memset(s_hist_kind, 0, sizeof(s_hist_kind));
    s_hist_dirty = true;

    lv_obj_add_flag(s_log_tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_log_empty, LV_OBJ_FLAG_HIDDEN);

    /* Kill the scroll animation before jumping: an inertial scroll still
     * running would survive the jump and leave the list off-position. */
    lv_anim_delete(s_log_scroll, NULL);
    lv_obj_scroll_to_y(s_log_scroll, 0, LV_ANIM_OFF);

    ESP_LOGD(TAG, "history cleared");
}

static lv_obj_t *action_cell_at(lv_obj_t *bar, int32_t x, int32_t w,
                                const char *symbol, const lv_font_t *font,
                                bool accent, ui_touch_tap_cb_t on_tap,
                                lv_obj_t **out_label)
{
    const int32_t inset_l = x == 0 ? 0 : ACTIONS_GAP / 2;
    const int32_t inset_r = x + w == LINK_USABLE_W ? 0 : ACTIONS_GAP / 2;
    const int32_t btn_w = w - inset_l - inset_r;

    lv_obj_t *btn = ui_touch_make_tap_button(bar, on_tap, NULL);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, btn_w, ACTIONS_H - ACTIONS_GAP);
    lv_obj_set_pos(btn, x + inset_l, ACTIONS_GAP / 2);
    lv_obj_set_style_bg_color(btn, LINK_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LINK_RADIUS_BTN, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, font, 0);
    /* Accent on the glyph only: the colour of our own frames' marks. */
    lv_obj_set_style_text_color(lbl, accent ? LINK_COLOR_ACCENT_FG
                                            : LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, btn_w - 2 * LINK_PAD_XS);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_center(lbl);

    if (out_label != NULL) {
        *out_label = lbl;
    }

    return btn;
}

/*
 * ---------------------------------------------------------------------
 * The configure sheet - what the mode is set to
 * ---------------------------------------------------------------------
 *
 * Titled "Reglages capture".
 *
 *   PROFIL RADIO
 *   Meshtastic                            >
 *   868 MHz  SF6  125 kHz  4/5  0x12
 *   ---------------------------------------
 *   PROTOCOLE                         aucun
 *   ---------------------------------------
 *   FILTRE ID                          tous
 *
 * The labels carry no accents: the compiled Montserrat holds ASCII and the
 * LVGL symbols, nothing of Latin-1 beyond the degree sign, so "REGLAGES"
 * is spelt the way "FREQUENCE" already is on the profile screen. Adding
 * the accented range is a font rebuild, not a string change.
 *
 * ONE ENTRY THAT IS A BLOCK, TWO THAT ARE LINES
 *
 * The sheet answers one question - what is this capture running on - and
 * eight layouts failed to say that the radio settings ARE the profile
 * rather than sit beside it: indentation, a box round the two, a pill
 * straddling that box. Every one was a drawing of a relationship, and every
 * one had to be decoded before it could be read.
 *
 * Nothing draws it here. The profile is simply given three lines of its
 * own - its kind, its name, and underneath, in small, what it amounts to -
 * while the parser and the filter get one line each. The difference in size
 * is the whole statement: the settings are part of the profile entry, not a
 * neighbour of it, because they are inside the same block of text.
 *
 * The labels are set in capitals and dimmed, so they read as the kind of a
 * thing rather than as a word competing with the value beside it. Hairlines
 * separate the three entries - without them the profile's third line and
 * the parser's label run together into one paragraph.
 *
 * Tapping the profile opens a screen where the selector heads the list and
 * the six parameters follow it: changing profile rewrites them in place, in
 * front of the user, which is the containment demonstrated rather than
 * drawn.
 *
 * The parser and the filter are peers of the profile, not lesser versions
 * of it: the same radio settings can be decoded several ways, and the
 * filter configures nothing at all - it narrows what the rows show. Both
 * wait on the parser JSON; both are shown all the same, because their
 * absence is worth knowing.
 */
typedef enum {
    SHEET_PROFILE,
    SHEET_PARSER,
    SHEET_FILTER,
    SHEET_ROW_COUNT,
} sheet_row_t;

/* The profile entry carries two labels that change, not one. */
static lv_obj_t *s_sheet_rows[SHEET_ROW_COUNT];
static lv_obj_t *s_sheet_radio;

/* Trailing zeros go, but the point stays only if something follows it:
 * "868.3", "868", never "868." - as the settings screen writes it. */
static void format_freq_mhz(uint32_t khz, char *buf, size_t size)
{
    const unsigned mhz = khz / 1000;
    unsigned frac = khz % 1000;

    if (frac == 0) {
        lv_snprintf(buf, size, "%u MHz", mhz);
        return;
    }

    while (frac % 10 == 0) {
        frac /= 10;
    }

    lv_snprintf(buf, size, "%u.%u MHz", mhz, frac);
}

/* The margins inside the usable width, as the journal's rows have them:
 * nothing on the left, the scrollbar's lane on the right, kept free even
 * when the sheet does not scroll so that every list ends at the same x. */
#define SHEET_ROW_X 0
#define SHEET_ROW_R VC_SCROLLBAR_W

/* A one-line entry: label left, value right. */
#define SHEET_ROW_H 44

/*
 * The profile entry: the caption, the name, and the settings under it.
 * Three text boxes of 14, 18 and 14 px with air between them. The name is
 * M16 - the largest size the build carries, M18 not being compiled in.
 */
#define SHEET_BLOCK_H    76
#define SHEET_CAPTION_DY 6
#define SHEET_NAME_DY    24
#define SHEET_RADIO_DY   50

/* The chevron's column, taken out of the width of anything beside it. The
 * journal's, so chevrons line up from one screen to the next. */
#define SHEET_CHEV_W VC_CHEVRON_W

/* The label column on a one-line entry. Wide enough for "FILTRE ID". */
#define SHEET_LABEL_W 88

/* Called when the sheet is torn down: the labels are gone, and the tick
 * must stop writing to them. */
static void sheet_forget_rows(void)
{
    for (int i = 0; i < SHEET_ROW_COUNT; i++) {
        s_sheet_rows[i] = NULL;
    }
    s_sheet_radio = NULL;
}

/*
 * The profile row opens the profile screen rather than a picker: the
 * selector is up there, with the settings it commands underneath, and
 * choosing from here would have shown the name changing with no sign of
 * what changed with it.
 */
static void on_sheet_profile_tapped(void *user_data)
{
    (void)user_data;

    ui_manager_switch_to(UI_SCREEN_CONFIG);
}

static void on_sheet_row_tapped(void *user_data)
{
    (void)user_data;

    /* The parser and the filter both wait on the parser JSON. */
}

/* A hairline between two entries, inset to the text margin. */
static void sheet_separator(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LINK_USABLE_W - SHEET_ROW_X - SHEET_ROW_R, 1);
    lv_obj_set_pos(line, LINK_USABLE_X + SHEET_ROW_X, y);
    lv_obj_set_style_bg_color(line, LINK_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

/*
 * The profile: three lines in one target, with the chevron centred against
 * all of them. The caption above the name and the settings below it are
 * what make this one entry rather than a name with a stray line under it.
 */
static void sheet_block(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *btn = ui_touch_make_tap_button(parent, on_sheet_profile_tapped,
                                             NULL);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LINK_USABLE_W, SHEET_BLOCK_H);
    lv_obj_set_pos(btn, LINK_USABLE_X, y);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, LINK_RADIUS_BTN, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(btn);
    lv_label_set_text(cap, tr(T_RADIO_PROFILE_CAPS));
    lv_obj_set_style_text_font(cap, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(cap, LINK_COLOR_ACCENT_FG, 0);
    lv_obj_set_pos(cap, SHEET_ROW_X, SHEET_CAPTION_DY);

    /* The name is the one thing here read from across a desk. An imported
     * profile can be called anything, so it is cut rather than allowed to
     * run under the chevron - the screen it opens shows it in full. */
    lv_obj_t *name = lv_label_create(btn);
    lv_label_set_text(name, "");
    lv_obj_set_style_text_font(name, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(name, LINK_COLOR_TEXT, 0);
    lv_obj_set_width(name, LINK_USABLE_W - SHEET_ROW_X - SHEET_ROW_R
                               - SHEET_CHEV_W);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name, SHEET_ROW_X, SHEET_NAME_DY);

    /*
     * The whole signature, in the caption size. It runs to 215 px at its
     * widest ("868.3 MHz  SF12  500 kHz  4/8  0xFF") against the 252 px
     * the block leaves; a size up overruns, and this is a line one checks
     * rather than reads - the name above carries the weight.
     */
    lv_obj_t *radio = lv_label_create(btn);
    lv_label_set_text(radio, "");
    lv_obj_set_style_text_font(radio, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(radio, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(radio, LINK_USABLE_W - SHEET_ROW_X - SHEET_ROW_R);
    lv_label_set_long_mode(radio, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(radio, SHEET_ROW_X, SHEET_RADIO_DY);

    /* Centred on the block, not on the name: it opens all three lines. */
    lv_obj_t *chev = lv_label_create(btn);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chev, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(chev, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(chev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(chev, SHEET_CHEV_W);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -SHEET_ROW_R, 0);

    s_sheet_rows[SHEET_PROFILE] = name;
    s_sheet_radio = radio;
}

/* A one-line entry: the kind on the left, the value on the right. */
static void sheet_row(lv_obj_t *parent, sheet_row_t row, const char *label,
                      int32_t y, bool enabled)
{
    lv_obj_t *btn = ui_touch_make_tap_button(parent, on_sheet_row_tapped,
                                             (void *)(intptr_t)row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LINK_USABLE_W, SHEET_ROW_H);
    lv_obj_set_pos(btn, LINK_USABLE_X, y);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, LINK_RADIUS_BTN, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    if (enabled) {
        lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, LINK_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_ACCENT_FG, 0);
    lv_obj_set_style_text_opa(lbl, enabled ? LV_OPA_COVER : LINK_OPA_INERT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SHEET_ROW_X, 0);

    const int32_t chev_w = SHEET_CHEV_W;

    lv_obj_t *val = lv_label_create(btn);
    lv_label_set_text(val, "");
    lv_obj_set_style_text_font(val, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(val, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_opa(val, enabled ? LV_OPA_COVER : LINK_OPA_INERT, 0);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(val,
                     LINK_USABLE_W - SHEET_LABEL_W - SHEET_ROW_R - chev_w);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -SHEET_ROW_R - chev_w, 0);

    s_sheet_rows[row] = val;

    /* An inert row keeps its chevron, faded: a missing arrow says nothing,
     * a dimmed one says not yet. */
    lv_obj_t *chev = lv_label_create(btn);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chev, LINK_FONT_BODY, 0);
    lv_obj_set_style_text_color(chev, LINK_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_opa(chev, enabled ? LV_OPA_COVER : LINK_OPA_INERT, 0);
    lv_obj_set_style_text_align(chev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(chev, SHEET_CHEV_W);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -SHEET_ROW_R, 0);
}

/* Read back from what is in force, never from what was set. */
static void sheet_refresh_rows(void)
{
    if (s_sheet_rows[SHEET_PROFILE] == NULL) {
        return;
    }

    lv_label_set_text(s_sheet_rows[SHEET_PROFILE],
                      link_profile_name(link_profile_current()));

    /*
     * What the profile amounts to, run together as one line rather than set
     * out in columns: it is read as a signature, the way it is in the list
     * that the profile screen opens.
     *
     * The sync word and not the transmit power. Five settings fit here and
     * these are the five that decide whether a frame is heard at all - a
     * sync word one byte off is the classic reason a capture stays empty
     * with everything else right, and it cannot be guessed from the screen
     * the way silence from a wrong SF can. The power matters when this
     * device transmits, which is not what the capture does; it stays one
     * tap away on the profile screen.
     */
    link_profile_cfg_t profile;
    link_profile_get_current(&profile);
    const sx1262_radio_cfg_t *cfg = &profile.radio;

    char freq[16];
    format_freq_mhz(cfg->freq_hz / 1000, freq, sizeof(freq));

    char bw[12];
    link_profile_format_bw(cfg->bw_hz, bw, sizeof(bw));

    char summary[64];
    lv_snprintf(summary, sizeof(summary), "%s  SF%u  %s kHz  4/%u  0x%02X",
                freq, (unsigned)cfg->sf, bw, (unsigned)cfg->cr,
                (unsigned)cfg->sync_word);
    lv_label_set_text(s_sheet_radio, summary);

    lv_label_set_text(s_sheet_rows[SHEET_PARSER], tr(T_NONE));

    lv_label_set_text(s_sheet_rows[SHEET_FILTER],
                      s_filter_id[0] != 0 ? s_filter_id : FILTER_ALL_LABEL);
}

/*
 * The sheet's title bar, built like every settings header: the way back on
 * the left, the title centred, nothing on the right.
 *
 * A chevron rather than a cross, because that is what the rest of the
 * firmware uses to leave a screen, and a sheet is not different enough to
 * deserve its own gesture.
 *
 * Nothing on the right on purpose. Every row here commits as it is tapped -
 * choosing a profile applies it to the radio on the spot - so a validate
 * button would confirm something already done, and would imply that leaving
 * by the chevron discards it. One way out, and it means the same thing
 * whichever row was touched.
 */
static lv_obj_t *sheet_frame(const char *title, const char *symbol,
                             lv_obj_t **out_title)
{
    s_panel = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LINK_SCREEN_W, PANEL_H);
    lv_obj_set_pos(s_panel, 0, LINK_SCREEN_H);
    lv_obj_set_style_bg_color(s_panel, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(s_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &link_style_status_bar, 0);
    lv_obj_set_size(bar, LINK_SCREEN_W, PANEL_TITLE_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = ui_helpers_create_header_button(
        s_panel, symbol, on_panel_close_tapped, NULL);
    lv_obj_set_pos(back, LINK_BEZEL_MARGIN,
                   (PANEL_TITLE_H - LINK_HEADER_BTN_SZ) / 2);

    /* Centred on the bar, not on what the back button leaves: a title that
     * shifts with the presence of a button is not centred, it is floating.
     * The width stops it running under the button on either side. */
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, LINK_SCREEN_W - 2 * (LINK_BEZEL_MARGIN
                                               + LINK_HEADER_BTN_SZ));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    if (out_title != NULL) {
        *out_title = lbl;
    }

    return s_panel;
}

static void sheet_raise(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, panel_anim_y_cb);
    lv_anim_set_values(&a, LINK_SCREEN_H, PANEL_Y);
    lv_anim_set_duration(&a, PANEL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

bool ui_view_open_sheet(const char *title, ui_view_sheet_fill_t fill,
                        void (*closed)(void))
{
    if (s_panel == NULL && s_screen != NULL) {
        lv_obj_t *sheet = sheet_frame(title, LV_SYMBOL_LEFT, NULL);

        lv_obj_t *body = lv_obj_create(sheet);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, VIEW_CONTENT_W, VIEW_SHEET_H);
        lv_obj_set_pos(body, LINK_USABLE_X, PANEL_BODY_Y + LINK_PAD_SM);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

        fill(body);
        bubble_gestures(sheet);
        s_sheet_closed = closed;
        sheet_raise();
        return true;
    }
    return false;
}

static void on_session_tapped(void *user_data)
{
    (void)user_data;

    if (s_panel != NULL) {
        return;
    }

    sheet_forget_rows();

    lv_obj_t *sheet = sheet_frame(tr(T_CAPTURE_SETTINGS), LV_SYMBOL_LEFT, NULL);

    int32_t y = PANEL_BODY_Y + LINK_PAD_SM;

    /* The profile as a block, the other two as lines, hairlines between. */
    sheet_block(sheet, y);
    y += SHEET_BLOCK_H;

    sheet_separator(sheet, y);
    y += 1;

    sheet_row(sheet, SHEET_PARSER, tr(T_PROTOCOL), y, false);
    y += SHEET_ROW_H;

    sheet_separator(sheet, y);
    y += 1;

    sheet_row(sheet, SHEET_FILTER, tr(T_ID_FILTER), y, false);

    sheet_refresh_rows();
    sheet_raise();
}


/*
 * ---------------------------------------------------------------------
 * The actions sheet - what can be done to what was captured
 * ---------------------------------------------------------------------
 *
 * The other family. Clearing, and later sending and exporting: they act on
 * the result rather than on how it is being gathered, which is why they are
 * not on the session sheet.
 *
 * One of them today, so the cell carries its name rather than the word
 * "Actions": a menu holding a single entry is a menu for nothing, and a
 * label that names what happens needs no opening at all. The day there are
 * several, the cell becomes a burger or an ellipsis and opens a list on top
 * - never a screen of its own, which is far more than choosing among three
 * verbs deserves.
 */
/* The counts restart with the session, at the recorder. */
static void clear_view(void)
{
    on_raz_tapped(NULL);
}

/* Starts a new recording session, which clears the view. */
static void on_clear_tapped(void *user_data)
{
    (void)user_data;
    link_rec_close("clear");
    clear_view();
}

/*
 * The bar belongs to the screen, so it is built once and stays put as the
 * pages slide behind it.
 *
 *   [ Session ]   [ Effacer ]
 *
 * Two families, and the whole width split between them: what the mode is set
 * to, and what can be done to what it captured. Six intentions were
 * competing for three cells - profile, parser, radio values, filter, clear,
 * and knowing which profile is in force - and no arrangement of six separate
 * controls fitted 304 px. Grouping them by the question they answer does,
 * and two cells of 148 px are easier to hit than three of 95.
 */
/*
 * The frames on offer. One without a protocol; a protocol adds its own,
 * which is the only way to compose something a third-party device will
 * react to.
 */
static const t_id_t TX_FRAMES[] = {
    T_TEST_FRAME,
};

#define TX_FRAME_COUNT ((int)(sizeof(TX_FRAMES) / sizeof(TX_FRAMES[0])))

/* Picking sends and closes; the cross closes without sending. Two taps
 * rather than one, so nothing goes on air by accident - and the sheet
 * states what is about to be transmitted. */
static void on_tx_frame_tapped(void *user_data)
{
    (void)user_data;
    s_send_pending = true;
    panel_close();
}

static void tx_sheet_open(void)
{
    if (s_panel != NULL) {
        return;
    }

    lv_obj_t *sheet = sheet_frame(tr(T_FRAME_TO_SEND), LV_SYMBOL_CLOSE, NULL);

    int32_t y = PANEL_BODY_Y;

    for (int i = 0; i < TX_FRAME_COUNT; i++) {
        lv_obj_t *btn = ui_touch_make_tap_button(sheet, on_tx_frame_tapped,
                                                 (void *)(intptr_t)i);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, LINK_USABLE_W, SHEET_ROW_H);
        lv_obj_set_pos(btn, LINK_USABLE_X, y);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tr(TX_FRAMES[i]));
        lv_obj_set_style_text_font(lbl, LINK_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, LINK_COLOR_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SHEET_ROW_X, 0);

        lv_obj_t *chev = lv_label_create(btn);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(chev, LINK_FONT_BODY, 0);
        lv_obj_set_style_text_color(chev, LINK_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_align(chev, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(chev, SHEET_CHEV_W);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -SHEET_ROW_R, 0);

        y += SHEET_ROW_H;

        if (i + 1 < TX_FRAME_COUNT) {
            sheet_separator(sheet, y);
        }
    }

    sheet_raise();
}

static void on_send_tapped(void *user_data)
{
    (void)user_data;
    tx_sheet_open();
}

static void fill_action_bar(void)
{
    lv_obj_clean(s_actions);
    s_config_lbl = NULL;

    action_cell_at(s_actions, 0, ACTIONS_SIDE_W, LV_SYMBOL_SETTINGS,
                   LINK_FONT_TITLE, false, on_session_tapped, &s_config_lbl);

    action_cell_at(s_actions, ACTIONS_SIDE_W, ACTIONS_CENTRE_W,
                   LINK_SYMBOL_SEND, LINK_FONT_ICONS, true, on_send_tapped,
                   NULL);

    action_cell_at(s_actions, LINK_USABLE_W - ACTIONS_SIDE_W, ACTIONS_SIDE_W,
                   LV_SYMBOL_TRASH, LINK_FONT_TITLE, false, on_clear_tapped,
                   NULL);
}

static void build_action_bar(lv_obj_t *screen)
{
    s_actions = lv_obj_create(screen);
    lv_obj_remove_style_all(s_actions);
    lv_obj_set_size(s_actions, LINK_USABLE_W, ACTIONS_H);
    lv_obj_set_pos(s_actions, LINK_USABLE_X, ACTIONS_Y);
    lv_obj_set_style_bg_opa(s_actions, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_actions, LV_OBJ_FLAG_GESTURE_BUBBLE);

    fill_action_bar();
}

/* ------------------------------------------------------------------ */

lv_obj_t *ui_screen_capture_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_add_style(s_screen, &link_style_screen, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* A theme change rebuilds the screen without clearing the statics, so
     * the handles have to be given up before they are filled again. */
    s_config_lbl = NULL;
    for (int i = 0; i < VIEW_COUNT; i++) {
        s_views[i] = NULL;
    }

    /* Take the field as it stands, so the first refresh does not read it
     * as having just moved and clear a selection nobody touched. */
    s_filter_id_offset = s_id_offset;
    s_filter_id_len = s_id_len;

    ui_helpers_create_status_bar(s_screen, tr(VIEW_TITLES[VIEW_JOURNAL]),
                                 on_menu_tapped, NULL, &s_status);

    /* The pips go in the header - one place for "which page am I on"
     * across the product, and the band beside the clock is empty on
     * every screen that has one.
     *
     * Markers only, so a page is reached by swiping. The swipe was already
     * the gesture this screen was built around. */
    if (VIEW_COUNT > 1) {
        ui_helpers_status_bar_add_dots(&s_status, VIEW_COUNT, 0);
    }

    /* Clips the pages as they slide, so neither shows outside the band
     * between the header and the bar. */
    lv_obj_t *container = lv_obj_create(s_screen);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LINK_USABLE_W, VIEW_CONTENT_H);
    lv_obj_set_pos(container, LINK_USABLE_X, LINK_USABLE_Y + LINK_HEADER_H);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_views[VIEW_JOURNAL] = build_view_journal(container);
    s_views[VIEW_STATS] = ui_view_stats_create(container);

    /* Parked to the right and hidden; the journal is the one on screen. */
    for (int i = 1; i < VIEW_COUNT; i++) {
        lv_obj_set_pos(s_views[i], LINK_USABLE_W, 0);
        lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Built after the pages, so its dropdown list unrolls over them. */
    build_action_bar(s_screen);

    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    /* The event is delivered to the first ancestor that does not bubble,
     * so every object between a finger and the screen has to carry the
     * flag - a swipe started on a label would otherwise die on it. */
    bubble_gestures(s_screen);

    /* A theme rebuild can delete the screen mid-slide: lv_obj_delete drops
     * the animations without running the completed callback, which would
     * leave s_anim_running set and the swipe dead for good. The rows are
     * new objects too, so they need repainting. */
    s_anim_running = false;
    s_active_view = VIEW_JOURNAL;
    s_hist_dirty = true;

    /* The panel is not on the back stack, so nothing else would clear it:
     * a rebuild deletes the objects under it and would leave the statics
     * pointing at freed memory. */
    s_panel = NULL;
    s_sheet_closed = NULL;

#if CAPTURE_WORST_CASE
    s_hist_count = 0;
    fill_worst_case();
#endif

    ESP_LOGD(TAG, "built");

    return s_screen;
}

/* Delta to the row above, sent or received. */
static void push_history(const link_radio_frame_t *f, hist_kind_t kind)
{
    const int64_t delta = s_hist_count > 0 ? f->ts_ms - s_hist[0].ts_ms : -1;

    if (s_hist_count < HIST_ROWS) {
        s_hist_count++;
    }

    /* Newest first: shift down, then write the top. */
    for (int i = s_hist_count - 1; i > 0; i--) {
        s_hist[i] = s_hist[i - 1];
        s_hist_kind[i] = s_hist_kind[i - 1];
    }
    s_hist[0] = *f;
    s_hist[0].delta_ms = delta;
    s_hist_kind[0] = kind;
}

#if CAPTURE_WORST_CASE
/*
 * Every kind of row, every delta format, the widest values. 0xDD because
 * D is the widest hex digit in the body font - 0xFF is one of the
 * narrowest and hid the overflow. The SNRs span the three margin colours.
 */
static void fill_worst_case(void)
{
    static const struct {
        hist_kind_t kind;
        int64_t delta_ms;
        int16_t snr_qdb;
        uint8_t len;
    } ROWS[] = {
        {HIST_RX, 999, -80, 255},
        {HIST_TX, 9900, 0, 255},
        {HIST_CRC, 59000, -80, 255},
        {HIST_LOST, 3599000, 0, 0},
        {HIST_HDR_ERR, 999LL * 3600000, 0, 0},
        {HIST_RX, 999, 48, 0},
        {HIST_RX, 59000, 0, 255},
    };
    const int n = (int)(sizeof(ROWS) / sizeof(ROWS[0]));

    link_radio_frame_t f = {0};
    link_profile_cfg_t profile;
    link_profile_get_current(&profile);
    f.radio = profile.radio;
    memset(f.payload, 0xDD, sizeof(f.payload));
    f.rssi_dbm = -120;
    f.ts_ms = esp_timer_get_time() / 1000;
    f.epoch_ms = 4102444799000LL; /* 31/12/2099 23:59:59 */

    for (int i = 0; i < HIST_ROWS; i++) {
        const int k = i % n;

        f.len = ROWS[k].len;
        f.kept = ROWS[k].len > 0 ? 8 : 0;
        f.snr_qdb = ROWS[k].snr_qdb;
        push_history(&f, ROWS[k].kind);

        /* The oldest row keeps no delta, as the first frame does. */
        s_hist[0].delta_ms = i == 0 ? -1 : ROWS[k].delta_ms;
    }

    s_hist_dirty = true;
}
#endif

/*
 * Put the test frame on air. No row is added here: the journal takes
 * sent frames from the radio once they are out, whoever asked for them.
 * The payload stands in for what a protocol would compose.
 */
static void capture_send(void)
{
    static const uint8_t TEST_PAYLOAD[] = {0xA5, 0x5A, 0x00, 0x01,
                                           0x02, 0x03, 0x04, 0x05};

    const esp_err_t err =
        link_radio_tx_submit(TEST_PAYLOAD, sizeof(TEST_PAYLOAD));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tx refused: %s", esp_err_to_name(err));
    }
}

/* A frame of ours that went out, as a journal row. Failed ones stay in
 * the events view. */
static bool push_sent(const link_radio_tx_t *tx)
{
    if (tx->result != LINK_RADIO_TX_OK) {
        return false;
    }

    link_radio_frame_t f = {0};

    f.epoch_ms = tx->epoch_ms;
    f.ts_ms = tx->ts_ms;
    f.len = tx->len;
    f.kept = tx->len;
    memcpy(f.payload, tx->payload, tx->len);
    f.toa_us = tx->toa_us;
    f.radio = tx->radio;

    push_history(&f, HIST_TX);
    s_hist_dirty = true;
    return true;
}

static void log_row_set_hidden(const log_row_t *row, bool hidden)
{
    /* The target goes with them: an unfilled row must not be tappable, and
     * hiding an object stops it taking touches. */
    lv_obj_t *const parts[] = {row->target,  row->lbl_time, row->lbl_payload,
                               row->lbl_oct, row->lbl_rssi, row->lbl_snr,
                               row->chevron};

    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (hidden) {
            lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (row->rule != NULL) {
        if (hidden) {
            lv_obj_add_flag(row->rule, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(row->rule, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Only ever hidden here: showing it is the painter's call, which
     * knows the direction. */
    if (hidden) {
        lv_obj_add_flag(row->mark, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_log(void)
{
    /* hh:mm:ss, not the compact delta: "+1min" would look frozen. */
    if (s_hist_count > 0) {
        uint32_t s = (uint32_t)((esp_timer_get_time() / 1000
                                 - s_hist[0].ts_ms) / 1000);
        if (s > 359999) {
            s = 359999;
        }
        lv_label_set_text_fmt(s_log_pending, "(%02u:%02u:%02u)",
                              (unsigned)(s / 3600), (unsigned)((s / 60) % 60),
                              (unsigned)(s % 60));
        lv_obj_remove_flag(s_log_pending, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_log_pending, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_hist_dirty) {
        return;
    }
    s_hist_dirty = false;

    if (s_hist_count == 0) {
        /* Unfilled rows are hidden, not left blank: an empty label still
         * counts towards the scrollable height, which is what puts a
         * scrollbar next to a list holding one frame. */
        for (int i = 0; i < HIST_ROWS; i++) {
            log_row_set_hidden(&s_log_rows[i], true);
        }
        s_log_shown = 0;
        lv_obj_invalidate(s_log_scroll);
        return;
    }

    char stamp[24];
    char delta[12];
    char line[40];

    /* Rows are packed from the top, so a selection leaves no gaps in the
     * list - only fewer entries. */
    int row = 0;

    for (int i = 0; i < s_hist_count; i++) {
        const hist_kind_t kind = s_hist_kind[i];
        const bool unmeasured = kind == HIST_LOST || kind == HIST_HDR_ERR;
        const bool failed = kind == HIST_CRC || unmeasured;

        if (s_filter_id[0] != '\0' && failed) {
            continue;
        }
        if (!filter_accepts(&s_hist[i])) {
            continue;
        }

        s_log_row_frame[row] = i;
        log_row_set_hidden(&s_log_rows[row], false);

        /*
         * Time and delta share one field: they are one reading - when it
         * arrived, and how long after the frame before it.
         *
         * The delta is the gap to the previous frame, sent or received, not
         * to the row above: under a selection that row can be much older.
         * What it measures is the traffic, which is what it is for.
         */
        format_clock(s_hist[i].epoch_ms, stamp, sizeof(stamp));
        format_delta(s_hist[i].delta_ms, delta, sizeof(delta));

        if (delta[0] != '\0') {
            lv_snprintf(line, sizeof(line), "%s %s", stamp, delta);
        } else {
            lv_snprintf(line, sizeof(line), "%s", stamp);
        }
        lv_label_set_text(s_log_rows[row].lbl_time, line);

        lv_obj_set_style_bg_color(s_log_rows[row].mark,
                                  kind == HIST_TX ? LINK_COLOR_ACCENT_FG
                                                  : LINK_COLOR_TEXT_DIM,
                                  0);
        lv_obj_remove_flag(s_log_rows[row].mark, LV_OBJ_FLAG_HIDDEN);

        if (failed) {
            lv_label_set_text(s_log_rows[row].lbl_payload,
                              tr(kind == HIST_CRC    ? T_CRC_ERROR
                                 : kind == HIST_LOST ? T_FRAME_INCOMPLETE
                                                     : T_HEADER_INVALID));
            lv_obj_set_style_text_color(s_log_rows[row].lbl_payload,
                                        LINK_COLOR_ERROR, 0);
        } else {
            char preview[3 * LOG_PREVIEW_BYTES + 4];
            format_payload(&s_hist[i], LOG_PREVIEW_BYTES, preview,
                           sizeof(preview));
            lv_label_set_text(s_log_rows[row].lbl_payload, preview);
            lv_obj_set_style_text_color(s_log_rows[row].lbl_payload,
                                        LINK_COLOR_TEXT_DIM, 0);
        }

        /* The three columns. Bare numbers under their headings - the units
         * are named up there and written out in the detail sheet - and
         * coloured, which is the fastest read on this screen. */
        if (unmeasured) {
            lv_label_set_text(s_log_rows[row].lbl_oct, "--");
        } else {
            lv_label_set_text_fmt(s_log_rows[row].lbl_oct, "%u",
                                  (unsigned)s_hist[i].len);
        }

        if (kind == HIST_TX || unmeasured) {
            lv_label_set_text(s_log_rows[row].lbl_rssi, "--");
            lv_obj_set_style_text_color(s_log_rows[row].lbl_rssi,
                                        LINK_COLOR_TEXT_DIM, 0);
            lv_label_set_text(s_log_rows[row].lbl_snr, "--");
            lv_obj_set_style_text_color(s_log_rows[row].lbl_snr,
                                        LINK_COLOR_TEXT_DIM, 0);
        } else {
            lv_label_set_text_fmt(s_log_rows[row].lbl_rssi, "%d",
                                  (int)s_hist[i].rssi_dbm);
            lv_obj_set_style_text_color(s_log_rows[row].lbl_rssi,
                                        LINK_COLOR_TEXT, 0);

            lv_label_set_text_fmt(s_log_rows[row].lbl_snr, "%d",
                                  snr_db_rounded(&s_hist[i]));
            lv_obj_set_style_text_color(s_log_rows[row].lbl_snr,
                                        margin_color(
                                            link_radio_margin_tenths(&s_hist[i])),
                                        0);
        }

        row++;
    }

    s_log_shown = row;

    /* A selection can leave rows that the loop did not reach: they held
     * frames it rejects, and are hidden rather than left showing what they
     * last displayed. */
    for (int i = row; i < HIST_ROWS; i++) {
        log_row_set_hidden(&s_log_rows[i], true);
    }

    /* Under the last row shown, not under the twentieth: with a selection
     * in force the rows stop short, and a note pinned to the full height
     * would float in empty space below them. */
    if (s_hist_count == HIST_ROWS) {
        lv_obj_set_pos(s_log_tail, LINK_PAD_SM,
                       s_log_shown * VC_ROW_H + LINK_PAD_SM);
        lv_obj_remove_flag(s_log_tail, LV_OBJ_FLAG_HIDDEN);
    }

    /* The scrollbar moves and resizes as the content height changes, and
     * the labels' own invalidations do not cover where it used to be -
     * without this, stale fragments of it stay on the glass. */
    lv_obj_invalidate(s_log_scroll);
}

void ui_screen_capture_refresh(void)
{
    ui_helpers_update_status_bar(&s_status);

    /*
     * The configure sheet reads what is in force, and what is in force can
     * change while it is up: its profile row opens the profile screen, and
     * coming back from there lands on a sheet built before the change.
     *
     * Refreshing on the tick rather than on the way back, because the sheet
     * is not rebuilt by that return - there is no event to hang it on, and
     * a stale row is wrong for as long as it is looked at. The rows are
     * three labels; rewriting them a few times a second costs nothing.
     */
    sheet_refresh_rows();

    /*
     * A selection is a literal id, so moving the field leaves it matching
     * bytes that no longer mean anything - and, since nothing would match,
     * an empty list with no way back but reopening the selector. Dropping
     * it is the honest answer to the field having changed under it.
     */
    if (s_id_offset != s_filter_id_offset || s_id_len != s_filter_id_len) {
        s_filter_id_offset = s_id_offset;
        s_filter_id_len = s_id_len;
        filter_set(NULL);
    }

    /* One view, one recording session: whatever closed the last one -
     * this screen, Config, the desktop app - the counts restart. */
    link_rec_stats_t rec;
    link_rec_get_stats(&rec);
    if (rec.open && rec.session != s_session) {
        s_session = rec.session;
        clear_view();
    }

    link_profile_cfg_t profile;
    link_profile_get_current(&profile);

    char summary[72];
    link_profile_format_summary(link_profile_current(), summary,
                                sizeof(summary));
    const size_t used = strlen(summary);
    lv_snprintf(summary + used, sizeof(summary) - used, "\n%s 0x%02X", tr(T_SYNC_SHORT),
                (unsigned)profile.radio.sync_word);
    if (strcmp(lv_label_get_text(s_log_empty_cfg), summary) != 0) {
        lv_label_set_text(s_log_empty_cfg, summary);
    }

    /* Static: about 4 kB, and the view refreshed below takes a copy of
     * its own on the same LVGL stack. Only ever touched from that task. */
    static link_radio_state_t radio;
    link_radio_get(&radio);

    /*
     * Inserting a frame follows the scroll position:
     *
     *   at rest, list at the top    the new frame appears at the head;
     *   at rest, scrolled down      insert, then push the position down by
     *                               one row so what is being read stays
     *                               where the eye left it;
     *   gesture or inertia running  the list is untouchable - the content
     *                               would jump under the finger and the
     *                               scroll_to would cut the inertia short.
     *                               The frame waits: the counter is not
     *                               consumed, so the first refresh after
     *                               the gesture takes it.
     */
    bool scrolling = lv_anim_get(s_log_scroll, NULL) != NULL;
    for (lv_indev_t *indev = lv_indev_get_next(NULL);
         indev != NULL && !scrolling; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_scroll_obj(indev) == s_log_scroll) {
            scrolling = true;
        }
    }

    bool pushed = false;

    /*
     * From the radio's log, failures included, oldest first. Nothing the
     * clock could not stamp.
     */
    if (radio.entry_seq != s_last_entry_seq && !scrolling) {
        uint32_t fresh = radio.entry_seq - s_last_entry_seq;
        if (fresh > radio.entries) {
            fresh = radio.entries;
        }
        s_last_entry_seq = radio.entry_seq;

        for (int k = (int)fresh - 1; k >= 0; k--) {
            const link_radio_entry_t *e = &radio.entry[k];

            if (e->frame.epoch_ms == 0) {
                continue;
            }

            push_history(&e->frame,
                         e->kind == LINK_RADIO_ENTRY_FRAME  ? HIST_RX
                         : e->kind == LINK_RADIO_ENTRY_CRC  ? HIST_CRC
                         : e->kind == LINK_RADIO_ENTRY_LOST ? HIST_LOST
                                                            : HIST_HDR_ERR);
            pushed = true;
        }
        s_hist_dirty = true;
    }

    /* Ours, whoever asked for it - this screen or the protocol. */
    if (radio.tx.seq != s_last_tx_seq && !scrolling) {
        s_last_tx_seq = radio.tx.seq;
        pushed = push_sent(&radio.tx) || pushed;
    }

    if (pushed) {
        lv_obj_add_flag(s_log_empty, LV_OBJ_FLAG_HIDDEN);

        const int32_t sy = lv_obj_get_scroll_y(s_log_scroll);
        if (sy > 0) {
            lv_obj_scroll_to_y(s_log_scroll, sy + VC_ROW_H, LV_ANIM_OFF);
        }
    }

    /*
     * Only the page in front is written: no point rewriting rows behind one
     * nobody is looking at. The panel does not count as in front - it
     * leaves the top of the list showing, and watching it go on filling
     * under the sheet is the point of a sheet.
     */
    switch (s_active_view) {
    case VIEW_JOURNAL:
        refresh_log();
        break;
    case VIEW_STATS:
        ui_view_stats_refresh();
        break;
    default:
        break;
    }
}
