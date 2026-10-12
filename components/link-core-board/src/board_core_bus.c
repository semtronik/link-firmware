/**
 * @file board_core_bus.c
 * @brief The core I2C bus: RTC and fuel gauge.
 */

#include "board_core_bus.h"

#include "board_i2c.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board_core_bus";

#define CORE_I2C_FREQ_HZ 100000
#define I2C_TIMEOUT_MS   100

/* RTC and fuel gauge. Raise if the core bus gains a device. */
#define CORE_MAX_DEVICES 4

/* Shared so recovery can rebuild the bus identically. */
static const i2c_master_bus_config_t s_cfg = {
    .i2c_port = BOARD_CORE_I2C_PORT,
    .sda_io_num = BOARD_PIN_CORE_SDA,
    .scl_io_num = BOARD_PIN_CORE_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    /* The board has external pull-ups; internal ones would only weaken
     * the edges. */
    .flags.enable_internal_pullup = 0,
};

static SemaphoreHandle_t s_lock = NULL;
static bool s_ready = false;

static i2c_master_bus_handle_t s_bus = NULL;

/*
 * The new driver addresses a device handle, not a port plus an address
 * on every transfer, so each address used is turned into a handle once
 * and kept. Two entries would do today; the cache spares the callers
 * having to know that.
 */
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} s_devs[CORE_MAX_DEVICES];
static size_t s_dev_count = 0;

/* Call with the lock held. */
static esp_err_t dev_for(uint8_t addr, i2c_master_dev_handle_t *out)
{
    for (size_t i = 0; i < s_dev_count; i++) {
        if (s_devs[i].addr == addr) {
            *out = s_devs[i].dev;
            return ESP_OK;
        }
    }

    if (s_dev_count == CORE_MAX_DEVICES) {
        ESP_LOGE(TAG, "device cache full, raise CORE_MAX_DEVICES");
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = CORE_I2C_FREQ_HZ,
    };

    i2c_master_dev_handle_t dev = NULL;
    const esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }

    s_devs[s_dev_count].addr = addr;
    s_devs[s_dev_count].dev = dev;
    s_dev_count++;

    *out = dev;
    return ESP_OK;
}

/* Call with the lock held. Handles die with the bus they came from. */
static void forget_devices(void)
{
    s_dev_count = 0;
}

/*
 * Created on the first lock rather than by init: a caller that reaches
 * the bus before init would otherwise find a NULL lock and go through
 * unserialised, which is exactly the failure this module exists to
 * prevent.
 */
void board_core_bus_lock(void)
{
    if (s_lock == NULL) {
        static portMUX_TYPE spin = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&spin);
        if (s_lock == NULL) {
            s_lock = xSemaphoreCreateRecursiveMutex();
        }
        portEXIT_CRITICAL(&spin);
        assert(s_lock != NULL);
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void board_core_bus_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

bool board_core_bus_ready(void)
{
    return s_ready;
}

esp_err_t board_core_bus_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    board_core_bus_lock();

    if (board_i2c_bus_recover(BOARD_PIN_CORE_SCL, BOARD_PIN_CORE_SDA) != ESP_OK) {
        ESP_LOGW(TAG, "bus did not come back idle, claiming it anyway");
    }

    const esp_err_t err = i2c_new_master_bus(&s_cfg, &s_bus);

    s_ready = (err == ESP_OK);
    board_core_bus_unlock();

    ESP_RETURN_ON_ERROR(err, TAG, "bus init failed");
    ESP_LOGI(TAG, "ready (SDA=%d SCL=%d)", BOARD_PIN_CORE_SDA, BOARD_PIN_CORE_SCL);
    return ESP_OK;
}

/*
 * A slave that browns out mid-byte leaves SDA low, and every later
 * transfer then fails for that reason rather than for its own. Clearing
 * it here means a caller never has to.
 *
 * Call with the lock held.
 */
static void unwedge_locked(void)
{
    if (board_i2c_bus_is_idle(BOARD_PIN_CORE_SCL, BOARD_PIN_CORE_SDA)) {
        return;
    }

    /* Deleting the bus takes its device handles with it. */
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    forget_devices();

    board_i2c_bus_recover(BOARD_PIN_CORE_SCL, BOARD_PIN_CORE_SDA);

    if (i2c_new_master_bus(&s_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "bus did not come back, transfers will fail");
        s_ready = false;
    }
}

esp_err_t board_core_bus_read(uint8_t addr, uint8_t reg, uint8_t *out, size_t len)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    board_core_bus_lock();

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = dev_for(addr, &dev);
    if (err == ESP_OK) {
        /* Timeout in ms here, not ticks: the new driver takes a plain
         * millisecond count. */
        err = i2c_master_transmit_receive(dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            unwedge_locked();
        }
    }

    board_core_bus_unlock();

    return err;
}

esp_err_t board_core_bus_write(uint8_t addr, uint8_t reg, const uint8_t *data,
                               size_t len)
{
    if ((data == NULL && len > 0) || len > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t tx[33];
    tx[0] = reg;
    for (size_t i = 0; i < len; i++) {
        tx[i + 1] = data[i];
    }

    board_core_bus_lock();

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = dev_for(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, tx, len + 1, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            unwedge_locked();
        }
    }

    board_core_bus_unlock();

    return err;
}

bool board_core_bus_probe(uint8_t addr)
{
    if (!s_ready) {
        return false;
    }

    board_core_bus_lock();
    /* Probing is a bus-level operation: no device handle is created for
     * an address that may well not answer. */
    const esp_err_t err = i2c_master_probe(s_bus, addr, 50);
    board_core_bus_unlock();

    return err == ESP_OK;
}
