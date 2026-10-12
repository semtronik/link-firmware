#include "link_screenshot.h"

#include "sdkconfig.h"

#if CONFIG_LINK_SCREENSHOT

#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"

#include "link_display.h"
#include "link_lvgl.h"
#include "link_proto.h"
#include "link_serial.h"

#define CMD "screenshot"

#define FRAME_BYTES (LINK_DISPLAY_H_RES * LINK_DISPLAY_V_RES * 2)

/* A multiple of 3, so only the last chunk can carry base64 padding. */
#define CHUNK_BYTES 3072
#define LINE_BYTES  (CHUNK_BYTES / 3 * 4 + 64)

static void send_begin(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "cmd", CMD);
    cJSON_AddStringToObject(root, "phase", "begin");
    cJSON_AddNumberToObject(root, "width", LINK_DISPLAY_H_RES);
    cJSON_AddNumberToObject(root, "height", LINK_DISPLAY_V_RES);
    cJSON_AddStringToObject(root, "format", "rgb565be");
    cJSON_AddNumberToObject(root, "size", FRAME_BYTES);
    link_proto_send_json(root);
}

static void send_end(unsigned count, uint32_t crc)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "cmd", CMD);
    cJSON_AddStringToObject(root, "phase", "end");
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "crc32", crc);
    link_proto_send_json(root);
}

static void handle_screenshot(const cJSON *root, const cJSON *value)
{
    (void)root;
    (void)value;

    uint8_t *frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    char *line = malloc(LINE_BYTES);

    if (frame == NULL || line == NULL) {
        link_proto_send_err(CMD, "out of memory");
        goto out;
    }

    if (!link_lvgl_frame_copy(frame, FRAME_BYTES)) {
        link_proto_send_err(CMD, "no frame available");
        goto out;
    }

    send_begin();

    unsigned count = 0;

    for (size_t off = 0; off < FRAME_BYTES; off += CHUNK_BYTES) {
        const size_t len =
            (FRAME_BYTES - off < CHUNK_BYTES) ? FRAME_BYTES - off : CHUNK_BYTES;
        const int head = snprintf(line, LINE_BYTES,
                                  "{\"type\":\"" CMD "\",\"seq\":%u,\"data\":\"",
                                  count);
        size_t encoded = 0;

        if (mbedtls_base64_encode((unsigned char *)line + head,
                                  LINE_BYTES - head, &encoded, frame + off,
                                  len) != 0) {
            link_proto_send_err(CMD, "base64 encode failed");
            goto out;
        }

        snprintf(line + head + encoded, LINE_BYTES - head - encoded, "\"}");
        link_serial_send(line);
        count++;
    }

    send_end(count, esp_rom_crc32_le(0, frame, FRAME_BYTES));

out:
    free(line);
    heap_caps_free(frame);
}

esp_err_t link_screenshot_register(void)
{
    return link_proto_register(CMD, handle_screenshot);
}

#else

esp_err_t link_screenshot_register(void)
{
    return ESP_OK;
}

#endif
