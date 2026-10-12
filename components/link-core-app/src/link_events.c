#include "link_events.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wear_levelling.h"

static const char *TAG = "link_events";

#define MOUNT_POINT     "/data"
#define PARTITION_LABEL "fat"
#define LOG_PATH        MOUNT_POINT "/events.log"
#define LOG_PATH_PREV   MOUNT_POINT "/events.log.1"

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_lock;
static bool s_ready;

/*
 * ISO 8601 UTC once the clock is set, uptime before that. The cutoff is
 * "is this year plausible": an unset clock reads 1970, and a line
 * stamped that way is still useful as long as it says so.
 */
#define CLOCK_PLAUSIBLE_FROM 1577836800 /* 2020-01-01 */

static void format_timestamp(char *buf, size_t size)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    if (now.tv_sec >= CLOCK_PLAUSIBLE_FROM) {
        struct tm utc;
        gmtime_r(&now.tv_sec, &utc);
        strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
        return;
    }

    const uint32_t uptime_s =
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000U);
    snprintf(buf, size, "uptime=%" PRIu32 "s", uptime_s);
}

/* Caller holds the lock. */
static void rotate_if_needed(void)
{
    struct stat st;

    if (stat(LOG_PATH, &st) != 0 || st.st_size < LINK_EVENTS_ROTATE_BYTES) {
        return;
    }

    /* remove() then rename(): renaming onto an existing file is not
     * reliable on FAT. */
    remove(LOG_PATH_PREV);

    if (rename(LOG_PATH, LOG_PATH_PREV) == 0) {
        ESP_LOGI(TAG, "rotated at %ld bytes", (long)st.st_size);
        return;
    }

    /* Truncating loses the older half, which still beats filling the
     * partition and losing the ability to write at all. */
    ESP_LOGW(TAG, "rotate failed, truncating");
    FILE *f = fopen(LOG_PATH, "w");
    if (f != NULL) {
        fclose(f);
    }
}

esp_err_t link_events_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_vfs_fat_mount_config_t cfg = {
        /* First boot on a blank partition: there is nothing to lose by
         * formatting, and the alternative is no log at all. */
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    const esp_err_t err =
        esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PARTITION_LABEL, &cfg,
                                         &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "mounted on %s", MOUNT_POINT);

    return ESP_OK;
}

void link_events_append(const char *event, const char *details)
{
    if (!s_ready || event == NULL) {
        return;
    }

    /* Bounded: a call site logging an event must not block on a busy
     * filesystem. Dropping the line is the lesser failure. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "busy, dropped '%s'", event);
        return;
    }

    FILE *f = fopen(LOG_PATH, "a");
    if (f == NULL) {
        xSemaphoreGive(s_lock);
        return;
    }

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    if (details != NULL && *details != '\0') {
        fprintf(f, "%s %s %s\n", ts, event, details);
    } else {
        fprintf(f, "%s %s\n", ts, event);
    }
    fclose(f);

    rotate_if_needed();

    xSemaphoreGive(s_lock);
}

void link_events_dump(link_events_line_cb_t cb, void *user_data)
{
    if (!s_ready || cb == NULL) {
        return;
    }

    /* Held across both files, so a rotation cannot move them mid-read. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "busy, dump skipped");
        return;
    }

    const char *paths[] = {LOG_PATH_PREV, LOG_PATH};
    char line[256];

    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "r");
        if (f == NULL) {
            continue; /* the previous file need not exist */
        }

        while (fgets(line, sizeof(line), f) != NULL) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = '\0';
            }

            if (n == 0) {
                continue;
            }

            if (!cb(line, user_data)) {
                fclose(f);
                xSemaphoreGive(s_lock);
                return;
            }
        }

        fclose(f);
    }

    xSemaphoreGive(s_lock);
}

void link_events_erase(void)
{
    if (!s_ready) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    remove(LOG_PATH_PREV);
    remove(LOG_PATH);
    s_ready = false;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "erased");
}
