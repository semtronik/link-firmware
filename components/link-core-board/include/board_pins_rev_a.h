/**
 * @file board_pins_rev_a.h
 * @brief Pin map for link-base-brd revision A.
 *
 * Generated from the schematic - do not hand-edit. Included through
 * board_pins.h, which picks a revision; never include this file
 * directly.
 */

#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_REV_STRING "A"

/* Display — SPI LCD */
#define BOARD_PIN_LCD_CS        GPIO_NUM_10
#define BOARD_PIN_LCD_MOSI      GPIO_NUM_11
#define BOARD_PIN_LCD_SCK       GPIO_NUM_12
#define BOARD_PIN_LCD_DC        GPIO_NUM_47
#define BOARD_PIN_LCD_RST       GPIO_NUM_39
#define BOARD_PIN_LCD_BL        GPIO_NUM_38

/* Capacitive touch panel */
#define BOARD_PIN_CTP_SCL       GPIO_NUM_17
#define BOARD_PIN_CTP_SDA       GPIO_NUM_18
#define BOARD_PIN_CTP_INT       GPIO_NUM_16
#define BOARD_PIN_CTP_RESET     GPIO_NUM_41

/* Core I2C bus */
#define BOARD_PIN_CORE_SCL      GPIO_NUM_13
#define BOARD_PIN_CORE_SDA      GPIO_NUM_14
#define BOARD_PIN_CORE_INT      GPIO_NUM_15

/* Power management */
#define BOARD_PIN_PWR_KEY       GPIO_NUM_9
#define BOARD_PIN_PWR_HOLD      GPIO_NUM_21
#define BOARD_PIN_CHG           GPIO_NUM_40
#define BOARD_PIN_PRES_USB      GPIO_NUM_42

/* Module expansion header */
#define BOARD_PIN_IO_MOD1       GPIO_NUM_1
#define BOARD_PIN_IO_MOD2       GPIO_NUM_2
#define BOARD_PIN_IO_MOD3       GPIO_NUM_3
#define BOARD_PIN_IO_MOD4       GPIO_NUM_4
#define BOARD_PIN_IO_MOD5       GPIO_NUM_5
#define BOARD_PIN_IO_MOD6       GPIO_NUM_6
#define BOARD_PIN_IO_MOD7       GPIO_NUM_7
#define BOARD_PIN_IO_MOD8       GPIO_NUM_8

/* Misc expansion */
#define BOARD_PIN_IO_EXT1       GPIO_NUM_48

/* USB */
#define BOARD_PIN_USB_DM        GPIO_NUM_19
#define BOARD_PIN_USB_DP        GPIO_NUM_20

/* UART0 */
#define BOARD_PIN_UART0_TXD     GPIO_NUM_43
#define BOARD_PIN_UART0_RXD     GPIO_NUM_44

/* Strapping */
#define BOARD_PIN_BOOT          GPIO_NUM_0

#ifdef __cplusplus
}
#endif
