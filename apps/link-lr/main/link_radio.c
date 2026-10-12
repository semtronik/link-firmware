#include "link_radio.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_regul.h"
#include "link_time.h"
#include "sx1262.h"

static const char *TAG = "link_radio";

/*
 * Longest wait before looking anyway, ms.
 *
 * Not a poll interval: DIO1 wakes the task on every frame. This only
 * bounds how long a silent band goes unexamined, and covers the case of
 * an interrupt armed while the line is already high, where there is no
 * edge left to catch.
 */
#define IDLE_MS 100

/* Fallback pace when the interrupt could not be armed at all. */
#define POLL_MS 20

/* Instantaneous RSSI pace during a frame. */
#define SAMPLE_MS 10

/* Between frames: slower, and only the last NOISE_LEN readings are kept. */
#define NOISE_MS  100
#define NOISE_LEN 20

/* Scratch space in the data buffer area, not a config register: writing
 * it cannot upset anything. */
#define SCRATCH_REG 0x06BC
#define SCRATCH_VAL 0x5A

/*
 * The module sits on a header and may not be fitted. Nothing in the
 * command path notices: with no chip on the bus, every write goes
 * nowhere and every read comes back as whatever MISO floats to, so the
 * driver reports success and the firmware claims to be listening on a
 * radio that is not there.
 *
 * A write followed by a read is what distinguishes the two. Real memory
 * gives back what was put in it; an empty header gives back a constant.
 */
static bool chip_answers(void)
{
    const uint8_t out = SCRATCH_VAL;
    if (sx1262_reg_write(SCRATCH_REG, &out, 1) != ESP_OK) {
        return false;
    }

    uint8_t back = 0;
    if (sx1262_reg_read(SCRATCH_REG, &back, 1) != ESP_OK) {
        return false;
    }

    return back == SCRATCH_VAL;
}

static SemaphoreHandle_t s_lock;
static link_radio_state_t s_state;
static sx1262_radio_cfg_t s_cfg;

/* Set once the task runs: requests have someone to serve them. */
static bool s_started;

/* The chip is lent out: the task must not touch the bus. */
static bool s_suspended;

/*
 * Requests to the task, which alone drives the chip.
 *
 * One slot: s_call_lock serialises callers, and each waits on s_req_done
 * for its answer. The task is woken through the DIO1 semaphore it
 * already waits on.
 */
typedef enum {
    REQ_CONFIGURE,
    REQ_SUSPEND,
    REQ_RESUME,
    REQ_TX,
} req_kind_t;

static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_req_done;
static SemaphoreHandle_t s_wake;
static volatile bool s_req_pending;
static req_kind_t s_req_kind;
static sx1262_radio_cfg_t s_req_cfg;
static uint8_t s_req_payload[SX1262_MAX_PAYLOAD];
static size_t s_req_len;
static esp_err_t s_req_err;

/*
 * The frame on air, if any. Followed by the task between wakes, so a
 * caller never waits for the whole time on air.
 */
static bool s_tx_active;
static int64_t s_tx_start_us;
static int64_t s_tx_deadline_us;
static uint8_t s_tx_len;
static uint8_t s_tx_payload[SX1262_MAX_PAYLOAD];
static uint32_t s_tx_toa_us;

/* Margin over the time on air before TX done is given up on. */
#define TX_GUARD_US (100 * 1000)

/* A header with no frame behind it yet. The two arrive as separate
 * interrupts with the whole payload in between. */
static bool s_pending;
static int64_t s_pending_at_ms;

/* A preamble caught after the header belongs to the next reception. */
static int64_t s_preamble_us;
static int64_t s_header_us;
static uint8_t s_header_cr;
static bool s_header_crc_on;

/*
 * DIO1 is a level shared by every stage: a stage arriving before the
 * previous flag is cleared raises no edge, hence no stamp. This task reads
 * and clears at once, faster than stages can follow each other (~300 us
 * at SF5/500 kHz).
 */
#define IRQ_TASK_PRIO 20
#define IRQ_QUEUE_LEN 16
#define IRQ_STAMPS_MAX 8

typedef struct {
    int64_t at_us;
    uint16_t irq; /* one stage */
} irq_event_t;

/* In the order LoRa produces them; edges are paired in this order. */
static const uint16_t IRQ_STAGES[] = {
    SX1262_IRQ_PREAMBLE_DET,
    SX1262_IRQ_HEADER_VALID | SX1262_IRQ_HEADER_ERR,
    SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR,
    SX1262_IRQ_TX_DONE | SX1262_IRQ_TIMEOUT,
};
#define IRQ_STAGE_COUNT (sizeof(IRQ_STAGES) / sizeof(IRQ_STAGES[0]))

static QueueHandle_t s_irq_queue;
static SemaphoreHandle_t s_dio1;
static volatile esp_err_t s_irq_health = ESP_OK;

/* A read must not straddle a re-arm: it would pair an old edge with a new
 * flag. */
static SemaphoreHandle_t s_chip_lock;

/* Edges not yet matched to a flag. Under s_chip_lock. */
static int64_t s_stamp_carry[IRQ_STAMPS_MAX];
static int s_stamp_carry_n;

/*
 * A chip that stops answering - BUSY stuck high after a glitch, a module
 * knocked on its header - is reset and brought back rather than left
 * dead until the next boot. Tried at once, then less and less often.
 */
#define FAIL_LIMIT     5
#define RETRY_FIRST_MS 1000
#define RETRY_MAX_MS   30000

static uint32_t s_failures;
static bool s_recovering;
static uint32_t s_retry_ms;
static int64_t s_retry_at_us;
static int64_t s_recovery_from_us;

static link_radio_listener_t s_listener;

/* Samples during the frame under way. */
static uint16_t s_fr_n;
static int16_t s_fr_min;
static int16_t s_fr_max;
static int64_t s_next_sample_us;

/* Readings between frames, a ring; the oldest is overwritten. */
static int16_t s_noise[NOISE_LEN];
static uint8_t s_noise_n;
static uint8_t s_noise_at;
static int64_t s_next_noise_us;

/* Wall-clock ms of a boot-time instant; 0 on an unset clock. */
static int64_t epoch_at(int64_t at_us)
{
    if (!link_time_is_valid()) {
        return 0;
    }
    return link_time_now_ms() - (esp_timer_get_time() - at_us) / 1000;
}

/* Start of the preamble to HeaderValid: preamble, sync, 8 header symbols. */
static int64_t header_offset_us(void)
{
    const int64_t sym_us = ((int64_t)1 << s_cfg.sf) * 1000000 / s_cfg.bw_hz;
    const int64_t quarters =
        (int64_t)(s_cfg.preamble_len + (s_cfg.sf <= 6 ? 6 : 4) + 8) * 4 + 1;
    return sym_us * quarters / 4;
}

/* Fill a frame record from what the driver read out. start_us is the
 * frame's first symbol, boot time. */
static void fill_frame(link_radio_frame_t *f, const sx1262_rx_t *rx,
                       int64_t start_us)
{
    f->epoch_ms = epoch_at(start_us);
    f->ts_ms = start_us / 1000;
    f->rssi_dbm = rx->rssi_dbm;
    f->snr_qdb = rx->snr_qdb;
    f->signal_dbm = rx->signal_dbm;
    f->freq_error_hz = rx->freq_error_hz;
    f->crc_error = rx->crc_error;
    f->len = rx->len;

    /* Always the whole frame: the buffer holds the longest a LoRa
     * length field can express, so there is nothing to truncate. `kept`
     * stays in the record because readers use it, and because a smaller
     * buffer would bring the distinction back. */
    f->kept = rx->len;
    memcpy(f->payload, rx->payload, f->kept);
}


static void notify(const link_radio_event_t *ev)
{
    if (s_listener != NULL) {
        s_listener(ev);
    }
}

static void frame_inst_reset(void)
{
    s_fr_n = 0;
}

static void noise_reset(void)
{
    s_noise_n = 0;
    s_noise_at = 0;
}

static void noise_push(int16_t dbm)
{
    s_noise[s_noise_at] = dbm;
    s_noise_at = (uint8_t)((s_noise_at + 1) % NOISE_LEN);
    if (s_noise_n < NOISE_LEN) {
        s_noise_n++;
    }
}

/* The median, so that a burst between frames barely moves it. */
static int16_t noise_median(void)
{
    int16_t v[NOISE_LEN];
    memcpy(v, s_noise, sizeof(v));
    for (uint8_t i = 1; i < s_noise_n; i++) {
        const int16_t x = v[i];
        uint8_t j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
    return v[(s_noise_n - 1) / 2];
}

/* What was sampled during the frame, then ready for the next one. */
static void frame_inst_take(link_radio_frame_t *f)
{
    f->inst_n = s_fr_n;
    f->inst_min_dbm = s_fr_min;
    f->inst_max_dbm = s_fr_max;
    s_fr_n = 0;
    f->noise_n = s_noise_n;
    f->noise_dbm = s_noise_n > 0 ? noise_median() : 0;
}

/* Push a log entry, newest first, oldest falling off the end. */
static void log_entry(const link_radio_entry_t *e)
{
    /* Static: the event is a few hundred bytes, and only this task logs. */
    static link_radio_event_t ev;
    ev.kind = LINK_RADIO_EVENT_RX;
    ev.rx = *e;
    notify(&ev);

    memmove(&s_state.entry[1], &s_state.entry[0],
            (LINK_RADIO_LOG - 1) * sizeof(s_state.entry[0]));
    s_state.entry[0] = *e;
    if (s_state.entries < LINK_RADIO_LOG) {
        s_state.entries++;
    }
    s_state.entry_seq++;
}

static void publish(const sx1262_rx_t *rx, int64_t at_ms, int64_t stamp_us)
{
    /* The frame as it was sent: coding rate and CRC come from its header. */
    sx1262_radio_cfg_t sent = s_cfg;
    sent.cr = rx->cr;
    sent.crc_on = rx->crc_on;
    const uint32_t toa_us = sx1262_radio_time_on_air_us(&sent, rx->len);
    const int64_t start_us = stamp_us - toa_us;

    link_radio_entry_t e = {
        .kind = rx->crc_error ? LINK_RADIO_ENTRY_CRC
                              : LINK_RADIO_ENTRY_FRAME,
        .at_ms = at_ms != 0 ? at_ms : start_us / 1000,
    };
    fill_frame(&e.frame, rx, start_us);
    frame_inst_take(&e.frame);
    e.frame.preamble_us = s_preamble_us;
    e.frame.header_us = s_header_us;
    e.frame.end_us = stamp_us;
    e.frame.toa_us = toa_us;
    e.frame.radio = sent;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /*
     * A frame is only published to the link once the clock can stamp
     * it: the stamp is what says when something was heard, and one
     * taken from an unset clock is worse than none. The log below keeps
     * it regardless - it measures against boot time, which is always
     * good, and the diagnostic screen has to work before onboarding.
     */
    if (rx->crc_error || !link_time_is_valid()) {
        if (rx->crc_error) {
            s_state.crc_errors++;
            s_state.crc_seq++;
        }
    } else {
        /* Measured on the monotonic clock, not the wall one: a re-sync
         * between two frames would otherwise show up as a gap that never
         * happened. */
        const int64_t delta = s_state.last.ts_ms != 0
                                  ? e.frame.ts_ms - s_state.last.ts_ms
                                  : -1;

        s_state.rx_count++;
        s_state.rx_seq++;
        s_state.last = e.frame;
        s_state.last.delta_ms = delta;
    }

    log_entry(&e);

    xSemaphoreGive(s_lock);
}

/*
 * How long to wait for a frame after its header, ms.
 *
 * A header is decoded once its own few symbols are in; the payload
 * follows and can run to hundreds of milliseconds at the high spreading
 * factors. So a header with no frame yet is the normal state for most
 * of a reception, and only becomes a loss once even the longest
 * possible payload would have arrived.
 *
 * Symbol time is 2^SF / BW; the longest LoRa payload is a little over
 * three hundred symbols at the worst coding rate. Doubled, because what
 * is wanted is a bound that never fires early - a loss reported late
 * costs nothing, one reported early is a wrong answer.
 */
static uint32_t frame_timeout_ms(uint8_t sf, uint32_t bw_hz)
{
    if (sf < 5 || sf > 12 || bw_hz == 0) {
        return 4000;
    }

    const uint32_t symbol_us =
        (uint32_t)(((uint64_t)1 << sf) * 1000000U / bw_hz);
    return (uint32_t)((uint64_t)symbol_us * 320U * 2U / 1000U);
}

/* Close a reception that never completed, once no frame could still be
 * coming. */
static void check_lost(void)
{
    if (s_pending
        && esp_timer_get_time() / 1000 - s_pending_at_ms
               > (int64_t)frame_timeout_ms(s_cfg.sf, s_cfg.bw_hz)) {
        link_radio_entry_t lost = {
            .kind = LINK_RADIO_ENTRY_LOST,
            .at_ms = s_pending_at_ms,
        };

        /* Dated from its header, not from the timeout. */
        const int64_t start_us = s_header_us - header_offset_us();
        lost.frame.ts_ms = start_us / 1000;
        lost.frame.epoch_ms = epoch_at(start_us);
        lost.frame.delta_ms = -1;
        lost.frame.preamble_us = s_preamble_us;
        lost.frame.header_us = s_header_us;
        frame_inst_take(&lost.frame);
        lost.frame.radio = s_cfg;
        lost.frame.radio.cr = s_header_cr;
        lost.frame.radio.crc_on = s_header_crc_on;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        log_entry(&lost);
        xSemaphoreGive(s_lock);

        s_pending = false;
        s_preamble_us = 0;
        s_header_us = 0;
    }
}

/* Apply cfg and re-arm. Leaves the settings untouched on failure. */
static esp_err_t apply(const sx1262_radio_cfg_t *cfg)
{
    esp_err_t err = sx1262_radio_configure(cfg);
    if (err == ESP_OK) {
        err = sx1262_radio_rx_start();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        s_cfg = *cfg;
    }
    s_state.listening = err == ESP_OK;
    xSemaphoreGive(s_lock);

    s_pending = false;
    noise_reset();
    return err;
}

/* Everything captured before a re-arm. Under s_chip_lock. */
static void flush_stamps(void)
{
    int64_t stale;
    while (sx1262_dio1_irq_stamp(&stale)) {
    }
    s_stamp_carry_n = 0;
    if (s_irq_queue != NULL) {
        xQueueReset(s_irq_queue);
    }
}

static void chip_begin(void)
{
    xSemaphoreTake(s_chip_lock, portMAX_DELAY);
}

static void chip_end(void)
{
    flush_stamps();
    xSemaphoreGive(s_chip_lock);
}

static void set_listening(bool listening)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.listening = listening;
    xSemaphoreGive(s_lock);
}

static void enter_recovery(void)
{
    s_recovering = true;
    s_retry_ms = RETRY_FIRST_MS;
    s_retry_at_us = esp_timer_get_time();
    s_recovery_from_us = s_retry_at_us;
    s_pending = false;
    set_listening(false);

    /* Once per incident: the console shares its link with the protocol. */
    ESP_LOGW(TAG, "radio not answering, resetting it");
}

/* Count an access to the chip; enough failures in a row and it is reset. */
static void note_access(esp_err_t err)
{
    if (err == ESP_OK) {
        s_failures = 0;
        return;
    }

    if (++s_failures >= FAIL_LIMIT && !s_recovering) {
        enter_recovery();
    }
}

/* Reset, reconfigure and re-arm; on failure, try again later. */
static void try_recover(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_retry_at_us) {
        return;
    }

    esp_err_t err = sx1262_radio_init(&s_cfg);
    /* An empty header answers every command too: only the scratch
     * register tells a chip that is back from one that is gone. */
    if (err == ESP_OK && !chip_answers()) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        err = sx1262_radio_rx_start();
    }

    if (err != ESP_OK) {
        s_retry_at_us = now_us + (int64_t)s_retry_ms * 1000;
        s_retry_ms = s_retry_ms * 2 < RETRY_MAX_MS ? s_retry_ms * 2
                                                   : RETRY_MAX_MS;
        return;
    }

    s_recovering = false;
    s_failures = 0;
    flush_stamps();
    noise_reset();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.listening = true;
    s_state.recoveries++;
    xSemaphoreGive(s_lock);

    static link_radio_event_t ev;
    ev.kind = LINK_RADIO_EVENT_GAP;
    ev.gap.from_ms = epoch_at(s_recovery_from_us);
    ev.gap.to_ms = epoch_at(esp_timer_get_time());
    notify(&ev);

    ESP_LOGW(TAG, "radio back");
}

/* Record how the frame on air ended. The caller re-arms, or not. */
static void tx_finish(link_radio_tx_result_t result, int64_t end_us)
{
    s_tx_active = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_state.transmitting = false;
    s_state.tx.seq++;
    s_state.tx.result = result;
    s_state.tx.epoch_ms = epoch_at(s_tx_start_us);
    s_state.tx.ts_ms = s_tx_start_us / 1000;
    s_state.tx.len = s_tx_len;
    memcpy(s_state.tx.payload, s_tx_payload, s_tx_len);
    s_state.tx.toa_us = s_tx_toa_us;
    s_state.tx.radio = s_cfg;

    static link_radio_event_t ev;
    ev.kind = LINK_RADIO_EVENT_TX;
    ev.tx = s_state.tx;

    xSemaphoreGive(s_lock);

    notify(&ev);

    /* What was actually on air, cut short or not. */
    link_regul_record(s_cfg.freq_hz, s_cfg.bw_hz,
                      (uint32_t)(end_us - s_tx_start_us));
}

static void rearm_rx(void)
{
    chip_begin();
    const esp_err_t err = sx1262_radio_rx_start();
    chip_end();

    note_access(err);
    set_listening(err == ESP_OK);
}

/* For a chip that raises neither TX done nor timeout. */
static void tx_check_deadline(void)
{
    const int64_t now_us = esp_timer_get_time();

    if (s_recovering) {
        tx_finish(LINK_RADIO_TX_ABORTED, now_us);
        return;
    }
    if (now_us < s_tx_deadline_us) {
        return;
    }

    tx_finish(LINK_RADIO_TX_TIMEOUT, now_us);
    rearm_rx();
}

static esp_err_t tx_begin(const uint8_t *payload, size_t len)
{
    if (s_suspended) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (s_tx_active) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (s_recovering) {
        return ESP_ERR_INVALID_STATE;
    }

    /* A reception in progress is given up: the receiver goes off. */
    s_pending = false;
    flush_stamps();

    s_tx_len = (uint8_t)len;
    memcpy(s_tx_payload, payload, len);
    s_tx_toa_us = sx1262_radio_time_on_air_us(&s_cfg, len);
    s_tx_start_us = esp_timer_get_time();

    const esp_err_t err = sx1262_radio_tx_start(payload, len);
    note_access(err);
    if (err != ESP_OK) {
        sx1262_radio_rx_start();
        return err;
    }

    s_tx_deadline_us = s_tx_start_us + (int64_t)s_tx_toa_us * 3 / 2
                       + TX_GUARD_US;
    s_tx_active = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.transmitting = true;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

static esp_err_t serve(req_kind_t kind, const sx1262_radio_cfg_t *cfg)
{
    /* Anything that changes the chip's state cuts a frame short rather
     * than waiting out its time on air. */
    if (s_tx_active && kind != REQ_TX) {
        sx1262_radio_standby();
        tx_finish(LINK_RADIO_TX_ABORTED, esp_timer_get_time());
        flush_stamps();
        if (kind == REQ_RESUME && !s_suspended) {
            sx1262_radio_rx_start();
        }
    }

    switch (kind) {
    case REQ_TX:
        return tx_begin(s_req_payload, s_req_len);

    case REQ_CONFIGURE: {
        /* Checked before anything is touched: settings the chip would
         * refuse leave it listening as it was. */
        esp_err_t err = sx1262_radio_check_cfg(cfg);
        if (err != ESP_OK) {
            return err;
        }

        const bool was_boosted = sx1262_radio_rx_boosted();
        sx1262_radio_set_rx_boosted(s_req_payload[0] != 0);

        /* Lent out or being reset: remembered, and applied when the
         * chip comes back. */
        if (s_suspended || s_recovering) {
            s_cfg = *cfg;
            return ESP_OK;
        }

        err = apply(cfg);
        if (err != ESP_OK) {
            sx1262_radio_set_rx_boosted(was_boosted);
        }
        note_access(err);
        return err;
    }

    case REQ_SUSPEND:
        if (!s_suspended) {
            sx1262_radio_standby();
            s_suspended = true;
            s_pending = false;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_state.listening = false;
            xSemaphoreGive(s_lock);
        }
        return ESP_OK;

    case REQ_RESUME: {
        if (!s_suspended) {
            return ESP_OK;
        }

        /* The borrower armed its own receiver: its interrupt instants
         * describe nothing of ours. */
        flush_stamps();

        /* Back in our hands whatever happens next: a chip that will not
         * take the settings is one to reset, not one to leave lent. */
        s_suspended = false;

        /* Re-applied in full: the borrower left its own frequency and
         * bandwidth on the chip. */
        const esp_err_t err = apply(&s_cfg);
        if (err != ESP_OK && !s_recovering) {
            enter_recovery();
        }
        return err;
    }
    }

    return ESP_ERR_INVALID_ARG;
}

/* Logged once: the console shares its link with the protocol. */
static void note_anomaly(const char *what)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t n = ++s_state.irq_anomalies;
    xSemaphoreGive(s_lock);

    if (n == 1) {
        ESP_LOGW(TAG, "irq: %s - a stamp is not its event's own", what);
    } else {
        ESP_LOGD(TAG, "irq: %s (%lu)", what, (unsigned long)n);
    }
}

/*
 * Only edges from before the read belong to it: during the read the line
 * is held high, so a later edge comes from a flag this read did not see.
 */
static void irq_dispatch(uint16_t irq, int64_t read_start_us,
                         int64_t read_end_us)
{
    uint16_t stages[IRQ_STAGE_COUNT];
    int found = 0;

    for (size_t i = 0; i < IRQ_STAGE_COUNT; i++) {
        if (irq & IRQ_STAGES[i]) {
            stages[found++] = irq & IRQ_STAGES[i];
        }
    }

    int usable = 0;
    while (usable < s_stamp_carry_n
           && s_stamp_carry[usable] < read_start_us) {
        usable++;
    }
    int skip = 0;
    if (usable > found) {
        skip = usable - found;
        note_anomaly("edge with no flag behind it");
    }

    uint32_t latency_max = 0;

    for (int i = 0; i < found; i++) {
        irq_event_t ev = {.irq = stages[i]};

        if (skip + i < usable) {
            ev.at_us = s_stamp_carry[skip + i];
            const uint32_t latency = (uint32_t)(read_end_us - ev.at_us);
            if (latency > latency_max) {
                latency_max = latency;
            }
        } else {
            /* No edge of its own: it happened before this read. */
            ev.at_us = read_start_us;
            note_anomaly("stage without an edge of its own");
        }

        if (xQueueSend(s_irq_queue, &ev, 0) != pdTRUE) {
            note_anomaly("event queue full");
        }
    }

    memmove(&s_stamp_carry[0], &s_stamp_carry[usable],
            (size_t)(s_stamp_carry_n - usable) * sizeof(s_stamp_carry[0]));
    s_stamp_carry_n -= usable;

    if (latency_max > 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (latency_max > s_state.irq_latency_max_us) {
            s_state.irq_latency_max_us = latency_max;
        }
        xSemaphoreGive(s_lock);
    }
}

static void irq_collect_stamps(void)
{
    int64_t at_us;

    while (sx1262_dio1_irq_stamp(&at_us)) {
        if (s_stamp_carry_n < IRQ_STAMPS_MAX) {
            s_stamp_carry[s_stamp_carry_n++] = at_us;
        } else {
            note_anomaly("more edges than the service can hold");
        }
    }
}

static void irq_task(void *arg)
{
    (void)arg;

    while (true) {
        /* Timeout: a line already high when armed gives no edge. */
        if (s_dio1 != NULL) {
            xSemaphoreTake(s_dio1, pdMS_TO_TICKS(IDLE_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }

        xSemaphoreTake(s_chip_lock, portMAX_DELAY);

        if (s_suspended || s_recovering) {
            flush_stamps();
        } else {
            /* Until empty: a flag that rose during the read keeps the line
             * high and would never raise an edge. */
            for (int pass = 0; pass < (int)IRQ_STAGE_COUNT; pass++) {
                uint16_t irq = 0;
                const int64_t read_start_us = esp_timer_get_time();
                const esp_err_t err = sx1262_radio_irq_take(&irq);
                const int64_t read_end_us = esp_timer_get_time();

                s_irq_health = err;
                if (err != ESP_OK) {
                    break;
                }

                irq_collect_stamps();

                if (irq == 0) {
                    int stale = 0;
                    while (stale < s_stamp_carry_n
                           && s_stamp_carry[stale] < read_start_us) {
                        stale++;
                    }
                    if (stale > 0) {
                        note_anomaly("edge with no flag behind it");
                        memmove(&s_stamp_carry[0], &s_stamp_carry[stale],
                                (size_t)(s_stamp_carry_n - stale)
                                    * sizeof(s_stamp_carry[0]));
                        s_stamp_carry_n -= stale;
                    }
                    break;
                }

                irq_dispatch(irq, read_start_us, read_end_us);
            }
        }

        xSemaphoreGive(s_chip_lock);

        if (s_wake != NULL) {
            xSemaphoreGive(s_wake);
        }
    }
}

static void handle_event(const irq_event_t *ev)
{
    const uint16_t irq = ev->irq;
    const int64_t at_us = ev->at_us;

    if (irq & (SX1262_IRQ_TX_DONE | SX1262_IRQ_TIMEOUT)) {
        if (s_tx_active) {
            tx_finish((irq & SX1262_IRQ_TX_DONE) ? LINK_RADIO_TX_OK
                                                 : LINK_RADIO_TX_TIMEOUT,
                      at_us);
            rearm_rx();
        }
        return;
    }

    if (s_tx_active) {
        return;
    }

    if (irq & SX1262_IRQ_PREAMBLE_DET) {
        if (!s_pending) {
            s_preamble_us = at_us;
            frame_inst_reset();
        }
    }

    if (irq & SX1262_IRQ_HEADER_ERR) {
        const int64_t start_us = at_us - header_offset_us();
        link_radio_entry_t bad = {
            .kind = LINK_RADIO_ENTRY_HEADER_ERR,
            .at_ms = at_us / 1000,
        };
        bad.frame.ts_ms = start_us / 1000;
        bad.frame.epoch_ms = epoch_at(start_us);
        bad.frame.delta_ms = -1;
        bad.frame.preamble_us = s_preamble_us;
        bad.frame.header_us = at_us;
        frame_inst_take(&bad.frame);
        bad.frame.radio = s_cfg;
        bad.frame.radio.cr = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        log_entry(&bad);
        xSemaphoreGive(s_lock);

        s_preamble_us = 0;
    }

    if (irq & SX1262_IRQ_HEADER_VALID) {
        s_pending = true;
        s_pending_at_ms = at_us / 1000;
        s_header_us = at_us;
        /* Read now: a frame that never completes keeps its header's. */
        note_access(sx1262_radio_rx_header(&s_header_cr, &s_header_crc_on));
    }

    if (irq & SX1262_IRQ_RX_DONE) {
        sx1262_rx_t rx;
        const esp_err_t err = sx1262_radio_rx_read(&rx);
        note_access(err);
        if (err != ESP_OK) {
            return;
        }

        rx.crc_error = (irq & SX1262_IRQ_CRC_ERR) != 0;
        publish(&rx, s_pending ? s_pending_at_ms : 0, at_us);

        /* Debug, not info: this is per frame, and the console shares its
         * endpoint with the JSON protocol. */
        ESP_LOGD(TAG, "rx %u B, %d dBm, %.2f dB, signal %d dBm%s",
                 (unsigned)rx.len, (int)rx.rssi_dbm, rx.snr_qdb / 4.0,
                 (int)rx.signal_dbm, rx.crc_error ? " CRC!" : "");

        s_pending = false;
        s_preamble_us = 0;
        s_header_us = 0;
    }
}

/* A preamble with no header after it is a false detection once the
 * header is overdue. */
static bool in_frame(int64_t now_us)
{
    return s_pending
           || (s_preamble_us != 0
               && now_us - s_preamble_us < 2 * header_offset_us());
}

/* Read-only: under the chip lock so it does not interleave with an IRQ
 * read, but no flush - nothing is re-armed. */
static void sample_channel(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_tx_active) {
        return;
    }
    const bool framing = in_frame(now_us);
    if (framing) {
        if (now_us < s_next_sample_us) {
            return;
        }
        s_next_sample_us = now_us + SAMPLE_MS * 1000;
    } else {
        if (now_us < s_next_noise_us) {
            return;
        }
        s_next_noise_us = now_us + NOISE_MS * 1000;
    }

    int16_t dbm = 0;
    xSemaphoreTake(s_chip_lock, portMAX_DELAY);
    const esp_err_t err = sx1262_radio_rssi_inst(&dbm);
    xSemaphoreGive(s_chip_lock);
    note_access(err == ESP_ERR_INVALID_RESPONSE ? ESP_OK : err);

    if (err == ESP_OK && !framing) {
        noise_push(dbm);
    } else if (err == ESP_OK) {
        if (s_fr_n == 0 || dbm < s_fr_min) {
            s_fr_min = dbm;
        }
        if (s_fr_n == 0 || dbm > s_fr_max) {
            s_fr_max = dbm;
        }
        s_fr_n++;
    }
}

static void rx_task(void *arg)
{
    (void)arg;

    while (true) {
        /* The buffer holds one frame: the next overwrites it if the read
         * comes late. */
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(SAMPLE_MS));

        /* Events before requests: a reconfiguration would lose a frame
         * still sitting in the buffer. */
        irq_event_t ev;

        if (s_suspended) {
            /* Lent out: nothing of ours to read or to recover. */
            while (xQueueReceive(s_irq_queue, &ev, 0) == pdTRUE) {
            }
        } else if (s_recovering) {
            chip_begin();
            try_recover();
            chip_end();
        } else {
            while (xQueueReceive(s_irq_queue, &ev, 0) == pdTRUE) {
                handle_event(&ev);
            }

            if (s_tx_active) {
                tx_check_deadline();
            } else {
                check_lost();
            }

            sample_channel();
            note_access(s_irq_health);
        }

        if (s_req_pending) {
            chip_begin();
            s_req_err = serve(s_req_kind, &s_req_cfg);
            chip_end();
            s_req_pending = false;
            xSemaphoreGive(s_req_done);
        }
    }
}

/* Hand a request to the task and wait for its answer. */
static esp_err_t request(req_kind_t kind, const sx1262_radio_cfg_t *cfg,
                         const uint8_t *payload, size_t len)
{
    /* No task, no module: nothing would ever answer. */
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_call_lock, portMAX_DELAY);

    s_req_kind = kind;
    if (cfg != NULL) {
        s_req_cfg = *cfg;
    }
    if (payload != NULL) {
        memcpy(s_req_payload, payload, len);
        s_req_len = len;
    }
    s_req_pending = true;
    if (s_wake != NULL) {
        xSemaphoreGive(s_wake);
    }

    /* Every step of a request is bounded - BUSY and the SPI time out -
     * so the task always answers. */
    xSemaphoreTake(s_req_done, portMAX_DELAY);
    const esp_err_t err = s_req_err;

    xSemaphoreGive(s_call_lock);
    return err;
}

void link_radio_set_listener(link_radio_listener_t listener)
{
    s_listener = listener;
}

esp_err_t link_radio_start(const sx1262_radio_cfg_t *cfg, bool rx_boosted)
{
    sx1262_radio_set_rx_boosted(rx_boosted);

    s_lock = xSemaphoreCreateMutex();
    s_call_lock = xSemaphoreCreateMutex();
    s_req_done = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_call_lock == NULL || s_req_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        sx1262_radio_default_cfg(&s_cfg);
    }

    esp_err_t err = sx1262_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Releases the chip from reset, which the presence check needs. */
    err = sx1262_radio_init(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!chip_answers()) {
        ESP_LOGE(TAG, "no module answering on the header");
        ESP_LOGE(TAG, "  check: module seated, 3V3 on the header");
        return ESP_ERR_NOT_FOUND;
    }

    err = sx1262_radio_rx_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_state.listening = true;

    s_wake = xSemaphoreCreateBinary();
    s_chip_lock = xSemaphoreCreateMutex();
    s_irq_queue = xQueueCreate(IRQ_QUEUE_LEN, sizeof(irq_event_t));
    if (s_wake == NULL || s_chip_lock == NULL || s_irq_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (sx1262_dio1_irq_enable(&s_dio1) != ESP_OK) {
        s_dio1 = NULL;
        ESP_LOGE(TAG, "DIO1 interrupt unavailable, falling back to polling");
    }

    if (xTaskCreate(rx_task, "radio_rx", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(irq_task, "radio_irq", 3072, NULL, IRQ_TASK_PRIO, NULL)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    ESP_LOGI(TAG, "listening on %lu Hz, SF%u, BW %lu Hz",
             (unsigned long)s_cfg.freq_hz, (unsigned)s_cfg.sf,
             (unsigned long)s_cfg.bw_hz);

    return ESP_OK;
}

esp_err_t link_radio_configure(const sx1262_radio_cfg_t *cfg, bool rx_boosted)
{
    const uint8_t gain = rx_boosted ? 1 : 0;
    const esp_err_t err = request(REQ_CONFIGURE, cfg, &gain, 1);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfigure failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "reconfigured: %lu Hz, SF%u, BW %lu Hz, gain %s",
                 (unsigned long)cfg->freq_hz, (unsigned)cfg->sf,
                 (unsigned long)cfg->bw_hz, rx_boosted ? "boosted" : "saving");
    }

    return err;
}

void link_radio_suspend(void)
{
    request(REQ_SUSPEND, NULL, NULL, 0);
}

esp_err_t link_radio_resume(void)
{
    const esp_err_t err = request(REQ_RESUME, NULL, NULL, 0);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "resume failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t link_radio_tx_submit(const uint8_t *payload, size_t len)
{
    if (payload == NULL || len == 0 || len > SX1262_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    return request(REQ_TX, NULL, payload, len);
}

void link_radio_reset_stats(void)
{
    if (s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_state.rx_count = 0;
    s_state.crc_errors = 0;
    memset(s_state.entry, 0, sizeof(s_state.entry));
    s_state.entries = 0;
    memset(&s_state.last, 0, sizeof(s_state.last));

    xSemaphoreGive(s_lock);
}

void link_radio_get(link_radio_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

int32_t link_radio_snr_limit_tenths(uint8_t sf)
{
    /* SX1262 datasheet, SF5 to SF12. */
    static const int16_t LIMIT[8] = {-25, -50, -75, -100, -125, -150, -175,
                                     -200};
    return (sf >= 5 && sf <= 12) ? LIMIT[sf - 5] : -75;
}

int32_t link_radio_margin_tenths(const link_radio_frame_t *f)
{
    const int32_t limit = link_radio_snr_limit_tenths(f->radio.sf);
    return ((int32_t)f->snr_qdb * 10 - limit * 4) / 4;
}

int32_t link_radio_snr_tenths(const link_radio_frame_t *f)
{
    const int32_t n = (int32_t)f->snr_qdb * 10;
    return (n >= 0 ? n + 2 : n - 2) / 4;
}

void link_radio_rx_counters(uint32_t *out_rx, uint32_t *out_crc)
{
    /* Callable before link_radio_start(): the signal sampler may run on
     * a board with no module fitted, where the lock was never made. */
    if (s_lock == NULL) {
        if (out_rx != NULL) {
            *out_rx = 0;
        }
        if (out_crc != NULL) {
            *out_crc = 0;
        }
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_rx != NULL) {
        *out_rx = s_state.rx_seq;
    }
    if (out_crc != NULL) {
        *out_crc = s_state.crc_seq;
    }
    xSemaphoreGive(s_lock);
}
