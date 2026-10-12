#include "ui_text.h"

#include <stdbool.h>

#include "link_i18n.h"

static const link_i18n_row_t TEXTS[T_COUNT] = {
    [T_PROFILE_1] = {"Profil 1", "Profile 1"},
    [T_PROFILE_2] = {"Profil 2", "Profile 2"},
    [T_PROFILE_3] = {"Profil 3", "Profile 3"},
    [T_RADIO_PROFILE] = {"Profil radio", "Radio profile"},

    [T_REGION] = {"Région", "Region"},
    [T_FREQUENCY] = {"Fréquence", "Frequency"},
    [T_SPREADING] = {"Étalement", "Spreading factor"},
    [T_BANDWIDTH] = {"Bande passante", "Bandwidth"},
    [T_CODING] = {"Codage", "Coding rate"},
    [T_SYNC_WORD] = {"Mot de synchro", "Sync word"},
    [T_POWER] = {"Puissance", "Power"},
    [T_PREAMBLE] = {"Préambule", "Preamble"},
    [T_RX_GAIN] = {"Gain de réception", "RX gain"},
    [T_OFF] = {"Inactif", "Off"},
    [T_ON] = {"Actif", "On"},
    [T_IQ_STANDARD] = {"Standard", "Standard"},
    [T_IQ_INVERTED] = {"Inversé", "Inverted"},
    [T_GAIN_SAVING] = {"Économie", "Power saving"},
    [T_GAIN_BOOSTED] = {"Renforcé", "Boosted"},
    [T_SYMBOLS_SHORT] = {"symb.", "sym."},

    [T_MENU] = {"Menu", "Menu"},
    [T_SCAN] = {"Balayage", "Band scan"},
    [T_PREFERENCES] = {"Préférences", "Preferences"},
    [T_DATE_TIME] = {"Date et heure", "Date and time"},
    [T_SYSINFO] = {"État système", "System status"},
    [T_ABOUT] = {"À propos", "About"},
    [T_FACTORY_RESET] = {"Retour usine", "Factory reset"},

    [T_SCAN_TUNE_FAILED] = {"accord KO", "tuning failed"},
    [T_SCAN_MEASURING] = {"mesure…", "measuring…"},

    [T_SYS_RADIO] = {"Radio", "Radio"},
    [T_SYS_HEALTH] = {"Contrôle", "Health"},
    [T_SYS_REC] = {"Enreg.", "Rec."},
    [T_RADIO_LISTENING] = {"à l'écoute", "listening"},
    [T_RADIO_STOPPED] = {"arrêtée", "stopped"},
    [T_REC_STOPPED] = {"arrêté (heure ?)", "stopped (clock?)"},
    [T_HEALTH_FMT] = {"%lu redém.  %lu us  %lu anom.",
                      "%lu resets  %lu us  %lu anom."},
    [T_REC_LOST_FMT] = {"S%lu  %lu l.  %u ko  %lu perdues",
                        "S%lu  %lu ln  %u kB  %lu lost"},
    [T_REC_FMT] = {"S%lu  %lu l.  %u ko", "S%lu  %lu ln  %u kB"},

    [T_VIEW_JOURNAL] = {"Journal", "Log"},
    [T_VIEW_STATS] = {"Stats", "Stats"},
    [T_FILTER_ALL] = {"Tous", "All"},
    [T_EMPTY_PAYLOAD] = {"(vide)", "(empty)"},
    [T_LOG_TAIL_FMT] = {"%d dernières trames affichées",
                        "Last %d frames shown"},
    [T_NO_FRAME] = {"Aucune trame", "No frames"},
    [T_YES] = {"oui", "yes"},
    [T_NO] = {"non", "no"},
    [T_DATA] = {"DONNÉES", "DATA"},
    [T_DATA_ONE] = {"DONNÉES (1 octet)", "DATA (1 byte)"},
    [T_DATA_FMT] = {"DONNÉES (%u octets)", "DATA (%u bytes)"},
    [T_FRAME_INCOMPLETE_LONG] = {"TRAME INCOMPLÈTE - aucun octet reçu",
                                 "INCOMPLETE FRAME - no byte received"},
    [T_HEADER_INVALID_LONG] = {"EN-TÊTE INVALIDE - aucun octet reçu",
                               "INVALID HEADER - no byte received"},
    [T_CRC_FAILED_LONG] = {"CRC ÉCHOUÉ - octets non fiables",
                           "CRC FAILED - bytes unreliable"},
    [T_FRAME_DETAILS] = {"Détails trame", "Frame details"},
    [T_SECTION_MARGIN] = {"MARGE SNR", "SNR MARGIN"},
    [T_SNR_MEASURED] = {"SNR mesuré", "Measured SNR"},
    [T_SNR_LIMIT_FMT] = {"Limite SF%u", "SF%u limit"},
    [T_MARGIN] = {"Marge", "Margin"},
    [T_MARGIN_EXPLAIN] = {"La marge est le SNR mesuré moins la limite de "
                          "démodulation du SF. Plus elle est haute, plus la "
                          "trame avait de réserve.",
                          "The margin is the measured SNR minus the "
                          "demodulation limit of the SF. The higher it is, "
                          "the more headroom the frame had."},
    [T_SECTION_CHANNEL] = {"CANAL PENDANT LA TRAME", "CHANNEL DURING THE FRAME"},
    [T_SIGNAL] = {"Signal", "Signal"},
    [T_NOISE_BEFORE] = {"Bruit avant", "Noise before"},
    [T_READINGS_FMT] = {"Lectures (%u)", "Readings (%u)"},
    [T_RANGE_DBM_FMT] = {"%d à %d dBm", "%d to %d dBm"},
    [T_SECTION_FRAME] = {"TRAME", "FRAME"},
    [T_FREQ_OFFSET] = {"Écart de fréquence", "Frequency offset"},
    [T_CODING_RATE] = {"Taux de codage", "Coding rate"},
    [T_TIME_ON_AIR] = {"Durée", "Time on air"},
    [T_PROFILE_DIFFERS_FMT] = {"%s (profil %s)", "%s (profile %s)"},
    [T_RADIO_PROFILE_CAPS] = {"PROFIL RADIO", "RADIO PROFILE"},
    [T_NONE] = {"aucun", "none"},
    [T_CAPTURE_SETTINGS] = {"Réglages capture", "Capture settings"},
    [T_PROTOCOL] = {"PROTOCOLE", "PROTOCOL"},
    [T_ID_FILTER] = {"FILTRE ID", "ID FILTER"},
    [T_TEST_FRAME] = {"Trame de test", "Test frame"},
    [T_FRAME_TO_SEND] = {"Trame à envoyer", "Frame to send"},
    [T_CRC_ERROR] = {"Erreur CRC", "CRC error"},
    [T_FRAME_INCOMPLETE] = {"Trame incomplète", "Incomplete frame"},
    [T_HEADER_INVALID] = {"En-tête invalide", "Invalid header"},
    [T_SYNC_SHORT] = {"synchro", "sync"},

    [T_ERRORS] = {"Erreurs", "Errors"},
    [T_DURATION_S_FMT] = {"%ld s", "%ld s"},
    [T_DURATION_MIN_FMT] = {"%ld min %02ld s", "%ld min %02ld s"},
    [T_DURATION_H_FMT] = {"%ld h %02ld min", "%ld h %02ld min"},
    [T_SESSION_FMT] = {"Session %lu", "Session %lu"},
    [T_NO_SESSION] = {"Pas encore de session", "No session yet"},
    [T_RECEIVED] = {"Reçues", "Received"},
    [T_RECEIVED_FMT] = {"%lu reçues", "%lu received"},
    [T_SENT] = {"Émises", "Sent"},
    [T_SENT_FMT] = {"%lu émises", "%lu sent"},
    [T_ERRORS_BY_TYPE] = {"ERREURS PAR TYPE", "ERRORS BY TYPE"},
    [T_TX_DONE] = {"Réussies", "Succeeded"},
    [T_TX_TIMEOUT] = {"Délai dépassé", "Timed out"},
    [T_TX_ABORTED] = {"Interrompues", "Aborted"},

    [T_SETUP_REGION_HINT] = {"Fixe les profils de base et\nl'évaluation des émissions.",
                             "Sets the base profiles and\nhow transmissions are assessed."},
    [T_SETUP_EU868] = {"EU868 · Europe 863–870 MHz", "EU868 · Europe 863–870 MHz"},
    [T_SETUP_US915] = {"US915 · Amériques 902–928 MHz",
                       "US915 · Americas 902–928 MHz"},
    [T_SETUP_PREPARING] = {"Préparation…", "Preparing…"},
};

const char *tr(t_id_t id)
{
    static bool checked;
    if (!checked) {
        checked = true;
        link_i18n_check(TEXTS, T_COUNT, "link-lr");
    }

    return link_i18n_get(TEXTS, T_COUNT, id);
}
