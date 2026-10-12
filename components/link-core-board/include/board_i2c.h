/**
 * @file board_i2c.h
 * @brief I2C bus recovery for the link boards.
 */

#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report whether the bus is idle.
 *
 * Both lines must read high. A low SDA with a high SCL is the classic
 * stuck-slave signature.
 */
bool board_i2c_bus_is_idle(gpio_num_t scl, gpio_num_t sda);

/**
 * @brief Unwedge a bus whose SDA is held low by a slave.
 *
 * A slave that loses power or is reset while driving a data bit keeps
 * SDA low, and the master can no longer issue a START. Clocking SCL
 * lets the slave finish the byte it believes it is sending, after which
 * it releases SDA and a STOP returns the bus to idle.
 *
 * Call before creating the I2C bus, or after a transaction fails. The
 * pins are handed back to their default state on return, so the driver
 * can claim them without reporting a conflict.
 *
 * @return ESP_OK if the bus is idle on return, ESP_ERR_INVALID_STATE if
 *         SDA is still held low, or an error from the GPIO layer.
 */
esp_err_t board_i2c_bus_recover(gpio_num_t scl, gpio_num_t sda);

#ifdef __cplusplus
}
#endif
