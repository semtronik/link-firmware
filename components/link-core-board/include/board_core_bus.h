/**
 * @file board_core_bus.h
 * @brief The core I2C bus: RTC and fuel gauge.
 *
 * One owner for the bus, because it has several users - the RTC, the
 * fuel gauge, a console command, a screen refresh - and they run on
 * different tasks. Everything reaching the bus goes through here, so
 * the lock and the recovery live in one place instead of being
 * reimplemented by each caller.
 *
 * Bring it up once, early:
 *   board_core_bus_init();
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Port 1 is left to the touch panel, which claims it for the whole run. */
#define BOARD_CORE_I2C_PORT 0

/**
 * @brief Claim the bus and install the driver.
 *
 * Recovers a wedged bus before claiming the pins: a slave that lost
 * power mid-transfer can still be holding SDA low, which would make
 * every later transaction fail for a reason unrelated to the slave.
 *
 * Safe to call twice; the second call is a no-op.
 */
esp_err_t board_core_bus_init(void);

/** @brief True once board_core_bus_init() has succeeded. */
bool board_core_bus_ready(void);

/**
 * @brief Read registers from a device on the core bus.
 *
 * Takes the lock for the transfer. On failure the bus is checked and
 * unwedged if a slave left SDA low, so the next call starts clean.
 *
 * @param addr 7-bit device address.
 * @param reg  Register to read from.
 * @param out  Receives @p len bytes.
 * @param len  Number of bytes to read.
 */
esp_err_t board_core_bus_read(uint8_t addr, uint8_t reg, uint8_t *out, size_t len);

/**
 * @brief Write registers to a device on the core bus.
 *
 * @param addr 7-bit device address.
 * @param reg  Register to write to.
 * @param data @p len bytes to write after the register.
 * @param len  Number of bytes to write.
 */
esp_err_t board_core_bus_write(uint8_t addr, uint8_t reg, const uint8_t *data,
                               size_t len);

/**
 * @brief Probe an address.
 *
 * @return true if a device acknowledged.
 */
bool board_core_bus_probe(uint8_t addr);

/**
 * @brief Take the bus lock across several transfers.
 *
 * Only needed when a sequence must not be interleaved with another
 * task's - a read-modify-write, or a scan. Single transfers already
 * lock themselves. Recursive, so a locked caller may still use the
 * functions above.
 */
void board_core_bus_lock(void);

/** @brief Release the bus lock. */
void board_core_bus_unlock(void);

#ifdef __cplusplus
}
#endif
