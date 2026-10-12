#include "ui_param_edit.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "link_profile.h"
#include "link_rec.h"
#include "ui_helpers.h"
#include "link_theme.h"
#include "ui_text.h"

static const char *TAG = "ui_param";

/* How a parameter is entered. The editor decides this; the screens do
 * not know and must not care. */
typedef enum {
    EDIT_LIST,    /* a handful of legal values */
    EDIT_NUMERIC, /* +/- over a range */
    EDIT_KEYPAD,  /* typed in, for values too fine to step through */
    EDIT_KEYPAD_HEX, /* typed in, hex, for a value that is a bit pattern */
    EDIT_KEYPAD_INT, /* typed in, a whole number over a wide range */
} edit_kind_t;

static lv_obj_t *s_overlay;
static ui_param_t s_param;

/* Edited in place, pushed as a whole on validate. Nothing reaches the
 * radio before that. */
static link_profile_cfg_t s_local;
static bool s_pending_commit;

static lv_obj_t *s_value_label;

/*
 * Frequencies are held in kHz throughout the editor. The chip tunes far
 * finer than 100 kHz, so MHz alone cannot express what the user needs
 * to type - and %f is not available here (see sdkconfig.defaults),
 * which rules out the obvious float.
 */
#define FREQ_MIN_KHZ (SX1262_FREQ_MIN_HZ / 1000u)
#define FREQ_MAX_KHZ (SX1262_FREQ_MAX_HZ / 1000u)

static const uint8_t SF_VALUES[] = {5, 6, 7, 8, 9, 10, 11, 12};
static const char *SF_LABELS[] = {"SF5", "SF6",  "SF7",  "SF8",
                                  "SF9", "SF10", "SF11", "SF12"};

/* The bandwidths in use; the narrower steps the chip has are left out. */
static const uint32_t BW_VALUES[] = {62500, 125000, 250000, 500000};
static const char *BW_LABELS[] = {"62.5 kHz", "125 kHz", "250 kHz", "500 kHz"};

static const uint8_t CR_VALUES[] = {5, 6, 7, 8};
static const char *CR_LABELS[] = {"4/5", "4/6", "4/7", "4/8"};

/* In link_region_t order. */
static const char *REGION_LABELS[LINK_REGION_COUNT] = {"EU868", "US915"};

/* Index 1 is the flag set. Filled when a picker opens: the labels
 * follow the language. */
static const char *s_two_labels[2];

static const char *crc_label(bool on)
{
    return tr(on ? T_ON : T_OFF);
}

static const char *iq_label(bool inverted)
{
    return tr(inverted ? T_IQ_INVERTED : T_IQ_STANDARD);
}

static const char *gain_label(bool boosted)
{
    return tr(boosted ? T_GAIN_BOOSTED : T_GAIN_SAVING);
}

#define PREAMBLE_MIN 1
#define PREAMBLE_MAX 65535

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static edit_kind_t param_kind(ui_param_t param)
{
    switch (param) {
    case UI_PARAM_FREQ:
        return EDIT_KEYPAD;
    case UI_PARAM_SYNC:
        return EDIT_KEYPAD_HEX;
    case UI_PARAM_PREAMBLE:
        return EDIT_KEYPAD_INT;
    case UI_PARAM_POWER:
        return EDIT_NUMERIC;
    default:
        return EDIT_LIST;
    }
}

const char *ui_param_label(ui_param_t param)
{
    switch (param) {
    case UI_PARAM_REGION:
        return tr(T_REGION);
    case UI_PARAM_FREQ:
        return tr(T_FREQUENCY);
    case UI_PARAM_SF:
        return tr(T_SPREADING);
    case UI_PARAM_BW:
        return tr(T_BANDWIDTH);
    case UI_PARAM_CR:
        return tr(T_CODING);
    case UI_PARAM_SYNC:
        return tr(T_SYNC_WORD);
    case UI_PARAM_POWER:
        return tr(T_POWER);
    case UI_PARAM_PREAMBLE:
        return tr(T_PREAMBLE);
    case UI_PARAM_CRC:
        return "CRC";
    case UI_PARAM_IQ:
        return "IQ";
    case UI_PARAM_RX_GAIN:
        return tr(T_RX_GAIN);
    default:
        return "?";
    }
}

static int bw_index(uint32_t bw_hz)
{
    for (int i = 0; i < COUNT_OF(BW_VALUES); i++) {
        if (BW_VALUES[i] == bw_hz) {
            return i;
        }
    }
    return -1;
}

/* Trailing zeros go, but the decimal point stays only if something
 * follows it: "868.3", "868", never "868.". */
static void format_freq(uint32_t khz, char *buf, unsigned size)
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

static void format_value(ui_param_t param, const link_profile_cfg_t *p,
                         char *buf, unsigned size)
{
    const sx1262_radio_cfg_t *cfg = &p->radio;

    switch (param) {
    case UI_PARAM_REGION:
        lv_snprintf(buf, size, "%s", link_regul_region_name(p->region));
        break;
    case UI_PARAM_FREQ:
        format_freq(cfg->freq_hz / 1000, buf, size);
        break;
    case UI_PARAM_SF:
        lv_snprintf(buf, size, "SF%u", (unsigned)cfg->sf);
        break;
    case UI_PARAM_BW: {
        /* Not from the list: a profile set from the app may hold a
         * step the list leaves out. */
        char bw[12];
        link_profile_format_bw(cfg->bw_hz, bw, sizeof(bw));
        lv_snprintf(buf, size, "%s kHz", bw);
        break;
    }
    case UI_PARAM_CR:
        lv_snprintf(buf, size, "4/%u", (unsigned)cfg->cr);
        break;
    case UI_PARAM_SYNC:
        lv_snprintf(buf, size, "0x%02X", (unsigned)cfg->sync_word);
        break;
    case UI_PARAM_POWER:
        lv_snprintf(buf, size, "%d dBm", (int)cfg->power_dbm);
        break;
    case UI_PARAM_PREAMBLE:
        lv_snprintf(buf, size, "%u %s", (unsigned)cfg->preamble_len,
                    tr(T_SYMBOLS_SHORT));
        break;
    case UI_PARAM_CRC:
        lv_snprintf(buf, size, "%s", crc_label(cfg->crc_on));
        break;
    case UI_PARAM_IQ:
        lv_snprintf(buf, size, "%s", iq_label(cfg->iq_inverted));
        break;
    case UI_PARAM_RX_GAIN:
        lv_snprintf(buf, size, "%s", gain_label(p->rx_boosted));
        break;
    default:
        lv_snprintf(buf, size, "?");
        break;
    }
}

void ui_param_format(ui_param_t param, char *buf, unsigned size)
{
    link_profile_cfg_t cfg;
    link_profile_get_current(&cfg);
    format_value(param, &cfg, buf, size);
}

/* Deferred: the overlay is deleted from inside one of its own events,
 * and applying the settings can take a moment. */
static void close_cb(void *arg)
{
    (void)arg;

    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_value_label = NULL;
    }

    if (!s_pending_commit) {
        return;
    }
    s_pending_commit = false;

    const esp_err_t err = link_profile_update_current(&s_local);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "no radio to configure");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings refused: %s", esp_err_to_name(err));
    }

    /* Nothing to notify: the config screen re-reads the profile on its
     * next tick, so a refused change simply never appears. */
}

static void on_back(void *user_data)
{
    (void)user_data;
    s_pending_commit = false;
    lv_async_call(close_cb, NULL);
}

static void on_validate(void *user_data)
{
    (void)user_data;
    link_rec_close("config");
    s_pending_commit = true;
    lv_async_call(close_cb, NULL);
}

/*
 * List: the editor's own overlay is dropped and the shared picker takes
 * over, header and all. Its commit is the validate.
 */
static void list_commit(int index)
{
    sx1262_radio_cfg_t *radio = &s_local.radio;

    switch (s_param) {
    case UI_PARAM_REGION:
        s_local.region = (link_region_t)index;
        break;
    case UI_PARAM_SF:
        radio->sf = SF_VALUES[index];
        break;
    case UI_PARAM_BW:
        radio->bw_hz = BW_VALUES[index];
        break;
    case UI_PARAM_CR:
        radio->cr = CR_VALUES[index];
        break;
    case UI_PARAM_CRC:
        radio->crc_on = index == 1;
        break;
    case UI_PARAM_IQ:
        radio->iq_inverted = index == 1;
        break;
    case UI_PARAM_RX_GAIN:
        s_local.rx_boosted = index == 1;
        break;
    default:
        return;
    }

    link_rec_close("config");
    s_pending_commit = true;
    lv_async_call(close_cb, NULL);
}

static int index_of_u8(const uint8_t *values, int count, uint8_t v)
{
    for (int i = 0; i < count; i++) {
        if (values[i] == v) {
            return i;
        }
    }
    return 0;
}

static void open_list_editor(void)
{
    const sx1262_radio_cfg_t *radio = &s_local.radio;
    const char **labels;
    int count = 2;
    int initial = 0;

    switch (s_param) {
    case UI_PARAM_REGION:
        labels = REGION_LABELS;
        count = LINK_REGION_COUNT;
        initial = (int)s_local.region;
        break;
    case UI_PARAM_SF:
        labels = SF_LABELS;
        count = COUNT_OF(SF_VALUES);
        initial = index_of_u8(SF_VALUES, count, radio->sf);
        break;
    case UI_PARAM_BW:
        labels = BW_LABELS;
        count = COUNT_OF(BW_VALUES);
        initial = bw_index(radio->bw_hz);
        if (initial < 0) {
            initial = 0;
        }
        break;
    case UI_PARAM_CR:
        labels = CR_LABELS;
        count = COUNT_OF(CR_VALUES);
        initial = index_of_u8(CR_VALUES, count, radio->cr);
        break;
    case UI_PARAM_CRC:
        s_two_labels[0] = crc_label(false);
        s_two_labels[1] = crc_label(true);
        labels = s_two_labels;
        initial = radio->crc_on ? 1 : 0;
        break;
    case UI_PARAM_IQ:
        s_two_labels[0] = iq_label(false);
        s_two_labels[1] = iq_label(true);
        labels = s_two_labels;
        initial = radio->iq_inverted ? 1 : 0;
        break;
    default:
        s_two_labels[0] = gain_label(false);
        s_two_labels[1] = gain_label(true);
        labels = s_two_labels;
        initial = s_local.rx_boosted ? 1 : 0;
        break;
    }

    const ui_list_picker_cfg_t cfg = {
        .title = ui_param_label(s_param),
        .labels = labels,
        .count = count,
        .initial = initial,
        .on_commit = list_commit,
    };
    ui_list_picker_open(&cfg);
}

/*
 * Numeric: value in the middle, one big button either side.
 */
#define BIG_BTN_W 70
#define BIG_BTN_H 100

static void numeric_refresh(void)
{
    char buf[24];
    format_value(s_param, &s_local, buf, sizeof(buf));
    lv_label_set_text(s_value_label, buf);
}

static void numeric_step_cb(lv_event_t *e)
{
    const int sign = (int)(intptr_t)lv_event_get_user_data(e);

    switch (s_param) {
    case UI_PARAM_POWER: {
        int v = s_local.radio.power_dbm + sign;

        if (v < -9) {
            v = -9;
        }
        if (v > 22) {
            v = 22;
        }
        s_local.radio.power_dbm = (int8_t)v;
        break;
    }
    default:
        break;
    }

    numeric_refresh();
}

/* Plain LVGL buttons: these repeat on a long press, which the tap
 * guards in ui_touch would reject. */
static void numeric_button(lv_obj_t *parent, const char *text, int32_t x,
                           int sign)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &link_style_btn_big, 0);
    lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, BIG_BTN_W, BIG_BTN_H);
    lv_obj_set_pos(btn, x, LINK_CONTENT_Y + (LINK_CONTENT_H - BIG_BTN_H) / 2);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(btn, numeric_step_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)sign);
    lv_obj_add_event_cb(btn, numeric_step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)sign);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, LINK_FONT_HUGE, 0);
    lv_obj_center(lbl);
}

static void build_numeric_editor(lv_obj_t *parent)
{
    s_value_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_value_label, LINK_FONT_HUGE, 0);
    lv_obj_set_style_text_color(s_value_label, LINK_COLOR_TEXT, 0);
    /* Above centre: the header takes the top of the screen, so dead
     * centre reads as low. */
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -LINK_PAD_LG);

    numeric_button(parent, "-", LINK_USABLE_X + LINK_PAD_XL, -1);
    numeric_button(parent, "+",
                   LINK_USABLE_X + LINK_USABLE_W - BIG_BTN_W - LINK_PAD_XL, +1);

    numeric_refresh();
}

/*
 * Keypad: a frequency is typed, not stepped. Crossing the band 100 kHz
 * at a time would be thousands of taps.
 */
#define KP_ROWS     4
#define KP_COLS_MAX 5
#define KP_GAP   LINK_PAD_XS
#define KP_KEY_H 34

#define KEYPAD_BUF_SIZE 12
#define KEYPAD_DEBOUNCE_MS 200

static char s_keypad[KEYPAD_BUF_SIZE];
static int s_keypad_len;
static uint32_t s_keypad_last_ms;

/* Which keypad is up. Everything below reads this rather than the
 * parameter: the keypad is the same widget either way. */
static bool s_keypad_hex;
static bool s_keypad_int;
static int s_keypad_cols;
static int s_keypad_max_len;

static const char *KP_LABELS_DEC[KP_ROWS][KP_COLS_MAX] = {
    {"1", "2", "3"},
    {"4", "5", "6"},
    {"7", "8", "9"},
    {".", "0", LV_SYMBOL_BACKSPACE},
};

static const char *KP_LABELS_INT[KP_ROWS][KP_COLS_MAX] = {
    {"1", "2", "3"},
    {"4", "5", "6"},
    {"7", "8", "9"},
    {NULL, "0", LV_SYMBOL_BACKSPACE},
};

/* 16 digits and a backspace do not divide into a grid: the holes in the
 * last row are deliberate, and NULL means no key there. */
static const char *KP_LABELS_HEX[KP_ROWS][KP_COLS_MAX] = {
    {"1", "2", "3", "A", "B"},
    {"4", "5", "6", "C", "D"},
    {"7", "8", "9", "E", "F"},
    {"0", NULL, NULL, NULL, LV_SYMBOL_BACKSPACE},
};

static const char *keypad_label(int r, int c)
{
    if (s_keypad_hex) {
        return KP_LABELS_HEX[r][c];
    }
    return s_keypad_int ? KP_LABELS_INT[r][c] : KP_LABELS_DEC[r][c];
}

static void keypad_refresh(void)
{
    if (s_keypad_hex) {
        lv_label_set_text_fmt(s_value_label, "0x%s",
                              s_keypad_len == 0 ? "_" : s_keypad);
        return;
    }

    if (s_keypad_int) {
        lv_label_set_text_fmt(s_value_label, "%s %s",
                              s_keypad_len == 0 ? "_" : s_keypad,
                              tr(T_SYMBOLS_SHORT));
        return;
    }

    if (s_keypad_len == 0) {
        lv_label_set_text(s_value_label, "_ MHz");
    } else {
        lv_label_set_text_fmt(s_value_label, "%s MHz", s_keypad);
    }
}

/* "868.3" -> 868300 kHz. Parsed by hand: the C library's float
 * conversion is what the LVGL task has no stack for. */
static uint32_t keypad_to_khz(void)
{
    uint32_t mhz = 0;
    int i = 0;

    for (; i < s_keypad_len && s_keypad[i] != '.'; i++) {
        mhz = mhz * 10 + (uint32_t)(s_keypad[i] - '0');
    }

    /* Three digits after the point, zero-padded: ".3" is 300 kHz, not 3. */
    uint32_t frac = 0;
    uint32_t scale = 100;

    if (i < s_keypad_len && s_keypad[i] == '.') {
        for (i++; i < s_keypad_len && scale > 0; i++) {
            frac += (uint32_t)(s_keypad[i] - '0') * scale;
            scale /= 10;
        }
    }

    return mhz * 1000 + frac;
}

static void keypad_commit(void)
{
    if (s_keypad_len == 0) {
        return;
    }

    if (s_keypad_hex) {
        uint32_t v = 0;
        for (int i = 0; i < s_keypad_len; i++) {
            const char c = s_keypad[i];
            const uint32_t d = (c <= '9') ? (uint32_t)(c - '0')
                                          : (uint32_t)(c - 'A' + 10);
            v = (v << 4) | d;
        }
        s_local.radio.sync_word = (uint8_t)v;
        return;
    }

    if (s_keypad_int) {
        uint32_t v = 0;
        for (int i = 0; i < s_keypad_len; i++) {
            v = v * 10 + (uint32_t)(s_keypad[i] - '0');
        }
        if (v < PREAMBLE_MIN) {
            v = PREAMBLE_MIN;
        }
        if (v > PREAMBLE_MAX) {
            v = PREAMBLE_MAX;
        }
        s_local.radio.preamble_len = (uint16_t)v;
        return;
    }

    uint32_t khz = keypad_to_khz();

    if (khz < FREQ_MIN_KHZ) {
        khz = FREQ_MIN_KHZ;
    }
    if (khz > FREQ_MAX_KHZ) {
        khz = FREQ_MAX_KHZ;
    }

    s_local.radio.freq_hz = khz * 1000;
}

static void on_keypad_key(lv_event_t *e)
{
    /* Contact bounce on this panel double-fires otherwise. */
    const uint32_t now = lv_tick_get();
    if (now - s_keypad_last_ms < KEYPAD_DEBOUNCE_MS) {
        return;
    }
    s_keypad_last_ms = now;

    const char *key = lv_event_get_user_data(e);

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        if (s_keypad_len > 0) {
            s_keypad[--s_keypad_len] = '\0';
        }
    } else if (!s_keypad_hex && strcmp(key, ".") == 0) {
        /* One point, and not as the first character. */
        if (s_keypad_len > 0 && s_keypad_len < KEYPAD_BUF_SIZE - 2
            && strchr(s_keypad, '.') == NULL) {
            s_keypad[s_keypad_len++] = '.';
            s_keypad[s_keypad_len] = '\0';
        }
    } else if (s_keypad_len < s_keypad_max_len) {
        s_keypad[s_keypad_len++] = key[0];
        s_keypad[s_keypad_len] = '\0';
    }

    keypad_refresh();
}

static void on_validate_keypad(void *user_data)
{
    keypad_commit();
    on_validate(user_data);
}

static void keypad_load_current(void)
{
    if (s_keypad_hex) {
        s_keypad_len = lv_snprintf(s_keypad, sizeof(s_keypad), "%02X",
                                   (unsigned)s_local.radio.sync_word);
        return;
    }

    if (s_keypad_int) {
        s_keypad_len = lv_snprintf(s_keypad, sizeof(s_keypad), "%u",
                                   (unsigned)s_local.radio.preamble_len);
        return;
    }

    const uint32_t khz = s_local.radio.freq_hz / 1000;
    unsigned frac = khz % 1000;

    if (frac == 0) {
        s_keypad_len = lv_snprintf(s_keypad, sizeof(s_keypad), "%lu",
                                   (unsigned long)(khz / 1000));
        return;
    }

    while (frac % 10 == 0) {
        frac /= 10;
    }

    s_keypad_len = lv_snprintf(s_keypad, sizeof(s_keypad), "%lu.%u",
                               (unsigned long)(khz / 1000), frac);
}

static void build_keypad_editor(lv_obj_t *parent)
{
    s_keypad_cols = s_keypad_hex ? 5 : 3;
    s_keypad_max_len = s_keypad_hex ? 2 : s_keypad_int ? 5 : KEYPAD_BUF_SIZE - 2;

    /* Grid against the bottom of the usable area, value centred in
     * whatever is left between it and the header. */
    const int32_t key_w =
        (LINK_USABLE_W - (s_keypad_cols - 1) * KP_GAP) / s_keypad_cols;
    const int32_t grid_h = KP_ROWS * KP_KEY_H + (KP_ROWS - 1) * KP_GAP;
    const int32_t grid_y = LINK_USABLE_Y + LINK_USABLE_H - grid_h;

    s_value_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_value_label, LINK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_value_label, LINK_COLOR_TEXT, 0);
    lv_obj_set_width(s_value_label, LINK_SCREEN_W);
    lv_obj_set_style_text_align(s_value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_value_label, 0,
                   LINK_CONTENT_Y
                       + (grid_y - LINK_CONTENT_Y - LINK_FONT_TITLE->line_height)
                             / 2);

    for (int r = 0; r < KP_ROWS; r++) {
        for (int c = 0; c < s_keypad_cols; c++) {
            if (keypad_label(r, c) == NULL) {
                continue;
            }

            lv_obj_t *btn = lv_button_create(parent);
            lv_obj_remove_style_all(btn);
            lv_obj_add_style(btn, &link_style_btn, 0);
            lv_obj_add_style(btn, &link_style_btn_pressed, LV_STATE_PRESSED);
            lv_obj_set_size(btn, key_w, KP_KEY_H);
            lv_obj_set_pos(btn, LINK_USABLE_X + c * (key_w + KP_GAP),
                           grid_y + r * (KP_KEY_H + KP_GAP));
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(btn, on_keypad_key, LV_EVENT_CLICKED,
                                (void *)keypad_label(r, c));

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, keypad_label(r, c));
            lv_obj_set_style_text_font(lbl, LINK_FONT_TITLE, 0);
            lv_obj_center(lbl);
        }
    }

    keypad_load_current();
    s_keypad_last_ms = 0;
    keypad_refresh();
}

bool ui_param_edit_is_open(void)
{
    return s_overlay != NULL || ui_list_picker_is_open();
}

void ui_param_edit_open(ui_param_t param)
{
    if (ui_param_edit_is_open() || param >= UI_PARAM_COUNT) {
        return;
    }

    s_param = param;
    s_pending_commit = false;
    link_profile_get_current(&s_local);

    const edit_kind_t kind = param_kind(param);
    const bool keypad = kind == EDIT_KEYPAD || kind == EDIT_KEYPAD_HEX
                        || kind == EDIT_KEYPAD_INT;

    /* The list editor is the picker, which brings its own overlay and
     * header - building ours first would only stack two of them. */
    if (kind == EDIT_LIST) {
        open_list_editor();
        return;
    }

    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LINK_SCREEN_W, LINK_SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, LINK_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui_helpers_attach_overlay_header(
        s_overlay, ui_param_label(param), on_back, NULL,
        keypad ? on_validate_keypad : on_validate, NULL);

    if (keypad) {
        s_keypad_hex = (kind == EDIT_KEYPAD_HEX);
        s_keypad_int = (kind == EDIT_KEYPAD_INT);
        build_keypad_editor(s_overlay);
    } else {
        build_numeric_editor(s_overlay);
    }

    ESP_LOGD(TAG, "editing %d", (int)param);
}
