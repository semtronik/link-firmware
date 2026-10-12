#include "link_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psa/crypto.h"

static const char *TAG = "link_ota";

static link_ota_state_t s_state = LINK_OTA_IDLE;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static size_t s_total;
static size_t s_written;

static psa_hash_operation_t s_sha = PSA_HASH_OPERATION_INIT;
static char s_expected[65];

/* Static: one 4 KB buffer reused for every chunk, rather than a malloc
 * per message on a path that runs thousands of times per update. */
static uint8_t s_decode_buf[LINK_OTA_DECODE_BUF_SIZE];

esp_err_t link_ota_init(void)
{
    s_state = LINK_OTA_IDLE;

    return ESP_OK;
}

esp_err_t link_ota_begin(size_t size, const char *sha256_hex,
                         const char **out_partition)
{
    if (s_state == LINK_OTA_IN_PROGRESS) {
        ESP_LOGW(TAG, "session already open, dropping it");
        link_ota_abort();
    }

    /* Never the running slot. */
    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        ESP_LOGE(TAG, "no OTA partition - is the table single-factory?");
        s_state = LINK_OTA_ERROR;
        return ESP_ERR_NOT_FOUND;
    }

    if (size > s_target->size) {
        ESP_LOGE(TAG, "image is %u B, slot is %lu B", (unsigned)size,
                 (unsigned long)s_target->size);
        s_state = LINK_OTA_ERROR;
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t err = esp_ota_begin(s_target, size, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        s_state = LINK_OTA_ERROR;
        return err;
    }

    psa_status_t ps = psa_hash_setup(&s_sha, PSA_ALG_SHA_256);
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup: %d", (int)ps);
        esp_ota_abort(s_handle);
        s_state = LINK_OTA_ERROR;
        return ESP_FAIL;
    }

    strncpy(s_expected, sha256_hex, sizeof(s_expected) - 1);
    s_expected[sizeof(s_expected) - 1] = '\0';

    s_total = size;
    s_written = 0;
    s_state = LINK_OTA_IN_PROGRESS;

    if (out_partition != NULL) {
        *out_partition = s_target->label;
    }

    ESP_LOGI(TAG, "begin: %u B into %s", (unsigned)size, s_target->label);

    return ESP_OK;
}

esp_err_t link_ota_write(const uint8_t *data, size_t len)
{
    if (s_state != LINK_OTA_IN_PROGRESS) {
        return ESP_ERR_INVALID_STATE;
    }

    /* More than was announced means the sender and the receiver no
     * longer agree on what is being transferred. */
    if (s_written + len > s_total) {
        ESP_LOGE(TAG, "more data than announced");
        s_state = LINK_OTA_ERROR;
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        s_state = LINK_OTA_ERROR;
        return err;
    }

    /* Hashed as it goes by: the image is never held whole anywhere. */
    psa_hash_update(&s_sha, data, len);
    s_written += len;

    return ESP_OK;
}

esp_err_t link_ota_end(void)
{
    if (s_state != LINK_OTA_IN_PROGRESS) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t digest[32];
    size_t digest_len = 0;
    const psa_status_t ps = psa_hash_finish(&s_sha, digest, sizeof(digest),
                                            &digest_len);
    if (ps != PSA_SUCCESS || digest_len != sizeof(digest)) {
        ESP_LOGE(TAG, "psa_hash_finish: %d", (int)ps);
        esp_ota_abort(s_handle);
        s_state = LINK_OTA_ERROR;
        return ESP_FAIL;
    }

    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&hex[i * 2], "%02x", digest[i]);
    }
    hex[64] = '\0';

    /* Before esp_ota_end(), and before the boot partition moves: an
     * image that does not match must never become bootable. */
    if (strcmp(hex, s_expected) != 0) {
        ESP_LOGE(TAG, "hash mismatch");
        ESP_LOGE(TAG, "  expected %s", s_expected);
        ESP_LOGE(TAG, "  got      %s", hex);
        esp_ota_abort(s_handle);
        s_state = LINK_OTA_ERROR;
        return ESP_ERR_INVALID_CRC;
    }

    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        s_state = LINK_OTA_ERROR;
        return err;
    }

    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        s_state = LINK_OTA_ERROR;
        return err;
    }

    ESP_LOGI(TAG, "done, next boot from %s", s_target->label);
    s_state = LINK_OTA_IDLE;

    return ESP_OK;
}

void link_ota_abort(void)
{
    if (s_state == LINK_OTA_IN_PROGRESS) {
        esp_ota_abort(s_handle);
        psa_hash_abort(&s_sha);
        ESP_LOGW(TAG, "aborted");
    }

    s_state = LINK_OTA_IDLE;
    s_written = 0;
    s_total = 0;
}

link_ota_progress_t link_ota_get_progress(void)
{
    const link_ota_progress_t p = {
        .bytes_written = s_written,
        .total_size = s_total,
    };

    return p;
}

uint8_t *link_ota_decode_buffer(size_t *out_size)
{
    if (out_size != NULL) {
        *out_size = sizeof(s_decode_buf);
    }

    return s_decode_buf;
}
