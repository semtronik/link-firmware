/**
 * @file board_rtc.c
 * @brief PCF8563 real-time clock on the core I2C bus.
 */

#include "board_rtc.h"

#include <string.h>

#include "board_core_bus.h"
#include "esp_log.h"

static const char *TAG = "board_rtc";

/*
 * Register map, from the seconds register onwards. The seven time
 * registers are contiguous, so one burst read starting at 0x02 gets the
 * whole calendar.
 */
#define REG_SECONDS 0x02

/*
 * CLKOUT_control. Bit 7 (FE) gates the output, bits 1:0 (FD) pick the
 * frequency. The chip comes out of reset with FE set and FD at 00, so
 * the pin is already running at 32.768 kHz before anything writes here.
 */
#define REG_CLKOUT_CONTROL 0x0D

#define CLKOUT_FE_MASK  0x80
#define CLKOUT_FD_32768 0x00
#define CLKOUT_FD_1024  0x01
#define CLKOUT_FD_32    0x02
#define CLKOUT_FD_1     0x03

/*
 * Bit 7 of the seconds register is VL (Voltage Low), not part of the BCD
 * value. The chip sets it whenever its supply drops below the internal
 * threshold and only ever clears it on a write. It is therefore the one
 * honest answer to "has this clock been running continuously since I set
 * it?" - the time field alone always looks plausible.
 */
#define SECONDS_VL_MASK 0x80

/* Each field carries a different number of BCD digits; the unused top
 * bits hold flags (century, alarm) that must be masked off. */
#define MASK_SECONDS 0x7F
#define MASK_MINUTES 0x7F
#define MASK_HOURS   0x3F
#define MASK_DAY     0x3F
#define MASK_MONTH   0x1F

/* The PCF8563 stores a two-digit year; the month register's top bit is
 * the century flag. This board has no use for dates before 2000, so the
 * epoch is fixed rather than tracked. */
#define RTC_YEAR_EPOCH 2000

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static uint8_t bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

esp_err_t board_rtc_get_time(struct tm *out, bool *low_volt)
{
    if (out == NULL || low_volt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rx[7] = {0};

    const esp_err_t err =
        board_core_bus_read(BOARD_RTC_ADDR, REG_SECONDS, rx, sizeof(rx));
    if (err != ESP_OK) {
        return err;
    }

    *low_volt = (rx[0] & SECONDS_VL_MASK) != 0;

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd_to_bin(rx[0] & MASK_SECONDS);
    out->tm_min  = bcd_to_bin(rx[1] & MASK_MINUTES);
    out->tm_hour = bcd_to_bin(rx[2] & MASK_HOURS);
    out->tm_mday = bcd_to_bin(rx[3] & MASK_DAY);
    /* rx[4] is the weekday, which the chip maintains independently of the
     * date. It is deliberately ignored: mktime() derives it from the
     * calendar fields, and a weekday that disagrees with the date would
     * otherwise silently win. */
    out->tm_mon  = bcd_to_bin(rx[5] & MASK_MONTH) - 1;   /* tm_mon is 0-11 */
    out->tm_year = bcd_to_bin(rx[6]) + (RTC_YEAR_EPOCH - 1900);
    out->tm_isdst = -1;

    return ESP_OK;
}

esp_err_t board_rtc_set_time(const struct tm *t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int year = t->tm_year + 1900;
    if (year < RTC_YEAR_EPOCH || year > RTC_YEAR_EPOCH + 99) {
        ESP_LOGE(TAG, "year %d outside the %d-%d range the chip can store",
                 year, RTC_YEAR_EPOCH, RTC_YEAR_EPOCH + 99);
        return ESP_ERR_INVALID_ARG;
    }

    /* Writing the seconds register with bit 7 clear is what clears VL. */
    const uint8_t tx[7] = {
        bin_to_bcd((uint8_t)t->tm_sec),
        bin_to_bcd((uint8_t)t->tm_min),
        bin_to_bcd((uint8_t)t->tm_hour),
        bin_to_bcd((uint8_t)t->tm_mday),
        /* Weekday: the chip does not derive it, so store what the caller
         * computed. mktime() fills tm_wday in. */
        (uint8_t)(t->tm_wday & 0x07),
        bin_to_bcd((uint8_t)(t->tm_mon + 1)),
        bin_to_bcd((uint8_t)(year - RTC_YEAR_EPOCH)),
    };

    return board_core_bus_write(BOARD_RTC_ADDR, REG_SECONDS, tx, sizeof(tx));
}

esp_err_t board_rtc_set_clkout(board_rtc_clkout_t freq)
{
    uint8_t reg;

    switch (freq) {
    /* FE clear is what releases the pin; the FD bits stop mattering. */
    case BOARD_RTC_CLKOUT_OFF:
        reg = 0x00;
        break;
    case BOARD_RTC_CLKOUT_32768HZ:
        reg = CLKOUT_FE_MASK | CLKOUT_FD_32768;
        break;
    case BOARD_RTC_CLKOUT_1024HZ:
        reg = CLKOUT_FE_MASK | CLKOUT_FD_1024;
        break;
    case BOARD_RTC_CLKOUT_32HZ:
        reg = CLKOUT_FE_MASK | CLKOUT_FD_32;
        break;
    case BOARD_RTC_CLKOUT_1HZ:
        reg = CLKOUT_FE_MASK | CLKOUT_FD_1;
        break;
    default:
        ESP_LOGE(TAG, "unknown CLKOUT frequency %d", (int)freq);
        return ESP_ERR_INVALID_ARG;
    }

    return board_core_bus_write(BOARD_RTC_ADDR, REG_CLKOUT_CONTROL, &reg,
                                sizeof(reg));
}
