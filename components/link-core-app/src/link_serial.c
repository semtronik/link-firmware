#include "link_serial.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link_serial";

#define RX_QUEUE_DEPTH 8
#define READ_BUF_SIZE  1024

/* Assembling a line and carrying it are sized separately, and the queue
 * entry is the smaller of the two: what fits an OTA chunk - 4 KB of
 * base64 plus its JSON - with room to spare, without putting 8 KB per
 * slot into a queue eight deep. A line between the two sizes is
 * truncated on its way into the queue, which the JSON parse then
 * rejects; the sender gets an error either way. */
#define LINE_BUF_SIZE 8192

_Static_assert(LINK_SERIAL_MSG_SIZE <= LINE_BUF_SIZE,
               "a queue entry cannot hold more than a line");

static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_tx_lock;

esp_err_t link_serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 8192,
    };

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Routes stdout through the driver, so printf and the reads below
     * are talking to the same endpoint. */
    usb_serial_jtag_vfs_use_driver();

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(link_serial_msg_t));
    s_tx_lock = xSemaphoreCreateMutex();

    if (s_rx_queue == NULL || s_tx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ready");

    return ESP_OK;
}

void link_serial_rx_task(void *arg)
{
    (void)arg;

    /* Static: 8 KB of line buffer has no business on a task stack. */
    static char line[LINE_BUF_SIZE];
    static uint8_t chunk[READ_BUF_SIZE];
    int pos = 0;

    while (true) {
        const int len = usb_serial_jtag_read_bytes(chunk, sizeof(chunk),
                                                   pdMS_TO_TICKS(20));
        if (len <= 0) {
            continue;
        }

        for (int i = 0; i < len; i++) {
            const char c = (char)chunk[i];

            if (c == '\r') {
                continue;
            }

            if (c != '\n') {
                /* Truncated rather than allowed to run over the buffer.
                 * See the note on the sizes above. */
                if (pos < (int)sizeof(line) - 1) {
                    line[pos++] = c;
                }
                continue;
            }

            if (pos == 0) {
                continue;
            }
            line[pos] = '\0';

            /* Logs share this port. Anything not starting a JSON object
             * is one of those, not a command. */
            if (line[0] == '{') {
                link_serial_msg_t msg;
                strncpy(msg.data, line, sizeof(msg.data) - 1);
                msg.data[sizeof(msg.data) - 1] = '\0';

                if (xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "queue full, message dropped");
                }
            }

            pos = 0;
        }
    }
}

void link_serial_send(const char *json)
{
    if (json == NULL) {
        return;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    printf("%s\n", json);
    fflush(stdout);
    xSemaphoreGive(s_tx_lock);
}

QueueHandle_t link_serial_rx_queue(void)
{
    return s_rx_queue;
}
