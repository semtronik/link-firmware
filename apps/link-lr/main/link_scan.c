#include "link_scan.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sx1262_radio.h"

static const char *TAG = "link_scan";

/*
 * Settling allowed after a retune, before the channel is read.
 *
 * This is what sets the pace: it is paid on every channel, so it alone
 * decides how long a pass takes and therefore the shortest frame the
 * sweep can see. The PLL relocks in well under this; what is being
 * waited on is the AGC converging, and reading before it has converged
 * returns the bottom of the scale, which reads as a dead channel.
 *
 * The first number to revisit if short frames are being missed, or if
 * channels read implausibly low.
 */
#define SETTLE_US 1000

/* Readings averaged per channel per pass. More than one so a single
 * quantisation step or spur does not decide a channel, few enough that
 * they cost less than the settling above. */
#define READS_PER_CHANNEL 4

/*
 * Channels read between one tick given back to the rest of the system.
 *
 * The sweep settles on every channel with a busy wait - microseconds,
 * too short to block on - so a whole pass is solid CPU. The UI sits
 * above it and preempts it anyway; the tick of sleep is for what sits
 * below, the idle task and its watchdog among them. taskYIELD() would
 * not do: it only yields to equal or higher priority.
 *
 * The cost is that a tick is 10 ms against roughly one millisecond of
 * work per channel, so sleeping too often would stretch a pass by an
 * order of magnitude and with it the shortest frame the sweep can see.
 * Sixteen channels of work per tick keeps a 57-channel pass near 70 ms.
 */
#define SLEEP_EVERY_CHANNELS 16

static const uint32_t s_bw_hz[LINK_SCAN_BW_COUNT] = {
    [LINK_SCAN_BW_500K] = 500000U,
    [LINK_SCAN_BW_250K] = 250000U,
    [LINK_SCAN_BW_125K] = 125000U,
};

static SemaphoreHandle_t s_lock;
static link_scan_state_t s_state;
static TaskHandle_t s_task;
static volatile bool s_stop;

/* Aperture the task should be sweeping at. Written from the UI task,
 * read by the sweep task, which reconfigures when it differs from what
 * it last applied. */
static volatile link_scan_bw_t s_want_bw;

/* Set from the UI task to have the sweep drop its measurements at the
 * top of the next pass. Done there rather than under the lock from the
 * caller so a reset can never land midway through a pass and leave
 * half the band describing the run before it. */
static volatile bool s_want_reset;

uint32_t link_scan_bw_hz(link_scan_bw_t bw)
{
    return bw < LINK_SCAN_BW_COUNT ? s_bw_hz[bw] : s_bw_hz[LINK_SCAN_BW_500K];
}

uint16_t link_scan_channels(link_scan_bw_t bw)
{
    return (uint16_t)(((LINK_SCAN_TO_HZ - LINK_SCAN_FROM_HZ)
                       / link_scan_bw_hz(bw))
                      + 1);
}

uint32_t link_scan_channel_hz(link_scan_bw_t bw, uint16_t channel)
{
    return LINK_SCAN_FROM_HZ + (uint32_t)channel * link_scan_bw_hz(bw);
}

/* Clears the measurements, keeping what describes the run. */
static void forget_measurements(void)
{
    memset(s_state.chan_max, 0, sizeof(s_state.chan_max));
    memset(s_state.chan_min, 0, sizeof(s_state.chan_min));
    memset(s_state.chan_measured, 0, sizeof(s_state.chan_measured));
    s_state.passes = 0;
    s_state.hits = 0;
    s_state.last_hit_ms = 0;
    s_state.have_peak = false;
    s_state.peak_channel = 0;
}

/*
 * Configure for an aperture and arm the receiver.
 *
 * Paid once per aperture change, not per channel - which is the whole
 * reason the sweep can be fast. Channels within the band are reached by
 * sx1262_radio_retune() instead, a single PLL write.
 */
static bool configure_and_arm(link_scan_bw_t bw)
{
    /*
     * Built from the driver defaults, never from the stored settings.
     * Measuring energy demodulates nothing, so the spreading factor,
     * coding rate and sync word are irrelevant here - and inheriting
     * them would make this fail, silently, whenever the radio is
     * configured in a way sx1262_radio_configure() rejects. The point
     * is to work when nothing about the link is known.
     */
    sx1262_radio_cfg_t cfg;
    sx1262_radio_default_cfg(&cfg);
    cfg.freq_hz = LINK_SCAN_FROM_HZ;
    cfg.bw_hz = link_scan_bw_hz(bw);

    esp_err_t err = sx1262_radio_configure(&cfg);
    if (err == ESP_OK) {
        err = sx1262_radio_rx_start();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure at BW %lu Hz failed: %s",
                 (unsigned long)cfg.bw_hz, esp_err_to_name(err));
        return false;
    }

    /* The AGC needs far longer to converge from cold than it does after
     * a retune within the band. Paid once, here. */
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "sweeping %lu-%lu Hz, BW %lu Hz, %u channels",
             (unsigned long)LINK_SCAN_FROM_HZ, (unsigned long)LINK_SCAN_TO_HZ,
             (unsigned long)cfg.bw_hz, (unsigned)link_scan_channels(bw));
    return true;
}

/*
 * Let a floor follow the noise: straight down to a new trough, and a dB
 * at a time back up while none arrives. Decays on pass boundaries, so
 * the rate is in passes rather than readings and does not change when
 * the aperture changes the number of channels.
 */
static void floor_track(int16_t *floor, int16_t dbm, bool decay_due)
{
    if (dbm < *floor) {
        *floor = dbm;
    } else if (decay_due) {
        (*floor)++;
    }
}

/*
 * The same, mirrored, for a peak: straight up to a new high, and slowly
 * back down while none arrives.
 *
 * A plain max hold would be the obvious choice - it is what catches a
 * burst and keeps it - but held forever it also catches the loudest
 * crest of the noise, and every channel has one. After a few seconds
 * every bar has risen to its own noise crest, they all stand the same
 * height above their floors, and the profile is a solid block again.
 * Decaying, a crest fades over a few tens of seconds while a real burst
 * is renewed each time the device is triggered.
 */
static void peak_track(int16_t *peak, int16_t dbm, bool decay_due)
{
    if (dbm > *peak) {
        *peak = dbm;
    } else if (decay_due) {
        (*peak)--;
    }
}

/* Fold one channel's readings into the profile. */
static void publish_channel(uint16_t channel, int16_t dbm, bool decay_due,
                            bool peak_decay_due)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_state.chan_measured[channel]) {
        s_state.chan_max[channel] = dbm;
        s_state.chan_min[channel] = dbm;
        s_state.chan_measured[channel] = true;
    } else {
        peak_track(&s_state.chan_max[channel], dbm, peak_decay_due);
        floor_track(&s_state.chan_min[channel], dbm, decay_due);

        /* Two traces decaying towards each other can cross on a channel
         * that has gone quiet, which would show as a negative bar. */
        if (s_state.chan_max[channel] < s_state.chan_min[channel]) {
            s_state.chan_max[channel] = s_state.chan_min[channel];
        }

        if (dbm > LINK_SCAN_HIT_DBM) {
            s_state.hits++;
            s_state.last_hit_ms = now_ms;
        }
    }

    xSemaphoreGive(s_lock);
}

/* Recompute which channel stands highest over its own floor. Done once
 * per pass rather than per channel: it reads the whole profile, and the
 * answer only means anything against a complete one. */
static void publish_peak(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Simply the strongest channel, in dBm. Not the one standing
     * furthest over its own floor, which would rank a quiet channel
     * with a small bump above a loud one sitting in noise - and which
     * the profile, now drawn on one absolute scale, would contradict. */
    int16_t best = 0;
    bool found = false;
    uint16_t at = 0;

    for (uint16_t i = 0; i < s_state.channels; i++) {
        if (!s_state.chan_measured[i]) {
            continue;
        }
        if (!found || s_state.chan_max[i] > best) {
            best = s_state.chan_max[i];
            at = i;
            found = true;
        }
    }

    s_state.have_peak = found;
    s_state.peak_channel = at;
    s_state.passes++;

    xSemaphoreGive(s_lock);
}

/* Read one channel, averaged, after letting it settle. */
static bool read_channel(uint32_t freq_hz, int16_t *out_dbm)
{
    if (sx1262_radio_retune(freq_hz) != ESP_OK) {
        return false;
    }

    esp_rom_delay_us(SETTLE_US);

    int32_t sum = 0;
    int n = 0;
    for (int i = 0; i < READS_PER_CHANNEL; i++) {
        int16_t dbm;
        if (sx1262_radio_rssi_inst(&dbm) == ESP_OK) {
            sum += dbm;
            n++;
        }
    }

    if (n == 0) {
        return false;
    }

    *out_dbm = (int16_t)(sum / n);
    return true;
}

static void scan_task(void *arg)
{
    (void)arg;

    link_scan_bw_t armed = LINK_SCAN_BW_COUNT;
    uint32_t pass = 0;

    while (!s_stop) {
        const link_scan_bw_t bw = s_want_bw;

        if (armed != bw) {
            const bool ok = configure_and_arm(bw);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_state.tune_failed = !ok;
            if (ok) {
                s_state.bw = bw;
                s_state.bw_hz = link_scan_bw_hz(bw);
                s_state.channels = link_scan_channels(bw);
                forget_measurements();
            }
            xSemaphoreGive(s_lock);

            if (!ok) {
                /* Retrying immediately would spin on the bus; the user
                 * can still change aperture or leave the screen. */
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            armed = bw;
            pass = 0;
        }

        if (s_want_reset) {
            s_want_reset = false;

            /*
             * Rearm, not just clear the numbers.
             *
             * The receiver carries state the arrays do not: the AGC has
             * settled on whatever it has been hearing, a strong nearby
             * transmission included, and it comes back down slowly. A
             * reset that only zeroed the arrays measured the same band
             * through a receiver still turned down, and the floor came
             * out visibly higher than after a bandwidth change - which
             * reconfigures, and so starts the AGC afresh. Same path
             * here, so a reset means the same thing either way.
             */
            if (!configure_and_arm(bw)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_state.tune_failed = true;
                xSemaphoreGive(s_lock);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            forget_measurements();
            xSemaphoreGive(s_lock);
            pass = 0;
        }

        const uint16_t channels = link_scan_channels(bw);
        const bool decay_due = (pass % LINK_SCAN_FLOOR_DECAY_PASSES)
                               == LINK_SCAN_FLOOR_DECAY_PASSES - 1;
        const bool peak_decay_due = (pass % LINK_SCAN_PEAK_DECAY_PASSES)
                                    == LINK_SCAN_PEAK_DECAY_PASSES - 1;

        for (uint16_t ch = 0; ch < channels && !s_stop; ch++) {
            int16_t dbm;
            if (read_channel(link_scan_channel_hz(bw, ch), &dbm)) {
                publish_channel(ch, dbm, decay_due, peak_decay_due);
            }

            if ((ch % SLEEP_EVERY_CHANNELS) == SLEEP_EVERY_CHANNELS - 1) {
                vTaskDelay(1);
            }

            /* An aperture change mid-pass would mix channels measured
             * through two different filters into one profile. */
            if (s_want_bw != bw) {
                break;
            }
        }

        if (s_want_bw == bw && !s_stop) {
            publish_peak();
            pass++;
        }

        /* One tick per pass, so the sweep cannot starve the UI however
         * short a pass becomes. */
        vTaskDelay(1);
    }

    sx1262_radio_standby();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.running = false;
    xSemaphoreGive(s_lock);

    /* Cleared last: link_scan_stop() waits on it to know the radio is
     * free again. */
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t link_scan_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_task != NULL) {
        return ESP_OK;
    }

    const link_scan_bw_t bw = LINK_SCAN_BW_500K;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_state, 0, sizeof(s_state));
    s_state.bw = bw;
    s_state.bw_hz = link_scan_bw_hz(bw);
    s_state.channels = link_scan_channels(bw);
    s_state.running = true;
    xSemaphoreGive(s_lock);

    s_want_bw = bw;
    s_want_reset = false;
    s_stop = false;

    /*
     * Below the UI task, which runs at 4.
     *
     * The sweep is a busy loop and the screen is what the operator is
     * reading; above the UI, a redraw would wait on it whenever both
     * cores are busy. Nothing here is time-critical in the scheduling sense -
     * a pass that takes a little longer under UI load costs a little
     * sensitivity to very short frames, and nothing else.
     */
    if (xTaskCreate(scan_task, "radio_scan", 4096, NULL, 3, &s_task)
        != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.running = false;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void link_scan_stop(void)
{
    if (s_task == NULL) {
        return;
    }

    s_stop = true;

    for (int i = 0; i < 200 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_task != NULL) {
        ESP_LOGW(TAG, "scan task still running");
    }
}

void link_scan_reset(void)
{
    s_want_reset = true;
}

void link_scan_cycle_bw(void)
{
    if (s_lock == NULL) {
        return;
    }

    /* Only the request is published here. The task clears and
     * reconfigures, which keeps every write to the profile on one
     * thread and the aperture consistent with the readings in it. */
    s_want_bw = (link_scan_bw_t)((s_want_bw + 1) % LINK_SCAN_BW_COUNT);
}

void link_scan_get(link_scan_state_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}
