/**
 * @file link_display.c
 * @brief ST7789 panel bring-up for the link base board.
 *
 * Ported from the NHD-2.8 breakout proof of concept: the ST7789
 * initialisation sequence and the landscape orientation are kept as
 * validated there, only the pin assignment changes — it now comes from
 * board_pins.h instead of the breakout wiring.
 *
 * The panel has two possible owners: link_display_fill() before LVGL
 * starts, LVGL afterwards. link_display_attach_lvgl() switches between
 * them; they never run at the same time.
 */

#include "link_display.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "st7789t/st7789t_panel.h"

static const char *TAG = "link_display";

/* SPI2 is free on this board; SPI3 has no particular advantage here. */
#define LCD_SPI_HOST SPI2_HOST

/*
 * Conservative bring-up clock. The ST7789VI datasheet allows far more,
 * but a slow clock rules out signal integrity as a cause if the first
 * image is wrong. Raise once the panel is confirmed working.
 */
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)

#define LCD_CMD_BITS   8
#define LCD_PARAM_BITS 8

static esp_lcd_panel_handle_t s_panel = NULL;

/*
 * draw_bitmap only queues a transfer; it does not wait for the pixels to
 * land. Without this handshake link_display_fill() can return while the
 * last rows are still in flight, so a caller that switches the backlight
 * on straight after shows whatever noise the GRAM powered up with - and
 * the row buffer gets freed while DMA may still be reading it.
 */
static SemaphoreHandle_t s_trans_done = NULL;

/* Set by link_display_attach_lvgl(), read from the ISR: once non-NULL,
 * completed transfers belong to LVGL. */
static lv_display_t *volatile s_lvgl_disp = NULL;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event,
                                void *user_ctx)
{
    lv_display_t *disp = s_lvgl_disp;

    if (disp != NULL) {
        lv_display_flush_ready(disp);
        return false;
    }

    BaseType_t higher_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &higher_woken);
    return higher_woken == pdTRUE;
}

void link_display_attach_lvgl(lv_display_t *disp)
{
    s_lvgl_disp = disp;
    ESP_LOGI(TAG, "panel handed over to LVGL, fill path closed");
}

bool link_display_lvgl_attached(void)
{
    return s_lvgl_disp != NULL;
}

esp_lcd_panel_handle_t link_display_panel(void)
{
    return s_panel;
}

esp_err_t link_display_panel_init(void)
{
    if (s_panel != NULL) {
        ESP_LOGW(TAG, "panel already initialised");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SPI bus: sck=%d mosi=%d", BOARD_PIN_LCD_SCK, BOARD_PIN_LCD_MOSI);
    const spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_PIN_LCD_SCK,
        .mosi_io_num = BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1, /* write-only panel, no MISO net on this board */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LINK_DISPLAY_H_RES * LINK_DISPLAY_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    /* Counting, not binary: one give lands per queued row and they must
     * all be accounted for, not collapsed into a single token. */
    s_trans_done = xSemaphoreCreateCounting(LINK_DISPLAY_V_RES, 0);
    ESP_RETURN_ON_FALSE(s_trans_done != NULL, ESP_ERR_NO_MEM, TAG,
                        "transfer semaphore alloc failed");

    ESP_LOGI(TAG, "panel IO: cs=%d dc=%d", BOARD_PIN_LCD_CS, BOARD_PIN_LCD_DC);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_PIN_LCD_DC,
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        /* Serves both owners: see on_color_trans_done() above. */
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_config, &io_handle),
                        TAG, "esp_lcd_new_panel_io_spi failed");

    ESP_LOGI(TAG, "panel driver: st7789t, rst=%d", BOARD_PIN_LCD_RST);
    const esp_lcd_panel_dev_st7789t_config_t panel_config = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789t(io_handle, &panel_config, &s_panel),
                        TAG, "esp_lcd_new_panel_st7789t failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");

    /*
     * Landscape 320x240. The native MADCTL in the init sequence is
     * portrait; swap_xy rotates it and mirror() compensates the
     * horizontal flip observed during proof-of-concept bring-up.
     */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror failed");

    /* Display on, but the backlight stays off until the caller fills. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on failed");

    ESP_LOGI(TAG, "panel ready (%dx%d)", LINK_DISPLAY_H_RES, LINK_DISPLAY_V_RES);
    return ESP_OK;
}

esp_err_t link_display_fill(uint32_t rgb888)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "panel not initialised");
    ESP_RETURN_ON_FALSE(s_lvgl_disp == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "panel owned by LVGL");

    /*
     * The panel wants each pixel MSB first, but the ESP32-S3 stores a
     * uint16_t little-endian - so the 16-bit value has to be swapped on
     * its way to the wire.
     *
     * In LVGL 8.3 this was free: lv_color_t *was* the wire pixel and
     * CONFIG_LV_COLOR_16_SWAP baked the swap into it, so building the
     * colour through lv_color_hex() inherited it. In 9.x lv_color_t is
     * a 3-byte BGR888 struct, unrelated to what the panel reads, and
     * lv_color_to_u16() returns native-endian RGB565. The swap is
     * therefore explicit here.
     *
     * It still happens exactly once per owner: this path swaps its one
     * pixel by hand, and LVGL renders straight into
     * LV_COLOR_FORMAT_RGB565_SWAPPED (see link_lvgl.c) without a
     * per-flush pass. Doing both to the same pixel would cancel out and
     * every colour would come out wrong while the image still looked
     * coherent.
     */
    const uint16_t wire =
        __builtin_bswap16(lv_color_to_u16(lv_color_hex(rgb888 & 0xFFFFFF)));

    /* One row of DMA-capable internal RAM (640 B), reused for every line. */
    uint16_t *row = heap_caps_malloc(LINK_DISPLAY_H_RES * sizeof(uint16_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(row != NULL, ESP_ERR_NO_MEM, TAG, "row buffer alloc failed");

    for (int i = 0; i < LINK_DISPLAY_H_RES; i++) {
        row[i] = wire;
    }

    esp_err_t err = ESP_OK;
    int queued = 0;
    for (int y = 0; y < LINK_DISPLAY_V_RES && err == ESP_OK; y++) {
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LINK_DISPLAY_H_RES, y + 1, row);
        if (err == ESP_OK) {
            queued++;
        }
    }

    /* One give per row that actually made it into the queue: drain them
     * all, so the buffer is only freed once DMA is done with it. */
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(s_trans_done, portMAX_DELAY);
    }

    heap_caps_free(row);
    ESP_RETURN_ON_ERROR(err, TAG, "draw_bitmap failed");

    ESP_LOGI(TAG, "filled with #%06lX", (unsigned long)(rgb888 & 0xFFFFFF));
    return ESP_OK;
}

/*
 * Backlight: PWM dimming.
 *
 * The base board drives the panel LEDs through a CAT4104 constant-current
 * sink. Its EN pin doubles as a dimming input, rated 1-20 kHz: we sit at
 * the top of that window, because 5 kHz was audible on the bench. Past
 * 20 kHz the sink no longer tracks the duty cycle, so this is a ceiling,
 * not a preference.
 *
 * 20 kHz at 10 bits needs 20.5 MHz of the 80 MHz LEDC source, so the
 * resolution costs nothing here.
 */
#define LCD_BL_TIMER   LEDC_TIMER_0
#define LCD_BL_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_MODE    LEDC_LOW_SPEED_MODE
#define LCD_BL_RES     LEDC_TIMER_10_BIT
#define LCD_BL_FREQ_HZ 20000

static uint8_t s_backlight_percent;

static esp_err_t backlight_init(void)
{
    static bool configured = false;

    if (configured) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer = {
        .speed_mode = LCD_BL_MODE,
        .duty_resolution = LCD_BL_RES,
        .timer_num = LCD_BL_TIMER,
        .freq_hz = LCD_BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer config failed");

    /* duty 0: the panel stays dark until the caller asks for light. */
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_PIN_LCD_BL,
        .speed_mode = LCD_BL_MODE,
        .channel = LCD_BL_CHANNEL,
        .timer_sel = LCD_BL_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG,
                        "backlight channel config failed");

    configured = true;
    return ESP_OK;
}

esp_err_t link_display_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    const uint32_t max_duty = (1U << LCD_BL_RES) - 1U;
    const uint32_t duty = (max_duty * percent) / 100U;

    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_MODE, LCD_BL_CHANNEL, duty), TAG,
                        "backlight set_duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_BL_MODE, LCD_BL_CHANNEL), TAG,
                        "backlight update_duty failed");

    s_backlight_percent = percent;
    ESP_LOGI(TAG, "backlight %u%%", (unsigned)percent);
    return ESP_OK;
}

uint8_t link_display_backlight_get(void)
{
    return s_backlight_percent;
}

esp_err_t link_display_backlight_on(void)
{
    return link_display_backlight_set(100);
}

esp_err_t link_display_backlight_off(void)
{
    return link_display_backlight_set(0);
}
