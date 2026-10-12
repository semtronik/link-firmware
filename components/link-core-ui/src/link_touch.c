/**
 * @file link_touch.c
 * @brief FT5426 capacitive touch bring-up for the link base board.
 *
 * The controller logic lives in the managed component
 * esp_lcd_touch_ft5x06. This file only does the board-specific part:
 * the dedicated I2C bus, the panel IO, and the rotation from the
 * controller's portrait frame to the panel's landscape one.
 */

#include "link_touch.h"

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "link_display.h"

static const char *TAG = "link_touch";

/* Port 0 belongs to the core bus (RTC, fuel gauge), which stays claimed
 * for the whole run, so the touch panel gets port 1. */
/* Bus speed comes from ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG() (100 kHz),
 * which the new driver applies per device rather than per bus. */
#define TOUCH_I2C_PORT 1

/* The controller reports in its native portrait frame, 240 x 320. */
#define TOUCH_NATIVE_X_MAX LINK_DISPLAY_V_RES
#define TOUCH_NATIVE_Y_MAX LINK_DISPLAY_H_RES

static esp_lcd_touch_handle_t s_touch = NULL;

/*
 * The LVGL input driver and the console both poll through
 * link_touch_read(). read_data() latches the contact into the driver's
 * own buffer, so two interleaved calls make each other read the other's
 * sample - or an empty one.
 */
static SemaphoreHandle_t s_lock = NULL;

/*
 * ID_G_MODE (0xA4) is documented as selecting what CTP_INT does: 0 for
 * polling mode (the line simply held low while a contact lasts), 1 for
 * trigger mode (a pulse per internal scan).
 *
 * Measured on this board: the FT5426 REFUSES the write. It reads back
 * 0x00 whatever is written to it, so trigger mode is not available and
 * the code no longer tries - a write with no effect only misleads
 * whoever reads this next.
 *
 * It works regardless. Despite the documented polling mode, the line
 * produces edges continuously while a finger is down - thousands of
 * them per minute of use, and the pointer follows a drag smoothly. The
 * exact mechanism is not established here (scan-related activity on the
 * line, or bounce); what is established is that CTP_INT delivers usable
 * edges, which is what the interrupt path needs.
 *
 * The register is still read at init: it records the controller's
 * actual behaviour at every boot, and would flag a different part that
 * does accept the mode.
 */
#define FT5x06_ID_G_MODE 0xA4

static link_touch_isr_cb_t s_isr_cb = NULL;
static void *s_isr_arg = NULL;
static volatile uint32_t s_isr_count = 0;
static int s_int_mode = -1;

esp_lcd_touch_handle_t link_touch_handle(void)
{
    return s_touch;
}

uint32_t link_touch_isr_count(void)
{
    return s_isr_count;
}

int link_touch_int_mode(void)
{
    return s_int_mode;
}

/*
 * Interrupt context. Counts the edge and hands over to the registered
 * callback - nothing else: no I2C (the read happens on the LVGL task),
 * no logging, no lock.
 */
static void touch_isr(esp_lcd_touch_handle_t tp)
{
    s_isr_count++;

    if (s_isr_cb != NULL && s_isr_cb(s_isr_arg)) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t link_touch_set_isr_callback(link_touch_isr_cb_t cb, void *arg)
{
    ESP_RETURN_ON_FALSE(s_touch != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "touch not initialised");

    /* Order matters: arm the target before the source, disarm the
     * source before the target, so the ISR never sees a half-set pair. */
    if (cb != NULL) {
        s_isr_arg = arg;
        s_isr_cb = cb;
        ESP_RETURN_ON_ERROR(
            esp_lcd_touch_register_interrupt_callback(s_touch, touch_isr), TAG,
            "interrupt callback register failed");
        ESP_LOGI(TAG, "CTP_INT armed on gpio %d", BOARD_PIN_CTP_INT);
    } else {
        ESP_RETURN_ON_ERROR(
            esp_lcd_touch_register_interrupt_callback(s_touch, NULL), TAG,
            "interrupt callback remove failed");
        s_isr_cb = NULL;
        s_isr_arg = NULL;
    }

    return ESP_OK;
}

/*
 * Read straight off the panel IO rather than through the component:
 * esp_lcd_touch_ft5x06 exposes no register accessor, and managed
 * components are gitignored - patching one would be undone by the next
 * dependency resolution.
 */
static void touch_read_int_mode(void)
{
    uint8_t mode = 0xFF;

    if (esp_lcd_panel_io_rx_param(s_touch->io, FT5x06_ID_G_MODE, &mode, 1) != ESP_OK) {
        ESP_LOGW(TAG, "ID_G_MODE read failed");
        return;
    }

    s_int_mode = mode;
    ESP_LOGI(TAG, "CTP_INT mode: ID_G_MODE=0x%02X", mode);
}

static esp_err_t touch_i2c_init(i2c_master_bus_handle_t *out_bus)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = BOARD_PIN_CTP_SDA,
        .scl_io_num = BOARD_PIN_CTP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The board carries its own pull-ups; internal ones would only
         * weaken the edges. */
        .flags.enable_internal_pullup = 0,
    };

    return i2c_new_master_bus(&cfg, out_bus);
}

esp_err_t link_touch_init(void)
{
    if (s_touch != NULL) {
        ESP_LOGW(TAG, "touch already initialised");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "I2C bus: sda=%d scl=%d", BOARD_PIN_CTP_SDA, BOARD_PIN_CTP_SCL);
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(touch_i2c_init(&bus), TAG, "touch i2c bus failed");

    const esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &io_handle),
                        TAG, "touch panel io failed");

    /*
     * x_max/y_max describe the controller's own frame, not the panel's:
     * the rotation to landscape is done in link_touch_read(). The driver
     * owns the reset pulse, so CTP_RESET must not be driven anywhere
     * else. Both lines are active low.
     */
    ESP_LOGI(TAG, "driver: ft5x06, rst=%d int=%d",
             BOARD_PIN_CTP_RESET, BOARD_PIN_CTP_INT);
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_NATIVE_X_MAX,
        .y_max = TOUCH_NATIVE_Y_MAX,
        .rst_gpio_num = BOARD_PIN_CTP_RESET,
        .int_gpio_num = BOARD_PIN_CTP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        /* Left neutral on purpose: link_touch_read() does the mapping,
         * so there is one place to fix if an axis comes out flipped. */
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "lock alloc failed");

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(io_handle, &tp_cfg, &s_touch),
                        TAG, "ft5x06 init failed");

    /* After the driver init: it resets the controller, so the value
     * read before would not be the one in force. */
    touch_read_int_mode();

    ESP_LOGI(TAG, "touch ready");
    return ESP_OK;
}

bool link_touch_read(uint16_t *x, uint16_t *y)
{
    if (s_touch == NULL || x == NULL || y == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;
    const bool got = esp_lcd_touch_read_data(s_touch) == ESP_OK &&
                     esp_lcd_touch_get_data(s_touch, &point, &count, 1) == ESP_OK &&
                     count > 0;

    xSemaphoreGive(s_lock);

    if (!got) {
        return false;
    }

    /*
     * Portrait -> landscape. The axes trade places and the new Y runs
     * backwards; X does not. Taken from the NHD-2.8 proof of concept,
     * where it was checked at all four corners on the same FT5426 and
     * the same panel orientation. If an axis comes out reversed on the
     * bench, this is the one place to change.
     */
    *x = point.y;
    *y = (uint16_t)(TOUCH_NATIVE_X_MAX - 1 - point.x);

    return true;
}
