#include "screens_text.h"

#include <stdbool.h>

#include "link_i18n.h"

static const link_i18n_row_t TEXTS[ST_COUNT] = {
    [ST_BACK] = {"Retour", "Back"},
    [ST_OK] = {"OK", "OK"},

    [ST_PREFERENCES] = {"Préférences", "Preferences"},
    [ST_THEME] = {"Thème", "Theme"},
    [ST_THEME_LIGHT] = {"Clair", "Light"},
    [ST_THEME_DARK] = {"Sombre", "Dark"},
    [ST_BACKLIGHT] = {"Luminosité", "Brightness"},
    [ST_LANGUAGE] = {"Langue", "Language"},

    [ST_ABOUT] = {"À propos", "About"},
    [ST_BOARD] = {"Carte", "Board"},

    [ST_BATTERY_LOW] = {"BATTERIE FAIBLE", "LOW BATTERY"},
    [ST_BATTERY_LOW_HINT] = {"Pensez à recharger", "Please recharge"},
    [ST_BATTERY_CRITICAL] = {"BATTERIE CRITIQUE", "BATTERY CRITICAL"},
    [ST_BATTERY_CRITICAL_HINT] = {"Arrêt automatique…", "Shutting down…"},

    [ST_DATE_TIME] = {"Date et heure", "Date and time"},
    [ST_ZONE] = {"Fuseau", "Time zone"},
    [ST_DATE] = {"Date", "Date"},
    [ST_TIME] = {"Heure", "Time"},
    [ST_CLOCK_STATE] = {"État", "Status"},
    [ST_CLOCK_NOT_SET] = {"à régler", "not set"},

    [ST_POWER_OFF] = {"Éteindre ?", "Power off?"},

    [ST_FACTORY_RESET_Q] = {"Retour usine ?", "Factory reset?"},
    [ST_FACTORY_RESET_HINT] = {"Tous les réglages, l'heure,\nles enregistrements et le\n"
                               "journal seront effacés.\n\n"
                               "L'appareil redémarre ensuite.",
                               "All settings, the clock,\nthe recordings and the\n"
                               "event log will be erased.\n\n"
                               "The device then restarts."},
    [ST_CANCEL] = {"Annuler", "Cancel"},
    [ST_ERASE] = {"Effacer", "Erase"},

    [ST_SYSINFO] = {"État système", "System status"},
    [ST_BATTERY] = {"Batterie", "Battery"},
    [ST_POWER] = {"Charge", "Power"},
    [ST_UPTIME] = {"Allumé depuis", "Uptime"},
    [ST_GAUGE_SILENT] = {"jauge muette", "no gauge reading"},
    [ST_BATTERY_ABSENT] = {"absente", "absent"},
    [ST_BATTERY_DETECTING] = {"détection…", "detecting…"},
    [ST_POWER_DISCHARGING] = {"sur batterie", "on battery"},
    [ST_POWER_CHARGING] = {"en charge", "charging"},
    [ST_POWER_CHARGED] = {"chargée", "charged"},
    [ST_POWER_NO_BATTERY] = {"pas de batterie", "no battery"},

    [ST_ONB_TITLE] = {"Régler l'heure", "Set the clock"},
    [ST_ONB_CURRENT] = {"Heure actuelle :", "Current time:"},
    [ST_ONB_HINT] = {"Nécessaire pour horodater\nles trames reçues",
                     "Needed to timestamp\nreceived frames"},
    [ST_ONB_MANUAL] = {"Régler manuellement", "Set manually"},
    [ST_ONB_APP] = {"Connecter l'app", "Connect the app"},
    [ST_ONB_WAITING] = {"EN ATTENTE", "WAITING"},
    [ST_ONB_WAITING_HINT] = {"Branchez le câble USB et\nouvrez Link Studio.\n\n"
                             "L'heure sera réglée\nautomatiquement.",
                             "Plug in the USB cable\nand open Link Studio.\n\n"
                             "The clock will be set\nautomatically."},
};

const char *st(st_id_t id)
{
    static bool checked;
    if (!checked) {
        checked = true;
        link_i18n_check(TEXTS, ST_COUNT, "screens");
    }

    return link_i18n_get(TEXTS, ST_COUNT, id);
}
