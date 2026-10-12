#include "link_time.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "board_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "link_time";

/*
 * How often the system clock is pulled back to the RTC. The ESP32
 * oscillator is good to a few seconds a day, so five minutes keeps the
 * error to the order of tens of milliseconds - well under the second
 * the screens display, and cheap: one I2C read per interval.
 */
#define RESYNC_INTERVAL_MS (5 * 60 * 1000)

/* Shared with link_prefs: the offset belongs to the board, not to
 * whichever app is flashed on it. */
#define NVS_NAMESPACE  "link"
#define NVS_KEY_OFFSET "tz_offset"

/*
 * Broken-down UTC to epoch seconds.
 *
 * timegm() is not in the newlib ESP-IDF ships, and mktime() is the
 * wrong function here: it reads its argument as local time and shifts
 * it by whatever zone the C library thinks it is in. The arithmetic is
 * short enough to do outright - days since 1970, then seconds.
 */
static time_t utc_to_epoch(const struct tm *t)
{
    static const int16_t days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
    };

    const int year = t->tm_year + 1900;
    const int month = t->tm_mon;

    int32_t days = (year - 1970) * 365;

    /* Leap days between 1970 and the start of this year. */
    days += (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;

    days += days_before_month[month];

    /* This year's own leap day, once March is reached. */
    const bool leap =
        (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (leap && month >= 2) {
        days++;
    }

    days += t->tm_mday - 1;

    return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60
           + t->tm_sec;
}

/*
 * The RTC holds UTC, and the offset is applied on the way out. Keeping
 * UTC as what is stored means a change of offset - a border crossing,
 * daylight saving - moves the display without touching the clock, and
 * without making already-stamped frames shift underneath.
 */
static bool s_valid;
static int16_t s_offset_min;
static int64_t s_next_sync_ms;

static void offset_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    int16_t stored = 0;
    if (nvs_get_i16(handle, NVS_KEY_OFFSET, &stored) == ESP_OK) {
        s_offset_min = stored;
    }
    nvs_close(handle);
}

/* Reads the RTC and moves the system clock onto it. */
static bool sync_from_rtc(void)
{
    struct tm utc;
    bool low_volt = false;

    if (board_rtc_get_time(&utc, &low_volt) != ESP_OK) {
        return false;
    }

    /* The transfer worked but the chip lost power, so what it holds is
     * a plausible time that means nothing. */
    if (low_volt) {
        return false;
    }

    /* What a factory reset leaves, or a chip that lost track. */
    if (utc.tm_year + 1900 < LINK_TIME_VALID_FROM_YEAR) {
        return false;
    }

    const struct timeval tv = {.tv_sec = utc_to_epoch(&utc), .tv_usec = 0};
    settimeofday(&tv, NULL);

    return true;
}

esp_err_t link_time_init(void)
{
    offset_load();

    s_valid = sync_from_rtc();
    s_next_sync_ms = (esp_timer_get_time() / 1000) + RESYNC_INTERVAL_MS;

    if (s_valid) {
        char buf[20];
        link_time_format_ms(link_time_now_ms(), buf, sizeof(buf));
        ESP_LOGI(TAG, "clock set from RTC: %s (UTC%+d min)", buf,
                 (int)s_offset_min);
    } else {
        ESP_LOGW(TAG, "RTC has no time to give - clock not set");
    }

    return ESP_OK;
}

void link_time_tick(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;

    if (now_ms < s_next_sync_ms) {
        return;
    }
    s_next_sync_ms = now_ms + RESYNC_INTERVAL_MS;

    /* A failed re-sync leaves the clock where it is: drifting slowly is
     * better than jumping to nothing. */
    if (sync_from_rtc()) {
        s_valid = true;
    }
}

bool link_time_is_valid(void)
{
    return s_valid;
}

esp_err_t link_time_invalidate(void)
{
    s_valid = false;

    const struct tm unset = {.tm_year = 2000 - 1900, .tm_mday = 1};
    return board_rtc_set_time(&unset);
}

time_t link_time_get_utc(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return tv.tv_sec;
}

static void (*s_before_set)(void);

void link_time_set_before_set(void (*cb)(void))
{
    s_before_set = cb;
}

esp_err_t link_time_set_utc(time_t utc)
{
    if (s_before_set != NULL) {
        s_before_set();
    }

    struct tm utc_tm;
    gmtime_r(&utc, &utc_tm);

    /* The RTC first: it is the reference, and the write is what clears
     * its low-voltage flag. */
    const esp_err_t err = board_rtc_set_time(&utc_tm);
    if (err != ESP_OK) {
        return err;
    }

    const struct timeval tv = {.tv_sec = utc, .tv_usec = 0};
    settimeofday(&tv, NULL);

    s_valid = true;
    s_next_sync_ms = (esp_timer_get_time() / 1000) + RESYNC_INTERVAL_MS;

    return ESP_OK;
}

esp_err_t link_time_set(const struct tm *local)
{
    /* What the screens give is local, and what goes in is UTC. */
    return link_time_set_utc(utc_to_epoch(local) - s_offset_min * 60);
}

int16_t link_time_get_offset_minutes(void)
{
    return s_offset_min;
}

esp_err_t link_time_set_offset_minutes(int16_t minutes)
{
    s_offset_min = minutes;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i16(handle, NVS_KEY_OFFSET, minutes);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

void link_time_get_local(struct tm *out)
{
    const time_t local = link_time_get_utc() + s_offset_min * 60;
    gmtime_r(&local, out);
}

int64_t link_time_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void link_time_format_ms(int64_t ts_ms, char *buf, unsigned size)
{
    if (ts_ms <= 0 || !s_valid) {
        snprintf(buf, size, "--/--/-- --:--:--");
        return;
    }

    /* Stamps are UTC; the offset turns them into what the user reads. */
    const time_t local = (time_t)(ts_ms / 1000) + s_offset_min * 60;
    struct tm t;
    gmtime_r(&local, &t);

    snprintf(buf, size, "%02d/%02d/%02d %02d:%02d:%02d", t.tm_mday,
             t.tm_mon + 1, (t.tm_year + 1900) % 100, t.tm_hour, t.tm_min,
             t.tm_sec);
}
