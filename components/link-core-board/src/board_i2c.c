/**
 * @file board_i2c.c
 * @brief I2C bus recovery for the link boards.
 */

#include "board_i2c.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "board_i2c";

/*
 * Nine edges: eight for the data bits a slave may still be shifting out,
 * plus one for the ACK slot it is waiting on.
 */
#define RECOVERY_CLOCKS 9

/* 100 kHz-ish. Slow is fine here and works with weak pull-ups. */
#define HALF_PERIOD_US 5

/*
 * Both lines are driven open-drain: the master only ever pulls low and
 * releases, never drives high. Driving a line high against a slave that
 * is holding it low would be a short between two outputs.
 */
static esp_err_t configure_open_drain(gpio_num_t scl, gpio_num_t sda)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << scl) | (1ULL << sda),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        /* The board pulls both lines up. Adding the internal ones on top
         * would only put an ill-defined resistance in parallel with a
         * known one. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool board_i2c_bus_is_idle(gpio_num_t scl, gpio_num_t sda)
{
    return gpio_get_level(scl) == 1 && gpio_get_level(sda) == 1;
}

/*
 * Hand the pins back before the I2C driver claims them: since 6.0 it
 * reserves SDA/SCL and warns when it finds them already configured. The
 * pull-ups this re-enables sit in parallel with the board's own for the
 * few microseconds before the driver reconfigures both lines.
 */
static void release_pins(gpio_num_t scl, gpio_num_t sda)
{
    gpio_reset_pin(scl);
    gpio_reset_pin(sda);
}

esp_err_t board_i2c_bus_recover(gpio_num_t scl, gpio_num_t sda)
{
    esp_err_t err = configure_open_drain(scl, sda);
    if (err != ESP_OK) {
        return err;
    }

    /* Release both lines and let the pull-ups settle before looking. */
    gpio_set_level(scl, 1);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(HALF_PERIOD_US);

    if (board_i2c_bus_is_idle(scl, sda)) {
        release_pins(scl, sda);
        return ESP_OK;
    }

    if (gpio_get_level(scl) == 0) {
        /* A slave holding SCL low cannot be clocked out of it: the
         * master has no way to advance a bus it cannot drive. */
        ESP_LOGE(TAG, "SCL held low, bus cannot be recovered by clocking");
        release_pins(scl, sda);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "SDA held low, clocking %d pulses to release it", RECOVERY_CLOCKS);

    for (int i = 0; i < RECOVERY_CLOCKS; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(HALF_PERIOD_US);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(HALF_PERIOD_US);

        if (gpio_get_level(sda) == 1) {
            break;
        }
    }

    /* STOP: SDA rises while SCL is high. Without it a slave may still
     * consider a transfer open. */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(HALF_PERIOD_US);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(HALF_PERIOD_US);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(HALF_PERIOD_US);

    if (!board_i2c_bus_is_idle(scl, sda)) {
        ESP_LOGE(TAG, "bus still stuck after recovery (SCL=%d SDA=%d)",
                 gpio_get_level(scl), gpio_get_level(sda));
        release_pins(scl, sda);
        return ESP_ERR_INVALID_STATE;
    }

    release_pins(scl, sda);
    ESP_LOGI(TAG, "bus recovered");
    return ESP_OK;
}
