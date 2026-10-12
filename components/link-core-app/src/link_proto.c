#include "link_proto.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_events.h"
#include "link_factory.h"
#include "link_ota.h"
#include "link_serial.h"
#include "link_time.h"
#include "mbedtls/base64.h"

static const char *TAG = "link_proto";

#define PRODUCT "Link"

static const char *s_model;

/* 2024-01-01 to 2100-01-01: outside this, the sender has a broken clock
 * and writing it to the RTC would only spread the problem. */
#define UTC_MIN 1704067200.0
#define UTC_MAX 4102444800.0

/* UTC-12 to UTC+14, the range real zones occupy. */
#define OFFSET_MIN_MINUTES (-720)
#define OFFSET_MAX_MINUTES 840

static struct {
    const char *cmd;
    link_proto_handler_t handler;
} s_handlers[LINK_PROTO_HANDLERS_MAX];

static unsigned s_handler_count;

static link_proto_poll_cb_t s_poll;

static void send_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);

    if (str != NULL) {
        link_serial_send(str);
        free(str);
    } else {
        ESP_LOGE(TAG, "out of memory serialising a reply");
    }

    cJSON_Delete(root);
}

static void send_ack(const char *cmd)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "status", "ok");
    send_json(root);
}

static void send_err(const char *cmd, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "err");
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "msg", msg);
    send_json(root);
}

void link_proto_send_ack(const char *cmd)
{
    send_ack(cmd);
}

void link_proto_send_err(const char *cmd, const char *msg)
{
    send_err(cmd, msg);
}

void link_proto_send_json(cJSON *root)
{
    send_json(root);
}

void link_proto_set_poll(link_proto_poll_cb_t poll)
{
    s_poll = poll;
}

static void handle_get_time(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "time");
    cJSON_AddNumberToObject(root, "utc", (double)link_time_get_utc());
    cJSON_AddNumberToObject(root, "offset_min",
                            (double)link_time_get_offset_minutes());
    cJSON_AddBoolToObject(root, "valid", link_time_is_valid());
    send_json(root);
}

static void handle_set_time(const cJSON *root)
{
    const cJSON *utc = cJSON_GetObjectItem(root, "utc");
    const cJSON *offset = cJSON_GetObjectItem(root, "offset_min");

    if (!cJSON_IsNumber(utc)) {
        send_err("set_time", "utc (epoch seconds) required");
        return;
    }

    if (utc->valuedouble < UTC_MIN || utc->valuedouble > UTC_MAX) {
        send_err("set_time", "utc out of range");
        return;
    }

    /* The offset is optional, but a bad one is an error rather than
     * something to quietly ignore. */
    if (offset != NULL) {
        if (!cJSON_IsNumber(offset)) {
            send_err("set_time", "offset_min must be a number");
            return;
        }

        if (offset->valueint < OFFSET_MIN_MINUTES
            || offset->valueint > OFFSET_MAX_MINUTES) {
            send_err("set_time", "offset_min out of range");
            return;
        }

        link_time_set_offset_minutes((int16_t)offset->valueint);
    }

    if (link_time_set_utc((time_t)utc->valuedouble) != ESP_OK) {
        send_err("set_time", "rtc write failed");
        return;
    }

    link_events_append("TIME_SET", "source=app");
    send_ack("set_time");
}

/*
 * Identity
 */
static void handle_get_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "cmd", "get_version");
    cJSON_AddStringToObject(root, "product", PRODUCT);
    cJSON_AddStringToObject(root, "model", s_model);
    cJSON_AddStringToObject(root, "version", desc->version);
    cJSON_AddStringToObject(root, "build_date", desc->date);
    send_json(root);
}

static void handle_reboot(void)
{
    send_ack("reboot");
    link_events_append("SHUTDOWN", "reason=reboot_cmd");

    /* Long enough for the reply to reach the host. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* Asked for in full, so a stray or mistyped command cannot wipe a unit. */
static void handle_factory_reset(const cJSON *root)
{
    const cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    if (!cJSON_IsString(confirm) || strcmp(confirm->valuestring, "erase") != 0) {
        send_err("factory_reset", "confirm must be \"erase\"");
        return;
    }

    send_ack("factory_reset");

    /* Long enough for the reply to reach the host. */
    vTaskDelay(pdMS_TO_TICKS(500));
    link_factory_reset();
}

/*
 * Event log
 */
typedef struct {
    uint32_t seq;
} dump_ctx_t;

static bool dump_line_cb(const char *line, void *user_data)
{
    dump_ctx_t *ctx = user_data;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "line", line);
    cJSON_AddNumberToObject(root, "seq", (double)ctx->seq);
    send_json(root);

    ctx->seq++;

    return true;
}

/* Bracketed by begin and end, so the app knows the stream is complete
 * rather than merely paused. */
static void handle_get_events(void)
{
    cJSON *begin = cJSON_CreateObject();
    cJSON_AddStringToObject(begin, "type", "ack");
    cJSON_AddStringToObject(begin, "cmd", "get_events");
    cJSON_AddStringToObject(begin, "phase", "begin");
    send_json(begin);

    dump_ctx_t ctx = {.seq = 0};
    link_events_dump(dump_line_cb, &ctx);

    cJSON *end = cJSON_CreateObject();
    cJSON_AddStringToObject(end, "type", "ack");
    cJSON_AddStringToObject(end, "cmd", "get_events");
    cJSON_AddStringToObject(end, "phase", "end");
    cJSON_AddNumberToObject(end, "count", (double)ctx.seq);
    send_json(end);
}

/*
 * Firmware update
 */
static void handle_ota_begin(const cJSON *root)
{
    const cJSON *size = cJSON_GetObjectItem(root, "size");
    const cJSON *sha256 = cJSON_GetObjectItem(root, "sha256");

    if (!cJSON_IsNumber(size) || !cJSON_IsString(sha256)) {
        send_err("ota_begin", "missing size or sha256");
        return;
    }

    const char *partition = NULL;
    if (link_ota_begin((size_t)size->valuedouble, sha256->valuestring,
                       &partition)
        != ESP_OK) {
        link_ota_abort();
        send_err("ota_begin", "ota_begin failed");
        return;
    }

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "ack");
    cJSON_AddStringToObject(reply, "cmd", "ota_begin");
    cJSON_AddStringToObject(reply, "partition", partition);
    send_json(reply);
}

/* Any failure aborts the whole session: a gap in the image would only
 * be found later, at the hash check, after transferring the rest for
 * nothing. */
static void handle_ota_data(const cJSON *root)
{
    const cJSON *data = cJSON_GetObjectItem(root, "data");

    if (!cJSON_IsString(data)) {
        link_ota_abort();
        send_err("ota_data", "missing data field");
        return;
    }

    size_t buf_size = 0;
    uint8_t *buf = link_ota_decode_buffer(&buf_size);
    size_t decoded = 0;

    if (mbedtls_base64_decode(buf, buf_size, &decoded,
                              (const unsigned char *)data->valuestring,
                              strlen(data->valuestring))
        != 0) {
        link_ota_abort();
        send_err("ota_data", "base64 decode failed");
        return;
    }

    if (link_ota_write(buf, decoded) != ESP_OK) {
        link_ota_abort();
        send_err("ota_data", "ota_write failed");
        return;
    }

    const link_ota_progress_t progress = link_ota_get_progress();

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "ack");
    cJSON_AddStringToObject(reply, "cmd", "ota_data");
    cJSON_AddNumberToObject(reply, "written", (double)progress.bytes_written);
    send_json(reply);
}

static void handle_ota_end(void)
{
    if (link_ota_end() != ESP_OK) {
        link_ota_abort();
        send_err("ota_end", "verification failed");
        return;
    }

    link_events_append("OTA", "status=success");

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "ack");
    cJSON_AddStringToObject(reply, "cmd", "ota_end");
    cJSON_AddStringToObject(reply, "status", "success");
    send_json(reply);
}

static bool platform_owns(const char *cmd)
{
    static const char *const platform[] = {
        "get_time", "set_time", "get_version", "get_events",
        "ota_begin", "ota_data", "ota_end", "reboot", "factory_reset",
    };

    for (size_t i = 0; i < sizeof(platform) / sizeof(platform[0]); i++) {
        if (strcmp(cmd, platform[i]) == 0) {
            return true;
        }
    }

    return false;
}

/* Platform commands come first and cannot be overridden - the desktop
 * app relies on them answering the same way on every product. */
static bool dispatch_platform(const char *cmd, const cJSON *root)
{
    if (strcmp(cmd, "get_time") == 0) {
        handle_get_time();
    } else if (strcmp(cmd, "set_time") == 0) {
        handle_set_time(root);
    } else if (strcmp(cmd, "get_version") == 0) {
        handle_get_version();
    } else if (strcmp(cmd, "get_events") == 0) {
        handle_get_events();
    } else if (strcmp(cmd, "ota_begin") == 0) {
        handle_ota_begin(root);
    } else if (strcmp(cmd, "ota_data") == 0) {
        handle_ota_data(root);
    } else if (strcmp(cmd, "ota_end") == 0) {
        handle_ota_end();
    } else if (strcmp(cmd, "reboot") == 0) {
        handle_reboot();
    } else if (strcmp(cmd, "factory_reset") == 0) {
        handle_factory_reset(root);
    } else {
        return false;
    }

    return true;
}

esp_err_t link_proto_register(const char *cmd, link_proto_handler_t handler)
{
    if (cmd == NULL || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (platform_owns(cmd)) {
        ESP_LOGE(TAG, "'%s' is a platform command", cmd);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_handler_count >= LINK_PROTO_HANDLERS_MAX) {
        ESP_LOGE(TAG, "no room for '%s'", cmd);
        return ESP_ERR_NO_MEM;
    }

    s_handlers[s_handler_count].cmd = cmd;
    s_handlers[s_handler_count].handler = handler;
    s_handler_count++;

    return ESP_OK;
}

static void dispatch(const cJSON *root)
{
    const cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");

    if (!cJSON_IsString(cmd_item)) {
        return;
    }

    const char *cmd = cmd_item->valuestring;

    if (dispatch_platform(cmd, root)) {
        return;
    }

    const cJSON *value = cJSON_GetObjectItem(root, "value");

    for (unsigned i = 0; i < s_handler_count; i++) {
        if (strcmp(cmd, s_handlers[i].cmd) == 0) {
            s_handlers[i].handler(root, value);
            return;
        }
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "unknown command: %s", cmd);
    send_err(cmd, msg);
}

/*
 * Waits on the receive queue, but wakes regularly anyway so the
 * application still gets its poll when the app is saying nothing.
 */
#define QUEUE_WAIT_MS 100

static void proto_task(void *arg)
{
    (void)arg;

    QueueHandle_t queue = link_serial_rx_queue();

    while (true) {
        link_serial_msg_t msg;

        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(QUEUE_WAIT_MS))
            == pdTRUE) {
            cJSON *root = cJSON_Parse(msg.data);

            if (root == NULL) {
                send_err("?", "malformed JSON");
            } else {
                dispatch(root);
                cJSON_Delete(root);
            }
        }

        if (s_poll != NULL) {
            s_poll();
        }
    }
}

esp_err_t link_proto_start(const char *model)
{
    s_model = model;

    esp_err_t err = link_serial_init();
    if (err != ESP_OK) {
        return err;
    }

    err = link_ota_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Both tasks hold a link_serial_msg_t - 6 KB - as a local, so
     * neither can be sized as if it only held its own frames. */
    if (xTaskCreate(link_serial_rx_task, "serial_rx", 10240, NULL, 3, NULL)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* That message, plus what cJSON allocates and the OTA chunks that
     * pass through here. */
    if (xTaskCreate(proto_task, "proto", 16384, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening");

    return ESP_OK;
}
