#include "link_stats.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static link_stats_t s_st;

void link_stats_begin(uint32_t session)
{
    const int64_t since_ms = esp_timer_get_time() / 1000;

    taskENTER_CRITICAL(&s_lock);
    memset(&s_st, 0, sizeof(s_st));
    s_st.ready = true;
    s_st.session = session;
    s_st.since_ms = since_ms;
    taskEXIT_CRITICAL(&s_lock);
}

void link_stats_note_rx(const link_radio_entry_t *e)
{
    const link_radio_frame_t *f = &e->frame;
    const int32_t m = link_radio_margin_tenths(f);

    taskENTER_CRITICAL(&s_lock);
    switch (e->kind) {
    case LINK_RADIO_ENTRY_FRAME:
        s_st.ok++;
        if (m >= LINK_RADIO_MARGIN_GOOD) {
            s_st.good++;
        } else if (m >= LINK_RADIO_MARGIN_FAIR) {
            s_st.fair++;
        } else {
            s_st.poor++;
        }
        break;
    case LINK_RADIO_ENTRY_CRC:
        s_st.crc++;
        break;
    case LINK_RADIO_ENTRY_HEADER_ERR:
        s_st.hdr++;
        break;
    case LINK_RADIO_ENTRY_LOST:
        s_st.incomplete++;
        break;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void link_stats_note_tx(const link_radio_tx_t *tx)
{
    taskENTER_CRITICAL(&s_lock);
    switch (tx->result) {
    case LINK_RADIO_TX_OK:
        s_st.tx_ok++;
        break;
    case LINK_RADIO_TX_TIMEOUT:
        s_st.tx_timeout++;
        break;
    case LINK_RADIO_TX_ABORTED:
        s_st.tx_aborted++;
        break;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void link_stats_get(link_stats_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
}
