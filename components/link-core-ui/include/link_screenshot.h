/**
 * @file link_screenshot.h
 * @brief The "screenshot" protocol command.
 *
 * Streams the frame on the panel to the host, like get_events: an ack
 * with phase "begin" carrying the geometry, one "screenshot" message per
 * base64 chunk, then an ack with phase "end" carrying the chunk count
 * and a CRC-32 of the whole frame.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the command with link_proto.
 *
 * Call before link_proto_start(). Registers nothing when
 * CONFIG_LINK_SCREENSHOT is off.
 */
esp_err_t link_screenshot_register(void);

#ifdef __cplusplus
}
#endif
