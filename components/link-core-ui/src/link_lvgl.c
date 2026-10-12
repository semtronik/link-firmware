/**
 * @file link_lvgl.c
 * @brief LVGL runtime for the link base board.
 */

#include "link_lvgl.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "link_display.h"
#include "link_theme.h"
#include "link_touch.h"

static const char *TAG = "link_lvgl";

/*
 * Draw buffers: two twelfths of the panel, in internal DMA-capable RAM.
 *
 * PSRAM would leave internal RAM alone, but it is not reliably DMA
 * capable for the SPI master: esp_lcd would fall back to a bounce copy
 * on every flush. A twelfth keeps the pair near 26 kB, which the board
 * can spare, and pays for a direct transfer.
 *
 * lv_display_set_buffers() takes a size in BYTES and derives the line
 * count from it (8.3 took pixels): the panel is RGB565, so 2 B/px.
 */
#define LVGL_BUF_LINES  (LINK_DISPLAY_V_RES / 12)
#define LVGL_BUF_PIXELS (LINK_DISPLAY_H_RES * LVGL_BUF_LINES)
#define LVGL_BUF_BYTES  (LVGL_BUF_PIXELS * 2)

/* LVGL wants a tick well under its refresh period. */
#define LVGL_TICK_PERIOD_MS 2

/* Below the console REPL, so a busy screen never starves the shell. */
#define LVGL_TASK_PRIO 4

/*
 * Enough for the bring-up pages. LVGL 9 renders through a deep call
 * chain, and Xtensa register windows make every level of it cost, so
 * what a product needs scales with how deeply its widgets nest rather
 * than with how many it has. A product with a real UI overrides this
 * from its CMakeLists:
 *
 *   target_compile_definitions(${COMPONENT_LIB} PRIVATE LVGL_TASK_STACK=10240)
 *
 * The symptom of too little is a stack overflow in task "lvgl" on the
 * first frame, not a gradual slowdown.
 */
#ifndef LVGL_TASK_STACK
#define LVGL_TASK_STACK 6144
#endif
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 4

static SemaphoreHandle_t s_lock = NULL;
static bool s_running = false;
static TaskHandle_t s_task = NULL;
static lv_indev_t *s_indev = NULL;

#define FRAME_BYTES (LINK_DISPLAY_H_RES * LINK_DISPLAY_V_RES * 2)

#if CONFIG_LINK_SCREENSHOT
/* Partial rendering keeps no full frame: this one mirrors every flush. */
static uint8_t *s_frame = NULL;
#endif

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

#if CONFIG_LINK_SCREENSHOT
    if (s_frame != NULL) {
        const size_t row = (size_t)(area->x2 - area->x1 + 1) * 2;
        const uint8_t *src = px_map;

        for (int32_t y = area->y1; y <= area->y2; y++) {
            memcpy(s_frame + ((size_t)y * LINK_DISPLAY_H_RES + area->x1) * 2,
                   src, row);
            src += row;
        }
    }
#endif

    /* The buffer is already in the panel byte order: the display is
     * declared LV_COLOR_FORMAT_RGB565_SWAPPED, so LVGL rendered it that
     * way. No lv_draw_sw_rgb565_swap() pass here - it would swap a
     * second time, and cost a full read-modify-write per flush.
     *
     * draw_bitmap only queues: lv_display_flush_ready() is called from
     * the panel IO callback in link_display.c, once the pixels landed. */
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

/*
 * Interrupt context, called from link_touch.c. Only wakes the LVGL
 * task: the I2C read happens there, never here.
 */
static bool touch_isr_cb(void *arg)
{
    BaseType_t higher_woken = pdFALSE;

    vTaskNotifyGiveFromISR(s_task, &higher_woken);
    return higher_woken == pdTRUE;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;

    if (link_touch_read(&x, &y)) {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    while (true) {
        uint32_t next_ms = LVGL_TASK_MAX_DELAY_MS;

        if (link_lvgl_lock(UINT32_MAX)) {
            next_ms = lv_timer_handler();
            link_lvgl_unlock();
        }

        if (next_ms > LVGL_TASK_MAX_DELAY_MS) {
            next_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (next_ms < LVGL_TASK_MIN_DELAY_MS) {
            /* lv_timer_handler() can ask for 0: yield anyway, or this
             * task never lets the idle task run and the watchdog bites. */
            next_ms = LVGL_TASK_MIN_DELAY_MS;
        }

        /*
         * An edge cuts the wait short and the indev is read at once,
         * which is what keeps the pointer immediate. A contact already
         * under way is read on every pass too: LVGL only emits a click
         * on the PRESSED -> RELEASED transition, so it needs a read that
         * actually sees the finger gone, and the release raises no edge
         * of its own.
         *
         * Both are shortcuts on top of LVGL's read timer, which runs
         * regardless - see where the indev is created.
         */
        const bool pressed =
            s_indev != NULL &&
            lv_indev_get_state(s_indev) == LV_INDEV_STATE_PRESSED;

        const bool woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next_ms)) > 0;

        if ((woken || pressed) && s_indev != NULL &&
            link_lvgl_lock(UINT32_MAX)) {
            lv_indev_read(s_indev);
            link_lvgl_unlock();
        }
    }
}

bool link_lvgl_lock(uint32_t timeout_ms)
{
    if (s_lock == NULL) {
        return false;
    }

    const TickType_t ticks =
        (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE;
}

void link_lvgl_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

bool link_lvgl_is_running(void)
{
    return s_running;
}

bool link_lvgl_frame_copy(void *dst, size_t size)
{
#if CONFIG_LINK_SCREENSHOT
    if (s_frame == NULL || dst == NULL || size < FRAME_BYTES) {
        return false;
    }

    /* Under the lock, a refresh is never half flushed. */
    if (!link_lvgl_lock(1000)) {
        return false;
    }
    memcpy(dst, s_frame, FRAME_BYTES);
    link_lvgl_unlock();
    return true;
#else
    (void)dst;
    (void)size;
    return false;
#endif
}

void link_lvgl_buffer_info(unsigned *pixels, unsigned *bytes)
{
    if (pixels != NULL) {
        *pixels = (unsigned)LVGL_BUF_PIXELS;
    }
    if (bytes != NULL) {
        *bytes = (unsigned)LVGL_BUF_BYTES;
    }
}

esp_err_t link_lvgl_init(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }

    esp_lcd_panel_handle_t panel = link_display_panel();
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "panel not initialised");

    /* Recursive: a screen builder holding the lock may call helpers that
     * take it again. */
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "lock alloc failed");

    lv_init();
    /* Straight from lv_version.h, which LVGL ships in its own package:
     * this reports what the binary was actually built against, not what
     * idf_component.yml asked for. */
    ESP_LOGI(TAG, "LVGL %d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH);
    link_theme_init();

    /* heap_caps_malloc() returns at least 4-byte aligned blocks, which
     * satisfies LV_DRAW_BUF_ALIGN; lv_display_set_buffers() asserts on
     * it. */
    void *buf1 = heap_caps_malloc(LVGL_BUF_BYTES,
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(LVGL_BUF_BYTES,
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf1 != NULL && buf2 != NULL, ESP_ERR_NO_MEM, TAG,
                        "draw buffer alloc failed (%u B each)",
                        (unsigned)LVGL_BUF_BYTES);

#if CONFIG_LINK_SCREENSHOT
    s_frame = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (s_frame == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the screenshot frame, command disabled");
    }
#endif

    lv_display_t *disp = lv_display_create(LINK_DISPLAY_H_RES, LINK_DISPLAY_V_RES);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "display create failed");

    /*
     * The panel reads RGB565 MSB first. Declaring the format makes LVGL
     * render directly in that order - one pass, no swap in flush_cb().
     * This is the LVGL-side half of the "swap exactly once" rule;
     * link_display_fill() does its own, see link_display.c.
     */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_user_data(disp, panel);
    /* Rotation is done in hardware by link_display_panel_init(); LVGL
     * sees a native landscape panel and needs no rotation of its own. */
    lv_display_set_buffers(disp, buf1, buf2, LVGL_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (link_touch_handle() != NULL) {
        s_indev = lv_indev_create();
        ESP_RETURN_ON_FALSE(s_indev != NULL, ESP_FAIL, TAG, "indev create failed");
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, disp);

        /*
         * LVGL's own read timer is deliberately left running.
         *
         * CTP_INT is what makes the pointer feel immediate, and it does
         * work on this board - but for a reason that is not established
         * (see link_touch.c: the controller reports polling mode, yet
         * the line keeps producing edges). Depending on it alone would
         * make the touch stop dead if a different batch of controllers,
         * or a different firmware revision, behaved as documented.
         *
         * So the interrupt provides the latency and this timer provides
         * the guarantee: every LV_DEF_REFR_PERIOD the indev is read
         * whatever the line did. Losing the edges would cost
         * responsiveness, never the touch itself.
         */
    } else {
        ESP_LOGW(TAG, "touch not initialised - screens will not respond");
    }

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG,
                        "tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_PERIOD_MS * 1000),
                        TAG, "tick timer start failed");

    /*
     * Hand the panel over only now: the IO callback starts calling
     * lv_display_flush_ready() on this handle the moment it is set, so
     * the display must already exist and carry its flush callback.
     */
    link_display_attach_lvgl(disp);
    s_running = true;

    ESP_RETURN_ON_FALSE(xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL,
                                    LVGL_TASK_PRIO, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "lvgl task create failed");

    /* Only now: the ISR notifies s_task, which must exist first. */
    if (s_indev != NULL) {
        ESP_RETURN_ON_ERROR(link_touch_set_isr_callback(touch_isr_cb, NULL), TAG,
                            "touch interrupt arm failed");
    }

    ESP_LOGI(TAG, "running: %u px buffers, %u B each",
             (unsigned)LVGL_BUF_PIXELS, (unsigned)LVGL_BUF_BYTES);
    return ESP_OK;
}
