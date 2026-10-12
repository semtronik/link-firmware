/**
 * @file sx1262.h
 * @brief SX1262 transport: SPI commands, registers, reset, DIO1.
 *
 * Seeed Wio-SX1262: its TCXO runs off DIO3, internal to the module.
 * RF work must SetDIO3AsTcxoCtrl then recalibrate.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Claim the module pins and bring up SPI, chip left in reset. */
esp_err_t sx1262_init(void);

/** @brief Pulse NRST and wait for BUSY to fall.
 *  @param[out] busy_fell_us  fall time in us, may be NULL. */
esp_err_t sx1262_reset(uint32_t *busy_fell_us);

/** @brief Write a command with its arguments. */
esp_err_t sx1262_cmd_write(uint8_t opcode, const uint8_t *args, size_t len);

/**
 * @brief Write a Get* command and read back its data, skipping the
 *        status byte these commands emit before it.
 */
esp_err_t sx1262_cmd_read_status(uint8_t opcode, uint8_t *out, size_t len);

/** @brief Write consecutive registers starting at addr. */
esp_err_t sx1262_reg_write(uint16_t addr, const uint8_t *data, size_t len);

/** @brief Read consecutive registers starting at addr. */
esp_err_t sx1262_reg_read(uint16_t addr, uint8_t *data, size_t len);

/** @brief Read len bytes from the radio data buffer at offset. */
esp_err_t sx1262_buffer_read(uint8_t offset, uint8_t *data, size_t len);

/**
 * @brief Install a DIO1 rising-edge interrupt that gives a semaphore.
 *
 * The semaphore is created by this call and returned through @p out.
 * Wiring the ISR to a semaphore rather than a callback keeps radio SPI
 * work out of interrupt context.
 */
esp_err_t sx1262_dio1_irq_enable(SemaphoreHandle_t *out);

/**
 * @brief Oldest unconsumed interrupt instant, microseconds since boot.
 *
 * Taken in the interrupt, so it is the moment the line rose rather than
 * the moment the task got round to looking - which is the difference
 * between microseconds and whatever the scheduler felt like.
 *
 * Consumed as it is read: each edge is reported once. Returns false
 * when none is waiting, which is the normal case on a timed-out wait.
 */
bool sx1262_dio1_irq_stamp(int64_t *out_us);

#ifdef __cplusplus
}
#endif
