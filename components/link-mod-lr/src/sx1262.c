/**
 * @file sx1262.c
 * @brief SX1262 transport: SPI commands, registers, reset, DIO1.
 *
 * See sx1262.h for what this is and is not.
 */

#include "sx1262.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sx1262";

/* The mezzanine mates row-swapped: base pin 11 touches module pin 12.
 * Reading the schematics by pin number gives a wrong map. */
#define SX1262_PIN_MISO  BOARD_PIN_IO_MOD1
#define SX1262_PIN_SCK   BOARD_PIN_IO_MOD2
#define SX1262_PIN_MOSI  BOARD_PIN_IO_MOD3
#define SX1262_PIN_RF_SW BOARD_PIN_IO_MOD4
#define SX1262_PIN_NRST  BOARD_PIN_IO_MOD5
#define SX1262_PIN_NSS   BOARD_PIN_IO_MOD6
#define SX1262_PIN_DIO1  BOARD_PIN_IO_MOD7
#define SX1262_PIN_BUSY  BOARD_PIN_IO_MOD8

/* Not SPI2: the display has that one, and a host can only be
 * initialised once even though the two sit on different pins. */
#define SX1262_SPI_HOST SPI3_HOST

#define SX1262_SPI_HZ (8 * 1000 * 1000)

#define CMD_WRITE_REGISTER 0x0D
#define CMD_READ_REGISTER  0x1D
#define CMD_READ_BUFFER    0x1E

/* A command sent while BUSY is high is silently dropped. */
#define SX1262_BUSY_TIMEOUT_MS 10U

/* NRST must be held low well past the datasheet's typical 100 us. A busy
 * wait, not vTaskDelay: at a 10 ms tick, pdMS_TO_TICKS(1) is 0 and the
 * pulse would last microseconds. */
#define SX1262_RESET_PULSE_US 1000U

/* opcode + 2 address bytes + a full 255-byte LoRa payload. */
#define SX1262_MAX_XFER 260U

static spi_device_handle_t s_dev = NULL;
static bool s_ready = false;

/*
 * One command at a time on the bus. The chip is reached from more than
 * one task, and without this a second caller could refill the shared DMA
 * buffers before they are clocked out, or raise NSS mid-command, which
 * the chip reads as end of command.
 */
static SemaphoreHandle_t s_bus_lock;

static SemaphoreHandle_t s_dio1_sem = NULL;

/* Interrupt instants, microseconds since boot, written by the ISR and
 * consumed by whoever is waiting. Four is more than a LoRa reception
 * can produce before the task has had its turn. */
#define SX1262_STAMP_SLOTS 4
static volatile int64_t s_stamp[SX1262_STAMP_SLOTS];
static volatile uint8_t s_stamp_head;
static uint8_t s_stamp_tail;

bool sx1262_dio1_irq_stamp(int64_t *out_us)
{
    if (s_stamp_tail == s_stamp_head) {
        return false;
    }

    *out_us = s_stamp[s_stamp_tail];
    s_stamp_tail = (uint8_t)((s_stamp_tail + 1) % SX1262_STAMP_SLOTS);
    return true;
}

static void IRAM_ATTR dio1_isr(void *arg)
{
    BaseType_t woken = pdFALSE;

    /*
     * Stamped here, not in the task.
     *
     * The line goes up within microseconds of the event; a task waking
     * on it is minutes late by comparison - scheduling latency, then an
     * SPI read to find out which event it was. Taking the time in the
     * interrupt keeps the instant, and lets the task work out what it
     * belonged to afterwards at its leisure.
     *
     * A small ring rather than one slot: two events can arrive between
     * two wake-ups, and the second would otherwise overwrite the first.
     */
    const uint8_t head = s_stamp_head;
    s_stamp[head] = esp_timer_get_time();
    s_stamp_head = (uint8_t)((head + 1) % SX1262_STAMP_SLOTS);

    if (s_dio1_sem != NULL) {
        xSemaphoreGiveFromISR(s_dio1_sem, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t sx1262_dio1_irq_enable(SemaphoreHandle_t *out)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_dio1_sem != NULL) {
        *out = s_dio1_sem;
        return ESP_OK;
    }

    s_dio1_sem = xSemaphoreCreateBinary();
    if (s_dio1_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_stamp_head = 0;
    s_stamp_tail = 0;

    esp_err_t err = gpio_set_intr_type(SX1262_PIN_DIO1, GPIO_INTR_POSEDGE);
    if (err != ESP_OK) {
        goto fail;
    }

    /* The service may already be installed by another driver. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }

    err = gpio_isr_handler_add(SX1262_PIN_DIO1, dio1_isr, NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    *out = s_dio1_sem;
    ESP_LOGI(TAG, "DIO1 interrupt armed on GPIO%d", (int)SX1262_PIN_DIO1);
    return ESP_OK;

fail:
    vSemaphoreDelete(s_dio1_sem);
    s_dio1_sem = NULL;
    return err;
}

static esp_err_t wait_busy_low(void)
{
    const int64_t deadline =
        esp_timer_get_time() + (int64_t)SX1262_BUSY_TIMEOUT_MS * 1000;

    while (gpio_get_level(SX1262_PIN_BUSY) != 0) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }

    }
    return ESP_OK;
}

/*
 * DMA needs internal, word-aligned RAM, which a caller's stack array is
 * not guaranteed to be, so every transfer is copied through these.
 * Shared state: only touched under s_bus_lock.
 */
static WORD_ALIGNED_ATTR uint8_t s_dma_tx[SX1262_MAX_XFER];
static WORD_ALIGNED_ATTR uint8_t s_dma_rx[SX1262_MAX_XFER];

static esp_err_t transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || len > SX1262_MAX_XFER) {
        return ESP_ERR_INVALID_ARG;
    }

    /* From here to the give below is one command, indivisible: the wait
     * on BUSY belongs inside it, since BUSY reads low between two of
     * another caller's transfers as readily as it does when the chip is
     * idle. */
    if (s_bus_lock != NULL) {
        xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    }

    esp_err_t err = wait_busy_low();
    if (err != ESP_OK) {
        if (s_bus_lock != NULL) {
            xSemaphoreGive(s_bus_lock);
        }
        /* Debug: a wedged chip fails every command, and the console
         * shares its link with the protocol. The caller reports it. */
        ESP_LOGD(TAG, "BUSY still high after %u ms - chip not answering",
                 (unsigned)SX1262_BUSY_TIMEOUT_MS);
        return err;
    }

    memcpy(s_dma_tx, tx, len);

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = s_dma_tx,
        .rx_buffer = s_dma_rx,
    };

    gpio_set_level(SX1262_PIN_NSS, 0);
    err = spi_device_polling_transmit(s_dev, &t);
    gpio_set_level(SX1262_PIN_NSS, 1);

    if (err == ESP_OK && rx != NULL) {
        memcpy(rx, s_dma_rx, len);
    }

    if (s_bus_lock != NULL) {
        xSemaphoreGive(s_bus_lock);
    }

    return err;
}

esp_err_t sx1262_cmd_write(uint8_t opcode, const uint8_t *args, size_t len)
{
    uint8_t tx[SX1262_MAX_XFER] = {0};
    uint8_t rx[SX1262_MAX_XFER] = {0};

    if (len + 1 > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }

    tx[0] = opcode;
    if (len > 0) {
        memcpy(&tx[1], args, len);
    }

    return transfer(tx, rx, len + 1);
}

/*
 * Byte 0 is clocked out with the opcode; the Get* commands then emit a
 * status byte of their own, so the data starts at byte 2. GetStatus is
 * the exception - its data is that first byte - and does not fit here.
 */
esp_err_t sx1262_cmd_read_status(uint8_t opcode, uint8_t *out, size_t len)
{
    uint8_t tx[SX1262_MAX_XFER] = {0};
    uint8_t rx[SX1262_MAX_XFER] = {0};

    if (len + 2 > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }

    tx[0] = opcode;

    esp_err_t err = transfer(tx, rx, len + 2);
    if (err == ESP_OK && len > 0) {
        memcpy(out, &rx[2], len);
    }
    return err;
}

esp_err_t sx1262_reg_write(uint16_t addr, const uint8_t *data, size_t len)
{
    uint8_t tx[SX1262_MAX_XFER] = {0};
    uint8_t rx[SX1262_MAX_XFER] = {0};

    if (len + 3 > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }

    tx[0] = CMD_WRITE_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)(addr & 0xFF);
    memcpy(&tx[3], data, len);

    return transfer(tx, rx, len + 3);
}

esp_err_t sx1262_reg_read(uint16_t addr, uint8_t *data, size_t len)
{
    uint8_t tx[SX1262_MAX_XFER] = {0};
    uint8_t rx[SX1262_MAX_XFER] = {0};

    /* opcode + 2 address bytes + 1 NOP before data. */
    if (len + 4 > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }

    tx[0] = CMD_READ_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)(addr & 0xFF);

    esp_err_t err = transfer(tx, rx, len + 4);
    if (err == ESP_OK) {
        memcpy(data, &rx[4], len);
    }
    return err;
}

esp_err_t sx1262_buffer_read(uint8_t offset, uint8_t *data, size_t len)
{
    uint8_t tx[SX1262_MAX_XFER] = {0};
    uint8_t rx[SX1262_MAX_XFER] = {0};

    /* opcode + offset + 1 NOP before data. */
    if (len + 3 > sizeof(tx)) {
        return ESP_ERR_INVALID_ARG;
    }

    tx[0] = CMD_READ_BUFFER;
    tx[1] = offset;

    esp_err_t err = transfer(tx, rx, len + 3);
    if (err == ESP_OK) {
        memcpy(data, &rx[3], len);
    }
    return err;
}

esp_err_t sx1262_reset(uint32_t *busy_fell_us)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_set_level(SX1262_PIN_NRST, 0);
    esp_rom_delay_us(SX1262_RESET_PULSE_US);

    const int64_t released = esp_timer_get_time();
    gpio_set_level(SX1262_PIN_NRST, 1);


    const int64_t deadline = released + 1000 * 1000; /* 1 s, generous */
    while (gpio_get_level(SX1262_PIN_BUSY) != 0) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    if (busy_fell_us != NULL) {
        *busy_fell_us = (uint32_t)(esp_timer_get_time() - released);
    }
    return ESP_OK;
}

esp_err_t sx1262_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }


    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << SX1262_PIN_NSS) |
                        (1ULL << SX1262_PIN_NRST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(SX1262_PIN_NSS, 1);   /* idle high, chip deselected */
    gpio_set_level(SX1262_PIN_NRST, 0);  /* held in reset */


    /* RF_SW takes nothing internal: R3 holds it high, which is the
     * state the module needs, and a pull-down would fight it. */
    const gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << SX1262_PIN_BUSY) | (1ULL << SX1262_PIN_DIO1) |
                        (1ULL << SX1262_PIN_RF_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = SX1262_PIN_MOSI,
        .miso_io_num = SX1262_PIN_MISO,
        .sclk_io_num = SX1262_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SX1262_MAX_XFER,
    };
    /*
     * DMA, and not for speed: without it a transfer is capped at the
     * host's 64-byte registers, whatever max_transfer_sz says, and
     * reading a payload out needs up to 258 bytes in one go. Frames past
     * about sixty bytes would be silently lost.
     */
    err = spi_bus_initialize(SX1262_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    /* spics_io_num = -1: NSS is driven by hand in transfer(). */
    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = SX1262_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SX1262_SPI_HOST, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        spi_bus_free(SX1262_SPI_HOST);
        return err;
    }

    /* Before s_ready: transfer() only takes the lock when it exists, so
     * a caller arriving between the two would slip through unguarded. */
    if (s_bus_lock == NULL) {
        s_bus_lock = xSemaphoreCreateMutex();
        if (s_bus_lock == NULL) {
            spi_bus_remove_device(s_dev);
            s_dev = NULL;
            spi_bus_free(SX1262_SPI_HOST);
            return ESP_ERR_NO_MEM;
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "SPI up at %d kHz on SCK=%d MISO=%d MOSI=%d, NSS=%d",
             SX1262_SPI_HZ / 1000, (int)SX1262_PIN_SCK, (int)SX1262_PIN_MISO,
             (int)SX1262_PIN_MOSI, (int)SX1262_PIN_NSS);
    ESP_LOGI(TAG, "NRST=%d BUSY=%d DIO1=%d, chip held in reset",
             (int)SX1262_PIN_NRST, (int)SX1262_PIN_BUSY, (int)SX1262_PIN_DIO1);
    ESP_LOGI(TAG, "RF_SW=%d not driven, reads %d",
             (int)SX1262_PIN_RF_SW, gpio_get_level(SX1262_PIN_RF_SW));

    return ESP_OK;
}
