/**
 * @file link_ota.h
 * @brief Firmware update over the serial link.
 *
 * The app opens a session with the size and the expected SHA-256, sends
 * the image in chunks, and closes it. The hash is computed as the bytes
 * arrive and checked before the boot partition is moved: an image that
 * does not match is never made bootable.
 *
 * Writes go to whichever slot is not running, so a failed update leaves
 * the working firmware untouched.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_OTA_IDLE,
    LINK_OTA_IN_PROGRESS,
    LINK_OTA_ERROR,
} link_ota_state_t;

typedef struct {
    size_t bytes_written;
    size_t total_size;
} link_ota_progress_t;

/** Decode buffer size. Chunks larger than this are refused. */
#define LINK_OTA_DECODE_BUF_SIZE 4096

esp_err_t link_ota_init(void);

/**
 * @brief Open a session.
 *
 * @param sha256_hex    64 hex characters, checked at link_ota_end().
 * @param out_partition Receives the target slot's label.
 */
esp_err_t link_ota_begin(size_t size, const char *sha256_hex,
                         const char **out_partition);

esp_err_t link_ota_write(const uint8_t *data, size_t len);

/**
 * @brief Verify the hash and switch the boot partition.
 *
 * @return ESP_ERR_INVALID_CRC when the image does not match the hash
 *         that was announced; the slot is left unbootable.
 */
esp_err_t link_ota_end(void);

/** @brief Drop the session and leave the running firmware in place. */
void link_ota_abort(void);

link_ota_progress_t link_ota_get_progress(void);

/**
 * @brief The shared decode buffer.
 *
 * Static, so a 4 KB chunk does not go through malloc on every message.
 */
uint8_t *link_ota_decode_buffer(size_t *out_size);

#ifdef __cplusplus
}
#endif
