/**
 * @file link_events.h
 * @brief Append-only event log on the FAT partition.
 *
 * For post-mortem diagnosis: what to write here is whatever you would
 * want to see if a unit came back from the field. Boot, shutdown,
 * battery events, radio failures - not debug traces. ESP_LOG is for
 * those, and it is gone the moment the board is unplugged.
 *
 * Two files in rotation, so the partition cannot fill: the current one
 * and the one before it.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Rotate at 32 KB - about 400 lines, so roughly 800 in all. */
#define LINK_EVENTS_ROTATE_BYTES (32 * 1024)

/**
 * @brief Mount the partition and open the current file.
 *
 * Call once after nvs_flash_init(). Formats the partition the first
 * time, which is why it comes before the first append rather than
 * lazily on one.
 */
esp_err_t link_events_init(void);

/**
 * @brief Append one line, timestamped.
 *
 * Stamped with the wall clock once it is set, and with uptime before
 * that - a line written during boot is still placed in the sequence.
 *
 * @param event   Short upper-case tag, e.g. "BOOT".
 * @param details Free text, or NULL.
 *
 * Silent when the log is not mounted: a missing log must never take
 * down the code that was trying to write to it.
 */
void link_events_append(const char *event, const char *details);

/** @brief Called once per line; return false to stop. */
typedef bool (*link_events_line_cb_t)(const char *line, void *user_data);

/**
 * @brief Stream both files, oldest first.
 *
 * Holds the lock throughout, so no rotation can happen mid-read.
 */
void link_events_dump(link_events_line_cb_t cb, void *user_data);

/**
 * @brief Delete both files, for a factory reset.
 *
 * Nothing is written after it: a restart is expected to follow, and the
 * next boot starts the log afresh.
 */
void link_events_erase(void);

#ifdef __cplusplus
}
#endif
