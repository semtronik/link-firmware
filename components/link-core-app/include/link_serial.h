/**
 * @file link_serial.h
 * @brief JSON-per-line transport over the USB CDC port.
 *
 * One message per line. The receive task keeps only lines that start
 * with '{', so log output on the same port cannot be mistaken for a
 * command, and hands them to a queue the protocol layer drains.
 *
 * Transport only: nothing here knows what a message means.
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sized for the largest message that crosses this link, which is an OTA
 * chunk: base64 costs a third on top of the binary it carries. */
#define LINK_SERIAL_MSG_SIZE 6144

typedef struct {
    char data[LINK_SERIAL_MSG_SIZE];
} link_serial_msg_t;

/** @brief Install the USB CDC driver and create the queue. */
esp_err_t link_serial_init(void);

/** @brief Receive task. Start it once, after link_serial_init(). */
void link_serial_rx_task(void *arg);

/**
 * @brief Send one JSON message, newline-terminated.
 *
 * Serialised: several tasks report on this port and a half-written line
 * is not a message.
 */
void link_serial_send(const char *json);

/** @brief Queue carrying received messages. */
QueueHandle_t link_serial_rx_queue(void);

#ifdef __cplusplus
}
#endif
