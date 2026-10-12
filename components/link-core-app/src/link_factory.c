#include "link_factory.h"

#include "esp_log.h"
#include "esp_system.h"
#include "link_events.h"
#include "link_time.h"
#include "nvs_flash.h"

static const char *TAG = "link_factory";

static void (*s_app_erase)(void);

void link_factory_set_app_erase(void (*cb)(void))
{
    s_app_erase = cb;
}

void link_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");

    /* The clock first: with it unset, no recording session can open
     * between the application's erase and the restart. */
    if (link_time_invalidate() != ESP_OK) {
        ESP_LOGE(TAG, "clock not cleared");
    }

    if (s_app_erase != NULL) {
        s_app_erase();
    }

    link_events_erase();

    const esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS not erased: %s", esp_err_to_name(err));
    }

    esp_restart();
}
