/**
 * @file link_proto_radio.h
 * @brief The radio commands of the JSON protocol.
 *
 * get_profile, set_profile and tx. The recording commands come from
 * link_proto_rec.h, the platform ones from link_proto.h.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add the radio commands to the protocol.
 *
 * Call before link_proto_start().
 */
esp_err_t link_proto_radio_register(void);

#ifdef __cplusplus
}
#endif
