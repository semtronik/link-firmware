/**
 * @file link_proto.h
 * @brief The JSON command set, carried over link_serial.
 *
 * Commands arrive as {"cmd": "..."} and are answered with a "type" of
 * "ack" or "err".
 *
 * Platform commands live here: get_time, set_time, get_version,
 * get_events, ota_begin, ota_data, ota_end, reboot, factory_reset. Anything to do with
 * a module is registered by the application through
 * link_proto_register(), so this file never learns what a radio is.
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handler for one command.
 *
 * @param root  The whole message, for a command that carries more than
 *              "value".
 * @param value The "value" field, or NULL when the message has none.
 */
typedef void (*link_proto_handler_t)(const cJSON *root, const cJSON *value);

#define LINK_PROTO_HANDLERS_MAX 12

/**
 * @brief Add an application command.
 *
 * Call before link_proto_start(). A name that collides with a platform
 * command is refused: the platform set is what the desktop app relies
 * on, and shadowing one of them would break it silently.
 *
 * @param cmd Command name, matched exactly. Must outlive the call - a
 *            string literal is the usual case.
 */
esp_err_t link_proto_register(const char *cmd, link_proto_handler_t handler);

/**
 * @brief Start the transport and the command task.
 *
 * @param model Reported by get_version, e.g. "Link-LR". Must outlive the
 *              call.
 */
esp_err_t link_proto_start(const char *model);

/*
 * Replies, for application handlers. They own serialising and freeing
 * what they are given.
 */

/** @brief Send {"type":"ack","cmd":<cmd>,"status":"ok"}. */
void link_proto_send_ack(const char *cmd);

/** @brief Send {"type":"err","cmd":<cmd>,"msg":<msg>}. */
void link_proto_send_err(const char *cmd, const char *msg);

/**
 * @brief Send an object built by the caller, then delete it.
 *
 * The object is consumed either way, including when serialising fails.
 */
void link_proto_send_json(cJSON *root);

/**
 * @brief Called once per pass of the command task, after any message.
 *
 * Where an application pushes what nobody asked for - a received frame,
 * a changed reading. Runs on the protocol task, so it must not block for
 * long. NULL to remove.
 */
typedef void (*link_proto_poll_cb_t)(void);

void link_proto_set_poll(link_proto_poll_cb_t poll);

#ifdef __cplusplus
}
#endif
