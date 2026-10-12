#include "link_rec.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_profile.h"
#include "link_radio.h"
#include "link_regul.h"
#include "link_serial.h"
#include "link_stats.h"
#include "link_time.h"
#include "power_key.h"

static const char *TAG = "link_rec";

#define REC_DIR   "/data/rec"
#define FORMAT_V  1
#define CAP_BYTES (4u * 1024 * 1024)
#define PART_MAX  (128u * 1024)
#define BUF_SIZE  4096
#define FLUSH_MS  2000
#define IDLE_MS   1000
#define QUEUE_LEN 16
#define CLOSE_WAIT_MS 1000
/* Releasing sends what was held, read back from flash. */
#define REQ_WAIT_MS   30000
/* An rx line with 255 bytes of payload, hex, is about 820. */
#define REC_LINE_MAX  1024

/* Radio events and close requests, in the order they happened. */
typedef enum {
    MSG_EVENT,
    MSG_CLOSE,
    /* Opens a session now rather than at the next idle wake. */
    MSG_WAKE,
    MSG_HOLD,
    MSG_RELEASE,
    MSG_ERASE,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    const char *reason;
    link_radio_event_t ev;
} msg_t;

static QueueHandle_t s_queue;
/* Around every file access, for the reader to come. */
static SemaphoreHandle_t s_io_lock;
static SemaphoreHandle_t s_req_lock;
static SemaphoreHandle_t s_req_done;

/* While a session is being read out, live lines wait in the file. */
static bool s_hold;
static link_rec_mark_t s_mark;

static volatile bool s_open;
/* Set by the Journal screen; a session opens only then. */
static volatile bool s_allowed;
static const char *s_open_reason = "boot";
static uint32_t s_next_id;
static uint32_t s_lost;
static uint32_t s_lost_reported;

static uint32_t s_session;
static unsigned s_part;
static int64_t s_session_ts;
static FILE *s_file;
static uint32_t s_file_bytes;
static volatile uint32_t s_total_bytes;
static volatile uint32_t s_lines;

static char s_buf[BUF_SIZE];
static size_t s_buf_len;
static uint32_t s_buf_lines;
static int64_t s_flushed_us;

/* Two lines: opening a part writes the session line while an event line
 * waits for room. */
typedef struct {
    char text[REC_LINE_MAX];
    size_t len;
} line_t;

static line_t s_event;
static line_t s_head;

static void count_lost(uint32_t n)
{
    __atomic_fetch_add(&s_lost, n, __ATOMIC_RELAXED);
}

/* Past the end it only stops writing; add() then drops the line. */
static void put(line_t *l, const char *fmt, ...)
{
    if (l->len >= sizeof(l->text)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(&l->text[l->len], sizeof(l->text) - l->len, fmt,
                            ap);
    va_end(ap);
    if (n > 0) {
        l->len += (size_t)n;
    }
}

static void put_esc(line_t *l, const char *s)
{
    for (; *s != '\0'; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            put(l, "\\%c", c);
        } else if (c < 0x20) {
            put(l, "\\u%04x", c);
        } else {
            put(l, "%c", c);
        }
    }
}

static void put_hex(line_t *l, const char *key, const uint8_t *data,
                    size_t len)
{
    put(l, ",\"%s\":\"", key);
    for (size_t i = 0; i < len; i++) {
        put(l, "%02X", data[i]);
    }
    put(l, "\"");
}

/* Quarter dB, written exactly: -23 is -5.75. */
static void put_qdb(line_t *l, const char *key, int16_t qdb)
{
    const int mag = qdb < 0 ? -qdb : qdb;
    put(l, ",\"%s\":%s%d.%02d", key, qdb < 0 ? "-" : "", mag / 4,
        (mag % 4) * 25);
}

/* Omitted when the clock gave nothing. */
static void put_ms(line_t *l, const char *key, int64_t ms)
{
    if (ms != 0) {
        put(l, ",\"%s\":%lld", key, (long long)ms);
    }
}

static const char *boot_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "reset_pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    default:
        return "other";
    }
}

static void format_session(line_t *l)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);

    l->len = 0;
    put(l, "{\"type\":\"session\",\"v\":%d,\"id\":%lu,\"part\":%u", FORMAT_V,
        (unsigned long)s_session, s_part);
    put_ms(l, "ts", s_session_ts);
    put(l, ",\"open\":\"%s\"", s_open_reason);
    if (strcmp(s_open_reason, "boot") == 0) {
        put(l, ",\"boot\":\"%s\"", boot_reason());
    }
    put(l, ",\"fw\":\"");
    put_esc(l, esp_app_get_description()->version);
    put(l, "\",\"dev\":\"%02X:%02X:%02X:%02X:%02X:%02X\"", mac[0], mac[1],
        mac[2], mac[3], mac[4], mac[5]);
    char profile[LINK_PROFILE_JSON_MAX];
    link_profile_current_json(profile, sizeof(profile));
    put(l, ",\"profile\":%s}", profile);
}

/* Truncated to the ms, as the Journal shows it: one number everywhere. */
static void put_toa(line_t *l, uint32_t toa_us)
{
    if (toa_us != 0) {
        put(l, ",\"toa_ms\":%lu", (unsigned long)(toa_us / 1000));
    }
}

static void format_rx(line_t *l, const link_radio_entry_t *e)
{
    static const char *const STATUS[] = {
        [LINK_RADIO_ENTRY_LOST] = "incomplete",
        [LINK_RADIO_ENTRY_FRAME] = "ok",
        [LINK_RADIO_ENTRY_CRC] = "crc_err",
        [LINK_RADIO_ENTRY_HEADER_ERR] = "header_err",
    };
    const link_radio_frame_t *f = &e->frame;
    const bool measured = e->kind == LINK_RADIO_ENTRY_FRAME
                          || e->kind == LINK_RADIO_ENTRY_CRC;

    l->len = 0;
    put(l, "{\"type\":\"rx\"");
    put_ms(l, "ts", f->epoch_ms);
    put(l, ",\"status\":\"%s\"", STATUS[e->kind]);

    if (measured) {
        put(l, ",\"len\":%u", (unsigned)f->len);
        put_hex(l, "data", f->payload, f->kept);
        put(l, ",\"rssi_dbm\":%d", (int)f->rssi_dbm);
        put(l, ",\"signal_dbm\":%d", (int)f->signal_dbm);
        put_qdb(l, "snr_db", f->snr_qdb);
        put(l, ",\"freq_err_hz\":%ld", (long)f->freq_error_hz);
    }
    put_toa(l, f->toa_us);
    if (f->radio.cr != 0) {
        put(l, ",\"cr\":%u,\"crc_on\":%s", (unsigned)f->radio.cr,
            f->radio.crc_on ? "true" : "false");
    }
    if (f->noise_n > 0) {
        put(l, ",\"noise_dbm\":%d", (int)f->noise_dbm);
    }
    if (f->inst_n > 0) {
        put(l, ",\"inst_n\":%u,\"inst_min_dbm\":%d,\"inst_max_dbm\":%d",
            (unsigned)f->inst_n, (int)f->inst_min_dbm, (int)f->inst_max_dbm);
    }
    put(l, "}");
}

static void format_tx(line_t *l, const link_radio_tx_t *tx)
{
    static const char *const RESULT[] = {
        [LINK_RADIO_TX_OK] = "ok",
        [LINK_RADIO_TX_TIMEOUT] = "timeout",
        [LINK_RADIO_TX_ABORTED] = "aborted",
    };

    l->len = 0;
    put(l, "{\"type\":\"tx\"");
    put_ms(l, "ts", tx->epoch_ms);
    put(l, ",\"len\":%u", (unsigned)tx->len);
    put_hex(l, "data", tx->payload, tx->len);
    put_toa(l, tx->toa_us);
    put(l, ",\"result\":\"%s\"}", RESULT[tx->result]);
}

static void format_gap(line_t *l, int64_t from_ms, int64_t to_ms)
{
    l->len = 0;
    put(l, "{\"type\":\"gap\"");
    put_ms(l, "from", from_ms);
    put_ms(l, "to", to_ms);
    put(l, ",\"reason\":\"radio_reset\"}");
}

static void format_end(line_t *l, const char *reason)
{
    l->len = 0;
    put(l, "{\"type\":\"end\"");
    put_ms(l, "ts", link_time_now_ms());
    put(l, ",\"reason\":\"%s\"}", reason);
}

static void format_lost(line_t *l, uint32_t count)
{
    l->len = 0;
    put(l, "{\"type\":\"lost\"");
    put_ms(l, "ts", link_time_now_ms());
    put(l, ",\"count\":%lu}", (unsigned long)count);
}

static void part_path(char *buf, size_t size, uint32_t session,
                      unsigned part)
{
    snprintf(buf, size, REC_DIR "/%06lu-%03u.jsonl", (unsigned long)session,
             part);
}

static bool parse_name(const char *name, uint32_t *session, unsigned *part)
{
    unsigned long s;
    unsigned p;
    char ext[8];
    if (sscanf(name, "%6lu-%3u.%7s", &s, &p, ext) != 3
        || strcmp(ext, "jsonl") != 0) {
        return false;
    }
    *session = (uint32_t)s;
    *part = p;
    return true;
}

static uint32_t file_size(uint32_t session, unsigned part)
{
    char path[64];
    part_path(path, sizeof(path), session, part);
    struct stat st;
    return stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
}

/* The highest session, and the bytes it all takes. */
static void scan(uint32_t *max_session, uint32_t *total)
{
    *max_session = 0;
    *total = 0;

    DIR *dir = opendir(REC_DIR);
    if (dir == NULL) {
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        uint32_t session;
        unsigned part;
        if (parse_name(ent->d_name, &session, &part)) {
            if (session > *max_session) {
                *max_session = session;
            }
            *total += file_size(session, part);
        }
    }
    closedir(dir);
}

/* Oldest first, never the file being written. Under s_io_lock. */
static void rotate(void)
{
    while (s_total_bytes > CAP_BYTES) {
        DIR *dir = opendir(REC_DIR);
        if (dir == NULL) {
            return;
        }

        uint32_t old_session = UINT32_MAX;
        unsigned old_part = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            uint32_t session;
            unsigned part;
            if (!parse_name(ent->d_name, &session, &part)
                || (session == s_session && part == s_part)) {
                continue;
            }
            if (session < old_session
                || (session == old_session && part < old_part)) {
                old_session = session;
                old_part = part;
            }
        }
        closedir(dir);

        if (old_session == UINT32_MAX) {
            return;
        }

        char path[64];
        part_path(path, sizeof(path), old_session, old_part);
        const uint32_t size = file_size(old_session, old_part);
        if (remove(path) != 0) {
            ESP_LOGE(TAG, "could not delete %s", path);
            return;
        }
        s_total_bytes = s_total_bytes > size ? s_total_bytes - size : 0;
        ESP_LOGI(TAG, "deleted %s (%lu B)", path, (unsigned long)size);
    }
}

static void flush(void)
{
    s_flushed_us = esp_timer_get_time();
    if (s_buf_len == 0) {
        return;
    }

    xSemaphoreTake(s_io_lock, portMAX_DELAY);

    const bool ok = s_file != NULL
                    && fwrite(s_buf, 1, s_buf_len, s_file) == s_buf_len
                    && fflush(s_file) == 0 && fsync(fileno(s_file)) == 0;
    if (ok) {
        s_file_bytes += s_buf_len;
        s_total_bytes += s_buf_len;
        rotate();
    } else {
        count_lost(s_buf_lines);
        ESP_LOGE(TAG, "write failed (%d), %lu lines lost", errno,
                 (unsigned long)s_buf_lines);
    }

    xSemaphoreGive(s_io_lock);

    s_buf_len = 0;
    s_buf_lines = 0;
}

static void add(line_t *l);

/* Every part opens on the session line, so one whose predecessors
 * rotated away still says what it was recorded with. */
static void open_part(void)
{
    char path[64];
    part_path(path, sizeof(path), s_session, s_part);

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    s_file = fopen(path, "a");
    xSemaphoreGive(s_io_lock);

    s_file_bytes = 0;
    if (s_file == NULL) {
        ESP_LOGE(TAG, "could not open %s (%d)", path, errno);
    } else {
        ESP_LOGI(TAG, "recording to %s", path);
    }

    format_session(&s_head);
    add(&s_head);
}

static void next_part(void)
{
    flush();

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    xSemaphoreGive(s_io_lock);

    s_part++;
    open_part();
}

static void add(line_t *l)
{
    if (l->len >= sizeof(l->text) - 1) {
        count_lost(1);
        ESP_LOGE(TAG, "line too long, dropped");
        return;
    }
    l->text[l->len++] = '\n';

    if (l != &s_head && s_file_bytes + s_buf_len + l->len > PART_MAX) {
        next_part();
    }
    if (s_buf_len + l->len > sizeof(s_buf)) {
        flush();
    }

    memcpy(&s_buf[s_buf_len], l->text, l->len);
    s_buf_len += l->len;
    s_buf_lines++;
    s_lines++;

    if (!s_hold) {
        l->text[l->len - 1] = '\0';
        link_serial_send(l->text);
    }
}

static void open_session(void)
{
    s_session = s_next_id++;
    s_part = 1;
    s_session_ts = link_time_now_ms();
    s_lines = 0;
    link_stats_begin(s_session);
    open_part();
    s_open = true;
}

static void close_session(const char *reason)
{
    if (!s_open) {
        return;
    }

    format_end(&s_event, reason);
    add(&s_event);
    flush();

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    xSemaphoreGive(s_io_lock);

    s_open = false;
    s_open_reason = reason;
    ESP_LOGI(TAG, "session %lu closed: %s", (unsigned long)s_session, reason);
}

typedef struct {
    uint32_t session;
    unsigned part;
} part_id_t;

static int part_cmp(const void *a, const void *b)
{
    const part_id_t *x = a;
    const part_id_t *y = b;
    if (x->session != y->session) {
        return x->session < y->session ? -1 : 1;
    }
    return x->part < y->part ? -1 : x->part > y->part;
}

/* Every part on flash, oldest first; the caller frees *out. */
static int list_parts(part_id_t **out)
{
    *out = NULL;
    DIR *dir = opendir(REC_DIR);
    if (dir == NULL) {
        return 0;
    }

    int count = 0;
    int cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        part_id_t id;
        if (!parse_name(ent->d_name, &id.session, &id.part)) {
            continue;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            part_id_t *grown = realloc(*out, (size_t)cap * sizeof(**out));
            if (grown == NULL) {
                break;
            }
            *out = grown;
        }
        (*out)[count++] = id;
    }
    closedir(dir);

    if (count > 0) {
        qsort(*out, (size_t)count, sizeof(**out), part_cmp);
    }
    return count;
}

int link_rec_read(uint32_t session, unsigned part, uint32_t from,
                  uint32_t to, link_rec_line_cb_t cb, void *user_data)
{
    char path[64];
    part_path(path, sizeof(path), session, part);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    /* The writer and the protocol task both read: no shared buffer. */
    char *line = malloc(REC_LINE_MAX + 2);
    if (line == NULL) {
        fclose(f);
        return -1;
    }

    int count = 0;
    if (from == 0 || fseek(f, (long)from, SEEK_SET) == 0) {
        while ((uint32_t)ftell(f) < to
               && fgets(line, REC_LINE_MAX + 2, f) != NULL) {
            const size_t len = strlen(line);
            if (len == 0 || line[len - 1] != '\n') {
                break;
            }
            line[len - 1] = '\0';
            cb(line, user_data);
            count++;
        }
    }

    free(line);
    fclose(f);
    return count;
}

esp_err_t link_rec_edge_lines(uint32_t session, unsigned part, char *first,
                              char *last, size_t size)
{
    char path[64];
    part_path(path, sizeof(path), session, part);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    first[0] = '\0';
    last[0] = '\0';
    if (fgets(first, (int)size, f) == NULL) {
        fclose(f);
        return ESP_OK;
    }
    char *nl = strchr(first, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }

    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    const long back = end < (long)size - 1 ? end : (long)size - 1;
    fseek(f, end - back, SEEK_SET);
    const size_t n = fread(last, 1, (size_t)back, f);
    last[n] = '\0';
    fclose(f);

    /* The last complete line: after the newline before the final one. */
    if (n > 0 && last[n - 1] == '\n') {
        last[n - 1] = '\0';
    }
    char *start = strrchr(last, '\n');
    if (start != NULL) {
        memmove(last, start + 1, strlen(start + 1) + 1);
    }
    return ESP_OK;
}

int link_rec_list(link_rec_session_t *out, int max)
{
    part_id_t *parts;
    const int n = list_parts(&parts);

    int count = 0;
    for (int i = 0; i < n; i++) {
        if (count > 0 && out[count - 1].id == parts[i].session) {
            out[count - 1].last_part = parts[i].part;
        } else if (count < max) {
            out[count++] = (link_rec_session_t){
                .id = parts[i].session,
                .first_part = parts[i].part,
                .last_part = parts[i].part,
            };
        } else {
            break;
        }
        out[count - 1].bytes += file_size(parts[i].session, parts[i].part);
    }

    free(parts);
    return count;
}

static void send_line(const char *line, void *user_data)
{
    (void)user_data;
    link_serial_send(line);
}

/* Everything written since the mark, sent as live lines. */
static void send_held(void)
{
    part_id_t *parts;
    const int n = list_parts(&parts);

    for (int i = 0; i < n; i++) {
        const part_id_t *p = &parts[i];
        if (p->session < s_mark.session
            || (p->session == s_mark.session && p->part < s_mark.part)) {
            continue;
        }
        const bool at_mark = p->session == s_mark.session
                             && p->part == s_mark.part;
        link_rec_read(p->session, p->part, at_mark ? s_mark.offset : 0,
                      UINT32_MAX, send_line, NULL);
    }

    free(parts);
}

static void hold(void)
{
    flush();
    s_hold = true;
    if (s_open) {
        s_mark = (link_rec_mark_t){s_session, s_part, s_file_bytes};
    } else {
        s_mark = (link_rec_mark_t){s_next_id, 1, 0};
    }
}

static void release(void)
{
    if (!s_hold) {
        return;
    }
    flush();
    send_held();
    s_hold = false;
}

static void erase(void)
{
    close_session("erase");
    s_open_reason = "erase";

    part_id_t *parts;
    const int n = list_parts(&parts);

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        char path[64];
        part_path(path, sizeof(path), parts[i].session, parts[i].part);
        if (remove(path) != 0) {
            ESP_LOGE(TAG, "could not delete %s", path);
        }
    }
    xSemaphoreGive(s_io_lock);

    free(parts);
    s_total_bytes = 0;
    s_lines = 0;
    ESP_LOGI(TAG, "erased %d files", n);
}

/* On the radio task, the only caller: the static is never shared. */
static void listener(const link_radio_event_t *ev)
{
    if (!s_open) {
        return;
    }
    static msg_t msg;
    msg.kind = MSG_EVENT;
    msg.ev = *ev;
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        count_lost(1);
    }
}

static void handle(const link_radio_event_t *ev)
{
    switch (ev->kind) {
    case LINK_RADIO_EVENT_RX:
        format_rx(&s_event, &ev->rx);
        link_stats_note_rx(&ev->rx);
        break;
    case LINK_RADIO_EVENT_TX:
        format_tx(&s_event, &ev->tx);
        link_stats_note_tx(&ev->tx);
        break;
    case LINK_RADIO_EVENT_GAP:
        format_gap(&s_event, ev->gap.from_ms, ev->gap.to_ms);
        break;
    default:
        return;
    }
    add(&s_event);
}

static void rec_task(void *arg)
{
    (void)arg;

    /* Static: a few hundred bytes, off the stack. */
    static msg_t msg;

    while (true) {
        if (!s_open && s_allowed && link_time_is_valid()) {
            open_session();
        }

        TickType_t wait = pdMS_TO_TICKS(IDLE_MS);
        if (s_buf_len > 0) {
            const int64_t left_ms =
                FLUSH_MS - (esp_timer_get_time() - s_flushed_us) / 1000;
            wait = left_ms > 0 ? pdMS_TO_TICKS(left_ms) : 0;
        }

        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            if (msg.kind == MSG_EVENT) {
                handle(&msg.ev);
            } else {
                if (msg.kind == MSG_CLOSE) {
                    close_session(msg.reason);
                } else if (msg.kind == MSG_HOLD) {
                    hold();
                } else if (msg.kind == MSG_RELEASE) {
                    release();
                } else if (msg.kind == MSG_ERASE) {
                    erase();
                }
                if (msg.kind != MSG_WAKE) {
                    xSemaphoreGive(s_req_done);
                }
                continue;
            }
        }

        const uint32_t lost = __atomic_load_n(&s_lost, __ATOMIC_RELAXED);
        if (s_open && lost != s_lost_reported) {
            format_lost(&s_event, lost - s_lost_reported);
            s_lost_reported = lost;
            add(&s_event);
        }

        if (s_buf_len > 0
            && (esp_timer_get_time() - s_flushed_us) / 1000 >= FLUSH_MS) {
            flush();
        }
    }
}

static void on_time_set(void)
{
    link_rec_close("time");
}

static void on_power_off(const char *why)
{
    link_rec_close(why != NULL && strstr(why, "battery") != NULL
                       ? "battery" : "shutdown");
}

static void on_restart(void)
{
    link_rec_close("reboot_cmd");
}

esp_err_t link_rec_start(void)
{
    if (mkdir(REC_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "no %s (%d): not recording", REC_DIR, errno);
        return ESP_ERR_NOT_FOUND;
    }

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    s_io_lock = xSemaphoreCreateMutex();
    s_req_lock = xSemaphoreCreateMutex();
    s_req_done = xSemaphoreCreateBinary();
    if (s_queue == NULL || s_io_lock == NULL || s_req_lock == NULL
        || s_req_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t max_session;
    uint32_t total;
    scan(&max_session, &total);
    s_total_bytes = total;
    ESP_LOGI(TAG, "%lu B on flash, last session %lu", (unsigned long)total,
             (unsigned long)max_session);

    s_next_id = max_session + 1;
    if (xTaskCreate(rec_task, "rec", 6144, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    link_radio_set_listener(listener);
    link_time_set_before_set(on_time_set);
    power_key_set_before_off(on_power_off);
    esp_register_shutdown_handler(on_restart);
    return ESP_OK;
}

void link_rec_allow(void)
{
    if (s_queue == NULL || s_allowed) {
        return;
    }
    s_allowed = true;

    /* Only the LVGL task calls this. */
    static msg_t wake = {.kind = MSG_WAKE};
    xQueueSend(s_queue, &wake, 0);
}

/* Done by the writer, in turn with the events queued before it. */
static void request(msg_kind_t kind, const char *reason, uint32_t wait_ms)
{
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    static msg_t msg;
    msg.kind = kind;
    msg.reason = reason;
    xSemaphoreTake(s_req_done, 0);
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(wait_ms)) != pdTRUE
        || xSemaphoreTake(s_req_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "request %d not served", (int)kind);
    }
    xSemaphoreGive(s_req_lock);
}

void link_rec_close(const char *reason)
{
    if (s_queue == NULL) {
        return;
    }

    /* Before the request: an allow() racing it opens the next session
     * once this one is closed, not before. */
    s_allowed = false;
    if (!s_open) {
        return;
    }
    request(MSG_CLOSE, reason, CLOSE_WAIT_MS);
}

void link_rec_hold(link_rec_mark_t *out)
{
    if (s_queue != NULL) {
        request(MSG_HOLD, NULL, REQ_WAIT_MS);
    }
    *out = s_mark;
}

void link_rec_release(void)
{
    if (s_queue != NULL) {
        request(MSG_RELEASE, NULL, REQ_WAIT_MS);
    }
}

esp_err_t link_rec_erase(void)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_allowed = false;
    request(MSG_ERASE, NULL, REQ_WAIT_MS);
    return ESP_OK;
}

void link_rec_get_stats(link_rec_stats_t *out)
{
    out->open = s_open;
    out->session = s_session;
    out->lines = s_lines;
    out->lost = __atomic_load_n(&s_lost, __ATOMIC_RELAXED);
    out->total_bytes = s_total_bytes;
}
