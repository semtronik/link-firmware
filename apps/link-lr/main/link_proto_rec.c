/*
 * The recording commands: list the sessions on flash, read one out,
 * erase them all. Specified in link/protocol/v1/protocol.md.
 */

#include "link_proto_rec.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "link_proto.h"
#include "link_rec.h"
#include "link_serial.h"

static const char *TAG = "proto_rec";

#define SESSIONS_MAX 256
#define EDGE_SIZE    1100

static void send_phase(const char *cmd, const char *phase, const uint32_t *id,
                       const int *count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "phase", phase);
    if (id != NULL) {
        cJSON_AddNumberToObject(root, "id", *id);
    }
    if (count != NULL) {
        cJSON_AddNumberToObject(root, "count", *count);
    }
    link_proto_send_json(root);
}

static void send_info(const link_rec_session_t *s, const link_rec_stats_t *rec,
                      char *first, char *last)
{
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "type", "session_info");
    cJSON_AddNumberToObject(info, "id", s->id);

    if (link_rec_edge_lines(s->id, s->first_part, first, last, EDGE_SIZE)
        == ESP_OK) {
        cJSON *head = cJSON_Parse(first);
        const cJSON *ts = cJSON_GetObjectItem(head, "ts");
        if (cJSON_IsNumber(ts)) {
            cJSON_AddNumberToObject(info, "ts", ts->valuedouble);
        }
        cJSON_Delete(head);
    }

    if (link_rec_edge_lines(s->id, s->last_part, first, last, EDGE_SIZE)
        == ESP_OK) {
        cJSON *tail = cJSON_Parse(last);
        const cJSON *ts = cJSON_GetObjectItem(tail, "ts");
        const cJSON *type = cJSON_GetObjectItem(tail, "type");
        const cJSON *reason = cJSON_GetObjectItem(tail, "reason");
        if (cJSON_IsNumber(ts)) {
            cJSON_AddNumberToObject(info, "end_ts", ts->valuedouble);
        }
        if (cJSON_IsString(type) && strcmp(type->valuestring, "end") == 0
            && cJSON_IsString(reason)) {
            cJSON_AddStringToObject(info, "reason", reason->valuestring);
        }
        cJSON_Delete(tail);
    }

    cJSON_AddBoolToObject(info, "current", rec->open && rec->session == s->id);
    cJSON_AddBoolToObject(info, "truncated", s->first_part > 1);
    cJSON_AddNumberToObject(info, "bytes", s->bytes);
    link_proto_send_json(info);
}

static void handle_get_sessions(const cJSON *root, const cJSON *value)
{
    (void)root;
    (void)value;

    link_rec_session_t *sessions = malloc(SESSIONS_MAX * sizeof(*sessions));
    char *first = malloc(EDGE_SIZE);
    char *last = malloc(EDGE_SIZE);
    if (sessions == NULL || first == NULL || last == NULL) {
        free(sessions);
        free(first);
        free(last);
        link_proto_send_err("get_sessions", "out of memory");
        return;
    }

    const int count = link_rec_list(sessions, SESSIONS_MAX);
    link_rec_stats_t rec;
    link_rec_get_stats(&rec);

    send_phase("get_sessions", "begin", NULL, NULL);
    for (int i = 0; i < count; i++) {
        send_info(&sessions[i], &rec, first, last);
    }
    send_phase("get_sessions", "end", NULL, &count);

    free(sessions);
    free(first);
    free(last);
}

static void send_line(const char *line, void *user_data)
{
    link_serial_send(line);
    (*(int *)user_data)++;
}

/* Live lines are held for the whole read, so none lands inside it. */
static void handle_get_session(const cJSON *root, const cJSON *value)
{
    (void)value;
    const cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsNumber(id_item) || id_item->valuedouble < 1) {
        link_proto_send_err("get_session", "id (session number) required");
        return;
    }
    const uint32_t id = (uint32_t)id_item->valuedouble;

    link_rec_session_t *sessions = malloc(SESSIONS_MAX * sizeof(*sessions));
    if (sessions == NULL) {
        link_proto_send_err("get_session", "out of memory");
        return;
    }

    link_rec_mark_t mark;
    link_rec_hold(&mark);

    const int n = link_rec_list(sessions, SESSIONS_MAX);
    const link_rec_session_t *s = NULL;
    for (int i = 0; i < n; i++) {
        if (sessions[i].id == id) {
            s = &sessions[i];
            break;
        }
    }

    if (s == NULL) {
        link_proto_send_err("get_session", "unknown session");
    } else {
        send_phase("get_session", "begin", &id, NULL);

        int count = 0;
        for (unsigned part = s->first_part; part <= s->last_part; part++) {
            uint32_t to = UINT32_MAX;
            if (id == mark.session) {
                if (part > mark.part) {
                    break;
                }
                if (part == mark.part) {
                    to = mark.offset;
                }
            }
            link_rec_read(id, part, 0, to, send_line, &count);
        }

        send_phase("get_session", "end", &id, &count);
    }

    free(sessions);
    link_rec_release();
}

static void handle_erase_sessions(const cJSON *root, const cJSON *value)
{
    (void)root;
    (void)value;

    if (link_rec_erase() != ESP_OK) {
        link_proto_send_err("erase_sessions", "recording not running");
        return;
    }
    link_proto_send_ack("erase_sessions");
}

esp_err_t link_proto_rec_register(void)
{
    ESP_RETURN_ON_ERROR(link_proto_register("get_sessions",
                                            handle_get_sessions),
                        TAG, "get_sessions");
    ESP_RETURN_ON_ERROR(link_proto_register("get_session",
                                            handle_get_session),
                        TAG, "get_session");
    ESP_RETURN_ON_ERROR(link_proto_register("erase_sessions",
                                            handle_erase_sessions),
                        TAG, "erase_sessions");
    return ESP_OK;
}
