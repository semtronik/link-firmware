#include "link_power.h"

#include <stdio.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_events.h"
#include "link_time.h"

static const char *TAG = "link_power";

/* The charger's status pin is an open collector, pulled up on the
 * board. No internal pull-up: two in parallel would only make the level
 * harder to reason about. */
#define CHG_ACTIVE_LEVEL 0

/*
 * Consecutive agreeing samples before a pin is believed. The charger
 * pulses ~CHG while it probes for a battery, so a single read catches
 * it mid-cycle and the bolt would flicker; at the 1 Hz this is polled,
 * three samples cover a ~500 ms detect cycle. PRES_USB gets the same
 * treatment - not because it is noisy, but so both signals settle over
 * the same window and the log cannot say charging started before the
 * cable went in.
 */
#define DEBOUNCE_N 3

/* Above this the cell is treated as full, so a charge that ends here is
 * a finished one rather than an interrupted one. The gauge filters its
 * own reading and rarely sits at exactly 100 when the charger lets go. */
#define FULL_PCT 95

/* A debounced input: the raw pin goes in, the settled level comes out
 * only once DEBOUNCE_N samples have agreed. */
typedef struct {
    bool level;
    unsigned high;
    unsigned low;
} debounced_t;

static bool debounce(debounced_t *d, bool raw)
{
    unsigned *agreeing = raw ? &d->high : &d->low;
    unsigned *opposing = raw ? &d->low : &d->high;

    *opposing = 0;
    if (*agreeing < DEBOUNCE_N) {
        (*agreeing)++;
    }

    if (*agreeing >= DEBOUNCE_N) {
        d->level = raw;
    }

    return d->level;
}

/* The charger's detect cycle is around 500 ms, so sampling faster than
 * that would only alias it. */
#define POLL_MS 1000

#define TASK_STACK 3072
#define TASK_PRIO  3

static link_power_t s_state = {
    .state = LINK_POWER_DISCHARGING,
};

/* Written by the sampling task, read by the screens. */
static SemaphoreHandle_t s_lock;

static debounced_t s_usb;
static debounced_t s_chg;

/* What the last written log line said, so only changes are recorded. */
static bool s_logged_usb;
static bool s_logged_charging;
static bool s_logged_init;

static void poll_once(void);
static void power_task(void *arg);

esp_err_t link_power_init(void)
{
    const gpio_config_t chg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_CHG,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&chg);
    if (err != ESP_OK) {
        return err;
    }

    /* Driven both ways by the USB detect divider, so it needs nothing. */
    const gpio_config_t usb = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PRES_USB,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&usb);
    if (err != ESP_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* One sample before the task starts, so the first screen drawn has
     * something better than the defaults to show. */
    poll_once();

    if (xTaskCreate(power_task, "power", TASK_STACK, NULL, TASK_PRIO, NULL)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ready");

    return ESP_OK;
}

static void poll_once(void)
{
    /* Both sampled in the same pass and settled over the same window,
     * so the pair is always coherent. */
    const bool usb =
        debounce(&s_usb, gpio_get_level(BOARD_PIN_PRES_USB) != 0);
    const bool charging =
        debounce(&s_chg, gpio_get_level(BOARD_PIN_CHG) == CHG_ACTIVE_LEVEL);

    /* This is the detector's clock: it advances once per call. */
    link_battery_poll(usb);

    link_battery_t bat;
    link_battery_get(&bat);

    link_power_state_t state;

    if (!usb) {
        /* No USB and the board is still running, so it is on the cell,
         * whatever the detector has settled on so far. */
        state = LINK_POWER_DISCHARGING;
    } else if (bat.presence == LINK_BATTERY_ABSENT) {
        state = LINK_POWER_NO_BATTERY;
    } else if (charging) {
        state = LINK_POWER_CHARGING;
    } else {
        /* The pin is also released while the charger has not started, so
         * this covers "done" and "waiting" alike. Either way nothing is
         * going into the cell. */
        state = LINK_POWER_CHARGED;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const link_power_state_t was = s_state.state;
    s_state.state = state;
    s_state.usb_present = usb;
    s_state.charging = charging;
    s_state.battery = bat;
    xSemaphoreGive(s_lock);

    if (state != was) {
        ESP_LOGI(TAG, "%s", link_power_state_str(state));
    }

    /*
     * Two independent records, written outside the lock: the event log
     * touches the filesystem and no screen should wait on that.
     *
     * The first poll only takes the initial reading - a board started on
     * USB would otherwise open its log with a connection that did not
     * happen. BOOT already says what the situation was.
     */
    if (!s_logged_init) {
        s_logged_init = true;
        s_logged_usb = usb;
        s_logged_charging = charging;
        return;
    }

    char detail[48];

    /*
     * USB first, always. Both signals settle over the same window, but
     * the charger can pull ~CHG on the very sample the cable is seen -
     * writing the cable first is what keeps the log readable in order.
     */
    if (usb != s_logged_usb) {
        s_logged_usb = usb;
        link_events_append("USB", usb ? "connected" : "disconnected");
    }

    if (charging == s_logged_charging) {
        return;
    }
    s_logged_charging = charging;

    if (charging) {
        snprintf(detail, sizeof(detail), "pct=%u", (unsigned)bat.soc_pct);
        link_events_append("CHARGE_START", detail);
        return;
    }

    /*
     * Why it stopped, which is the whole point of recording the end:
     *
     *   usb_lost   the cable went; the line above says so too
     *   full       still powered and the gauge is at the top
     *   stopped    still powered, not full - the charger gave up on its
     *              own: no cell, a thermal fault, a bad supply
     */
    const char *reason;
    if (!usb) {
        reason = "usb_lost";
    } else if (bat.gauge_ok && bat.soc_pct >= FULL_PCT) {
        reason = "full";
    } else {
        reason = "stopped";
    }

    snprintf(detail, sizeof(detail), "pct=%u reason=%s",
             (unsigned)bat.soc_pct, reason);
    link_events_append("CHARGE_END", detail);
}

static void power_task(void *arg)
{
    (void)arg;

    while (true) {
        poll_once();

        /* Rides along here for the same reason: it reaches the RTC over
         * the same bus, and must not do so from the LVGL thread. It only
         * transfers every few minutes and does nothing in between. */
        link_time_tick();

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void link_power_get(link_power_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

const char *link_power_state_str(link_power_state_t state)
{
    switch (state) {
    case LINK_POWER_DISCHARGING:
        return "sur batterie";
    case LINK_POWER_CHARGING:
        return "en charge";
    case LINK_POWER_CHARGED:
        return "chargee";
    case LINK_POWER_NO_BATTERY:
        return "pas de batterie";
    default:
        return "?";
    }
}
