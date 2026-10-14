/*
 * ESP32-S31 radio co-processor firmware: Wi-Fi station.
 *
 * Frames go between the host and the Wi-Fi driver as they are (Ethernet
 * II); the host does IP.  The firmware does association and WPA/WPA2/WPA3
 * (ESP-IDF's supplicant), and keeps reconnecting until told otherwise.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"

#include "espcp.h"

static const char *TAG = "wifi";

uint32_t espcp_stat_wifi_rx_drop, espcp_stat_wifi_tx_err;

static volatile uint8_t s_state = ESPCP_WIFI_OFF;
static bool s_want_connect;
static uint8_t s_reason;
static unsigned s_retries;
static wifi_config_t s_config;
static uint8_t s_bssid[6], s_channel, s_auth;
static TimerHandle_t s_retry_timer;

static int err_to_status(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_NO_MEM:
        return ESPCP_ENOMEM;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_WIFI_SSID:
    case ESP_ERR_WIFI_PASSWORD:
        return ESPCP_EINVAL;
    case ESP_ERR_WIFI_NOT_STARTED:
    case ESP_ERR_WIFI_NOT_INIT:
        return ESPCP_ENETDOWN;
    case ESP_ERR_WIFI_STATE:
        return ESPCP_EBUSY;
    case ESP_ERR_WIFI_TIMEOUT:
        return ESPCP_ETIMEDOUT;
    default:
        return ESPCP_EIO;
    }
}

static void get_status(struct espcp_wifi_status *ws)
{
    memset(ws, 0, sizeof(*ws));
    ws->ws_state = s_state;
    ws->ws_reason = s_reason;
    ws->ws_ssid_len = strnlen((const char *)s_config.sta.ssid, sizeof(ws->ws_ssid));
    memcpy(ws->ws_ssid, s_config.sta.ssid, ws->ws_ssid_len);
    if (s_state == ESPCP_WIFI_CONNECTED) {
        wifi_ap_record_t ap;
        memcpy(ws->ws_bssid, s_bssid, 6);
        ws->ws_channel = s_channel;
        ws->ws_auth = s_auth;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ws->ws_rssi = ap.rssi;
        }
    }
}

static void send_link_event(void)
{
    struct espcp_wifi_status ws;

    get_status(&ws);
    espcp_ctl_event(ESPCP_EVT_WIFI_LINK, &ws, sizeof(ws));
}

/* ------------------------------------------------------------------------
 * Data path
 */

static esp_err_t rx_cb(void *buffer, uint16_t len, void *eb)
{
    if (!espcp_send(ESPCP_CHAN_WIFI, 0, buffer, len, NULL, 0, 0)) {
        espcp_stat_wifi_rx_drop++;
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

void espcp_wifi_tx(const void *frame, size_t len)
{
    if (s_state != ESPCP_WIFI_CONNECTED) {
        return;
    }
    if (esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, len) != ESP_OK) {
        espcp_stat_wifi_tx_err++;
    }
}

/* ------------------------------------------------------------------------
 * Events
 */

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    if (s_want_connect && s_state != ESPCP_WIFI_CONNECTED) {
        esp_wifi_connect();
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx_cb);
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *ev = data;
        memcpy(s_bssid, ev->bssid, 6);
        s_channel = ev->channel;
        s_auth = ev->authmode;
        s_reason = 0;
        s_retries = 0;
        s_state = ESPCP_WIFI_CONNECTED;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx_cb);
        ESP_LOGI(TAG, "connected to %.32s, channel %u", ev->ssid, ev->channel);
        send_link_event();
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        const bool was = s_state == ESPCP_WIFI_CONNECTED;
        s_reason = ev->reason;
        if (s_state != ESPCP_WIFI_OFF) {
            s_state = s_want_connect ? ESPCP_WIFI_CONNECTING : ESPCP_WIFI_IDLE;
        }
        if (was || s_retries == 0) {
            ESP_LOGI(TAG, "disconnected, reason %u", ev->reason);
            send_link_event();
        }
        if (s_want_connect && s_state != ESPCP_WIFI_OFF) {
            /* Retry at once a few times, then every 5 seconds. */
            const unsigned ms = s_retries++ < 3 ? 100 : 5000;
            xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(ms), 0);
            xTimerStart(s_retry_timer, 0);
        }
        break;
    }
    case WIFI_EVENT_STA_STOP:
        s_state = ESPCP_WIFI_OFF;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * Control
 */

static esp_err_t radio_on(void)
{
    if (s_state != ESPCP_WIFI_OFF) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        /* No modem sleep: latency matters more here than power. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_state = ESPCP_WIFI_IDLE;
    }
    return err;
}

static void do_scan(uint32_t seq)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = radio_on();

    if (err == ESP_OK) {
        err = esp_wifi_scan_start(&sc, true);
    }
    if (err != ESP_OK) {
        espcp_ctl_reply(ESPCP_CTL_WIFI_SCAN, seq, err_to_status(err), NULL, 0);
        return;
    }

    uint16_t n = ESPCP_SCAN_MAX;
    wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
    const size_t replylen = sizeof(struct espcp_wifi_scan) +
                            ESPCP_SCAN_MAX * sizeof(struct espcp_wifi_ap);
    struct espcp_wifi_scan *ws = calloc(1, replylen);
    if (rec == NULL || ws == NULL) {
        esp_wifi_clear_ap_list();
        espcp_ctl_reply(ESPCP_CTL_WIFI_SCAN, seq, ESPCP_ENOMEM, NULL, 0);
        goto out;
    }
    err = esp_wifi_scan_get_ap_records(&n, rec);
    if (err != ESP_OK) {
        espcp_ctl_reply(ESPCP_CTL_WIFI_SCAN, seq, err_to_status(err), NULL, 0);
        goto out;
    }
    for (unsigned i = 0; i < n; i++) {
        struct espcp_wifi_ap *ap = &ws->ws_ap[i];
        memcpy(ap->ap_bssid, rec[i].bssid, 6);
        ap->ap_ssid_len = strnlen((const char *)rec[i].ssid, sizeof(ap->ap_ssid));
        memcpy(ap->ap_ssid, rec[i].ssid, ap->ap_ssid_len);
        ap->ap_channel = rec[i].primary;
        ap->ap_rssi = rec[i].rssi;
        ap->ap_auth = rec[i].authmode;
    }
    ws->ws_count = n;
    espcp_ctl_reply(ESPCP_CTL_WIFI_SCAN, seq, 0, ws,
                    sizeof(*ws) + n * sizeof(struct espcp_wifi_ap));
out:
    free(rec);
    free(ws);
}

static void do_connect(uint32_t seq, const void *data, size_t len)
{
    struct espcp_wifi_config wc;
    esp_err_t err;

    if (len < sizeof(wc)) {
        espcp_ctl_reply(ESPCP_CTL_WIFI_CONNECT, seq, ESPCP_EINVAL, NULL, 0);
        return;
    }
    memcpy(&wc, data, sizeof(wc));
    wc.wc_ssid[sizeof(wc.wc_ssid) - 1] = '\0';
    wc.wc_password[sizeof(wc.wc_password) - 1] = '\0';

    err = radio_on();
    if (err != ESP_OK) {
        goto out;
    }
    s_want_connect = false;
    xTimerStop(s_retry_timer, 0);
    if (s_state == ESPCP_WIFI_CONNECTED || s_state == ESPCP_WIFI_CONNECTING) {
        esp_wifi_disconnect();
    }

    memset(&s_config, 0, sizeof(s_config));
    strlcpy((char *)s_config.sta.ssid, wc.wc_ssid, sizeof(s_config.sta.ssid));
    strlcpy((char *)s_config.sta.password, wc.wc_password, sizeof(s_config.sta.password));
    if (wc.wc_flags & ESPCP_WC_BSSID) {
        s_config.sta.bssid_set = true;
        memcpy(s_config.sta.bssid, wc.wc_bssid, 6);
    }
    s_config.sta.threshold.authmode = wc.wc_minauth;
    s_config.sta.pmf_cfg.capable = true;
    s_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    s_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    s_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    err = esp_wifi_set_config(WIFI_IF_STA, &s_config);
    if (err != ESP_OK) {
        goto out;
    }
    s_want_connect = true;
    s_retries = 0;
    s_reason = 0;
    s_state = ESPCP_WIFI_CONNECTING;
    err = esp_wifi_connect();
out:
    espcp_ctl_reply(ESPCP_CTL_WIFI_CONNECT, seq, err_to_status(err), NULL, 0);
}

void espcp_wifi_ctl(uint8_t type, uint32_t seq, const void *data, size_t len)
{
    struct espcp_wifi_status ws;
    esp_err_t err = ESP_OK;

    switch (type) {
    case ESPCP_CTL_WIFI_START:
        err = radio_on();
        espcp_ctl_reply(type, seq, err_to_status(err), NULL, 0);
        break;
    case ESPCP_CTL_WIFI_STOP:
        s_want_connect = false;
        xTimerStop(s_retry_timer, 0);
        if (s_state != ESPCP_WIFI_OFF) {
            err = esp_wifi_stop();
            s_state = ESPCP_WIFI_OFF;
        }
        espcp_ctl_reply(type, seq, err_to_status(err), NULL, 0);
        break;
    case ESPCP_CTL_WIFI_SCAN:
        do_scan(seq);
        break;
    case ESPCP_CTL_WIFI_CONNECT:
        do_connect(seq, data, len);
        break;
    case ESPCP_CTL_WIFI_DISCONNECT:
        s_want_connect = false;
        xTimerStop(s_retry_timer, 0);
        if (s_state == ESPCP_WIFI_CONNECTED || s_state == ESPCP_WIFI_CONNECTING) {
            err = esp_wifi_disconnect();
            s_state = ESPCP_WIFI_IDLE;
        }
        espcp_ctl_reply(type, seq, err_to_status(err), NULL, 0);
        break;
    case ESPCP_CTL_WIFI_STATUS:
        get_status(&ws);
        espcp_ctl_reply(type, seq, 0, &ws, sizeof(ws));
        break;
    }
}

void espcp_wifi_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event, NULL));
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(100), pdFALSE,
                                 NULL, retry_cb);
    ESP_ERROR_CHECK(esp_read_mac(espcp_shm->es_wifi_mac, ESP_MAC_WIFI_STA));
}
