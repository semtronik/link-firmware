#include "power_key.h"

#include <stdatomic.h>
#include <stdio.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_events.h"

static const char *TAG = "power_key";

#define POLL_MS      50U
#define HOLD_SAMPLES (POWER_KEY_HOLD_MS / POLL_MS)

static unsigned s_latch_ms;

/* Written by the watcher task, read by an lv_timer on the LVGL task. */
static atomic_bool s_request;

void power_key_latch(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PWR_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(BOARD_PIN_PWR_HOLD, 1);

    /* esp_timer counts from power-on, not from app_main. */
    s_latch_ms = (unsigned)(esp_timer_get_time() / 1000);
}

unsigned power_key_latch_delay_ms(void)
{
    return s_latch_ms;
}

/*
 * The confirmation opens on the threshold rather than on release: the
 * user gets the screen while still holding, and lets go to answer it.
 * Nothing is cut here - power_key_power_off() runs from the overlay.
 */
static void watch_task(void *arg)
{
    (void)arg;
    unsigned held = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        /* Active low: the button pulls to ground. */
        if (gpio_get_level(BOARD_PIN_PWR_KEY) != 0) {
            held = 0;
            continue;
        }

        if (++held == HOLD_SAMPLES) {
            ESP_LOGI(TAG, "long press");
            atomic_store(&s_request, true);
        }
    }
}

void power_key_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PWR_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(watch_task, "power_key", 3072, NULL, 5, NULL);
}

bool power_key_take_request(void)
{
    return atomic_exchange(&s_request, false);
}

static void (*s_before_off)(const char *why);

void power_key_set_before_off(void (*cb)(const char *why))
{
    s_before_off = cb;
}

void power_key_power_off(const char *why)
{
    if (s_before_off != NULL) {
        s_before_off(why);
    }

    ESP_LOGW(TAG, "%s: releasing PWR_HOLD", why);

    /* Written before the rail is dropped. link_events closes the file
     * on every append, so the line is on flash by the time this
     * returns. */
    char detail[48];
    snprintf(detail, sizeof(detail), "reason=%s", why ? why : "unknown");
    link_events_append("SHUTDOWN", detail);

    /* Let the log drain while the board is still alive. */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(BOARD_PIN_PWR_HOLD, 0);

    /* With USB in, the charger holds /PGOOD low and the board survives. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(TAG, "still running - USB is forcing the latch");
}
