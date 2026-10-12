/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"
#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "st7789t_panel.h"

static const char *TAG = "lcd_panel.st7789t";

static esp_err_t panel_st7789t_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789t_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789t_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789t_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st7789t_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7789t_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7789t_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7789t_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7789t_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_cal; // save surrent value of LCD_CMD_COLMOD register
} st7789t_panel_t;

esp_err_t esp_lcd_new_panel_st7789t(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_st7789t_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    st7789t_panel_t *st7789t = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    st7789t = calloc(1, sizeof(st7789t_panel_t));
    ESP_GOTO_ON_FALSE(st7789t, ESP_ERR_NO_MEM, err, TAG, "no mem for st7789t panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7789t->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7789t->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7789t->colmod_cal = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        st7789t->colmod_cal = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7789t->io = io;
    st7789t->fb_bits_per_pixel = fb_bits_per_pixel;
    st7789t->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7789t->reset_level = panel_dev_config->flags.reset_active_high;
    st7789t->base.del = panel_st7789t_del;
    st7789t->base.reset = panel_st7789t_reset;
    st7789t->base.init = panel_st7789t_init;
    st7789t->base.draw_bitmap = panel_st7789t_draw_bitmap;
    st7789t->base.invert_color = panel_st7789t_invert_color;
    st7789t->base.set_gap = panel_st7789t_set_gap;
    st7789t->base.mirror = panel_st7789t_mirror;
    st7789t->base.swap_xy = panel_st7789t_swap_xy;
    st7789t->base.disp_on_off = panel_st7789t_disp_on_off;
    *ret_panel = &(st7789t->base);
    ESP_LOGD(TAG, "new st7789t panel @%p", st7789t);
    // printf("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n");
    return ESP_OK;

err:
    if (st7789t) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7789t);
    }
    return ret;
}

static esp_err_t panel_st7789t_del(esp_lcd_panel_t *panel)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);

    if (st7789t->reset_gpio_num >= 0) {
        gpio_reset_pin(st7789t->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7789t panel @%p", st7789t);
    free(st7789t);
    return ESP_OK;
}

static esp_err_t panel_st7789t_reset(esp_lcd_panel_t *panel)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789t->io;

    // perform hardware reset
    /* Timings : 100 ms RESX bas puis 100 ms RESX haut. Alignés sur l'exemple
     * Newhaven officiel (NHD-2.8C-CSXP-BREAKOUT.pdf p.12). Datasheet ST7789VI
     * §7.4.5 p.48 exige tRES > 10 µs et trrest > 120 ms avant d'envoyer la
     * première commande — 100 ms haut + 120 ms après SLPOUT couvrent large. */
    if (st7789t->reset_gpio_num >= 0) {
        gpio_set_level(st7789t->reset_gpio_num, st7789t->reset_level);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(st7789t->reset_gpio_num, !st7789t->reset_level);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else { // perform software reset
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

static esp_err_t panel_st7789t_init(esp_lcd_panel_t *panel)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789t->io;

    /* Séquence d'init NHD-2.8C-CSXP (ST7789VI), reprise de l'exemple officiel
     * Newhaven : lft-poc-notes/docs-externes/NHD-2.8C-CSXP-BREAKOUT.pdf p.12
     * ("Example Initialization Code"). Refs datasheet : ST7789VI v1.6, page
     * indiquée pour chaque cmd. Décodage bit-par-bit + comparaison aux valeurs
     * reset : lft-poc-notes/init-st7789vi-decodee.md
     *
     * La plupart des params rejouent les valeurs reset par robustesse. Les
     * vraies divergences du reset, qui rendent ce panneau utilisable, sont :
     *   COLMOD=55h    → 16bpp RGB565 (reset = 06h / 18bpp)
     *   VCOMS=2Bh     → 1.175 V     (reset = 20h / 0.7 V)
     *   VRHS=11h      → VAP/VAN +   (reset = 0Bh)
     *   PWCTRL1=A1h   → AVDD 6.8 V, AVCL -4.8 V (reset = 81h)
     *   E0h/E1h gamma → courbe panneau spécifique
     *   INVON         → obligatoire IPS
     */

    /* SLPOUT (11h) — Sleep Out. Datasheet §9.1.12 p.184.
     * Démarre oscillateur + boosters. Délai 120 ms requis avant la commande
     * suivante (datasheet : tSLPOUT > 5 ms strict, 120 ms si on veut SLPIN
     * juste après). */
    esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* COLMOD (3Ah) = 55h — Interface Pixel Format. Datasheet §9.1.32 p.224.
     * D6:D4=101 (RGB i/f 65k, ignoré en MCU), D2:D0=101 → MCU 16bpp RGB565.
     * Reset = 06h (18bpp) → on FORCE 16bpp pour économiser ⅓ du bus SPI. */
    esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t []){0x55}, 1);

    /* MADCTL (36h) — Memory Data Access Control. Datasheet §9.1.28 p.215.
     * On pousse madctl_val tenu à jour par le driver (bit RGB/BGR posé au
     * constructeur via rgb_ele_order, bits MX/MY/MV via mirror()/swap_xy()
     * appelés depuis LCD_Panel_Init). Hardcoder ici ferait perdre les bits
     * positionnés à runtime. */
    esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t []){st7789t->madctl_val}, 1);

    /* RAMCTRL (B0h) intentionnellement non envoyé : valeur reset 00h/F0h
     * (MCU interface, MSB-first sur bus, EPF=11) déjà correcte. L'exemple
     * officiel Newhaven ne l'envoie pas non plus. Le swap d'octets RGB565
     * pour matcher le bus MSB-first est géré côté LVGL, via le format
     * LV_COLOR_FORMAT_RGB565_SWAPPED du display (cf. link_lvgl.c). */

    /* PORCTRL (B2h) = {0Ch, 0Ch, 00h, 33h, 33h} — Porch Setting. §9.2.3 p.263.
     * 12 lignes back+front porch en normal, PSEN=0 (idle/partial = normal).
     * Reset DS identique. Détermine le fps avec FRCTRL2 (cf. plus bas). */
    esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t []){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5);

    /* GCTRL (B7h) = 35h — Gate Control. Datasheet §9.2.6 p.268.
     * VGHS=011 → VGH = +13.26 V. VGLS=101 → VGL = -10.43 V.
     * Reset DS = 35h → on rejoue le reset. */
    esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t []){0x35}, 1);

    /* VCOMS (BBh) = 2Bh — VCOM Setting. Datasheet §9.2.9 p.273.
     * VCOMS[5:0]=2Bh → VCOM = 0.1 + 43×0.025 = +1.175 V.
     * Reset DS = 20h (0.7 V) → on monte de +475 mV : tuning spécifique
     * panneau Newhaven pour minimiser le flicker. À ne pas changer sans
     * mesure visuelle.
     * Contrainte DS : VCOMS + VCOMS_offset + VDV ∈ [0.1 V, 1.675 V]. */
    esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t []){0x2B}, 1);

    /* LCMCTRL (C0h) = 2Ch — LCM Control. Datasheet §9.2.10 p.275.
     * XBGR=1, XMX=1, XMH=1 : XORs appliqués aux bits correspondants de
     * MADCTL/INVON par le contrôleur. Reset DS = 2Ch → rejoué tel quel,
     * effet net nul ici (on raisonne directement sur madctl_val). */
    esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t []){0x2C}, 1);

    /* VDVVRHEN (C2h) = {01h, FFh} — VDV/VRH command enable. §9.2.12 p.277.
     * CMDEN=1 → autorise C3h/C4h à écrire (sinon ils utilisent les valeurs
     * NVM usine et nos VRHS/VDVS sont ignorés). Reset DS = 01h/FFh. */
    esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t []){0x01, 0xFF}, 2);

    /* VRHS (C3h) = 11h — VRH Set. Datasheet §9.2.13 p.278.
     * VRHS[5:0]=11h → VAP ≈ 3.55 + 17×0.05 + VCOM_term ≈ 5.58 V,
     * VAN ≈ -3.23 V. Reset DS = 0Bh → on monte VAP de ~300 mV : meilleur
     * contraste, stabilité à 60 Hz. Valeur Newhaven officielle. */
    esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t []){0x11}, 1);

    /* VDVS (C4h) = 20h — VDV Set. Datasheet §9.2.14 p.280.
     * VDVS[5:0]=20h → VDV = 0 V (centre de la plage [-0.8 V, +0.775 V]) :
     * sources positives et négatives parfaitement symétriques.
     * Reset DS = 20h → rejoué. */
    esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t []){0x20}, 1);

    /* FRCTRL2 (C6h) = 0Fh — Frame Rate Control normal mode. §9.2.16 p.284.
     * NLA=000 (dot inversion, best pour IPS), RTNA=0Fh → 60 Hz.
     * Formule (avec porches B2h): fps = 10MHz/(320+12+12)×(250+15×16) ≈ 60.
     * Reset DS = 0Fh → rejoué. */
    esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t []){0x0F}, 1);

    /* PWCTRL1 (D0h) = {A4h, A1h} — Power Control 1. §9.2.21 p.290.
     * Byte 1 = A4h : signature fixe imposée par la DS (à copier tel quel).
     * Byte 2 = A1h : AVDD=6.8 V (D7:D6=10), AVCL=-4.8 V (D5:D4=10),
     *               VDDS=2.3 V (D1:D0=01).
     * Reset DS = A4h/81h → on AUGMENTE swing source de ±400 mV pour mieux
     * piloter la capa parasite du panneau Newhaven 2.8". */
    esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t []){0xA4, 0xA1}, 2);

    /* GAMSET (26h) retiré : sélecteur de gamma préprogrammée, redondant
     * avec PVGAMCTRL/NVGAMCTRL custom ci-dessous. */

    /* PVGAMCTRL (E0h) — Positive Voltage Gamma Control. §9.2.25 p.295.
     * Courbe gamma + custom 14 octets calibrée pour ce panneau IPS Newhaven
     * (valeurs officielles Newhaven, datasheet panneau p.12). NE PAS modifier sans
     * pattern test (dégradé linéaire) + colorimètre. */
    esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t []){0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}, 14);

    /* NVGAMCTRL (E1h) — Negative Voltage Gamma Control. §9.2.26 p.297.
     * Pendant négatif de E0h pour les trames de polarité inverse (dot
     * inversion). Désappariement P/N → flicker lent 1-2 Hz. */
    esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t []){0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}, 14);

    /* CMD2EN (DFh) et RGBCTRL (B1h) absents : utiles seulement en mode RGB
     * parallèle direct, aucun effet en interface SPI MCU. L'exemple Newhaven
     * ne les envoie pas non plus. */

    /* INVON (21h) — Display Inversion ON. Datasheet §9.1.16 p.190.
     * Obligatoire pour panneau IPS (sans ça : couleurs en négatif). */
    esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0);

    /* DISPON (29h) — Display ON. Datasheet §9.1.19 p.196.
     * Active la sortie RGB vers le panneau. Avant cette commande l'écran
     * reste noir même si la RAM contient une image. */
    esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0);

    return ESP_OK;
}

static esp_err_t panel_st7789t_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = st7789t->io;

    x_start += st7789t->x_gap;
    x_end += st7789t->x_gap;
    y_start += st7789t->y_gap;
    y_end += st7789t->y_gap;

    // define an area of frame memory where MCU can access
    esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4);
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st7789t->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_st7789t_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789t->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_st7789t_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789t->io;
    if (mirror_x) {
        st7789t->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7789t->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7789t->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7789t->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789t->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_st7789t_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789t->io;
    if (swap_axes) {
        st7789t->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7789t->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789t->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_st7789t_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    st7789t->x_gap = x_gap;
    st7789t->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7789t_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7789t_panel_t *st7789t = __containerof(panel, st7789t_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789t->io;
    int command = 0;
    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}
