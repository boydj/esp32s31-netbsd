/*
 * ESP32-S31 radio co-processor firmware: Bluetooth controller (BR/EDR and
 * LE) behind the VHCI, with the host's Bluetooth stack on the other side.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "espcp.h"

static const char *TAG = "bt";

static bool s_inited, s_enabled;
static SemaphoreHandle_t s_send_ok;
static uint32_t s_dropped;

static void host_send_available(void)
{
    xSemaphoreGive(s_send_ok);
}

/* A packet from the controller (H4 type byte first). */
static int host_recv(uint8_t *data, uint16_t len)
{
    ESP_LOGD(TAG, "rx type %u len %u", len ? data[0] : 0, len);
    if (len == 0 || !espcp_send(ESPCP_CHAN_HCI, data[0], data, len, NULL, 0,
                                portMAX_DELAY)) {
        s_dropped++;
    }
    return 0;
}

static const esp_vhci_host_callback_t s_vhci_cb = {
    .notify_host_send_available = host_send_available,
    .notify_host_recv = host_recv,
};

void espcp_bt_tx(const void *h4, size_t len)
{
    if (!s_enabled || len < 2) {
        return;
    }
    while (!esp_vhci_host_check_send_available()) {
        if (xSemaphoreTake(s_send_ok, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "controller busy, packet dropped");
            s_dropped++;
            return;
        }
    }
    ESP_LOGD(TAG, "tx type %u len %u", ((const uint8_t *)h4)[0], (unsigned)len);
    esp_vhci_host_send_packet((uint8_t *)h4, len);
}

static esp_err_t bt_enable(void)
{
    esp_err_t err;

    if (!s_inited) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
            return err;
        }
        s_inited = true;
    }
    if (!s_enabled) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
            return err;
        }
        esp_vhci_host_register_callback(&s_vhci_cb);
        s_enabled = true;
        ESP_LOGI(TAG, "controller enabled");
    }
    return ESP_OK;
}

/*
 * Controller start and stop run in a task of their own: enabling the
 * controller brings up the radio and can take a while, and the control
 * task has Wi-Fi requests to serve meanwhile.
 */
struct bt_req {
    uint8_t type;
    uint32_t seq;
};

static QueueHandle_t s_bt_q;

static void bt_ctl_run(uint8_t type, uint32_t seq)
{
    esp_err_t err = ESP_OK;

    switch (type) {
    case ESPCP_CTL_BT_ENABLE:
        err = bt_enable();
        break;
    case ESPCP_CTL_BT_DISABLE:
        if (s_enabled) {
            s_enabled = false;
            err = esp_bt_controller_disable();
        }
        break;
    }
    espcp_ctl_reply(type, seq, err == ESP_OK ? 0 :
                    err == ESP_ERR_NO_MEM ? ESPCP_ENOMEM : ESPCP_EIO, NULL, 0);
}

static void bt_task(void *arg)
{
    struct bt_req r;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_bt_q, &r, portMAX_DELAY) == pdTRUE) {
            bt_ctl_run(r.type, r.seq);
        }
    }
}

void espcp_bt_ctl(uint8_t type, uint32_t seq, const void *data, size_t len)
{
    const struct bt_req r = { .type = type, .seq = seq };

    (void)data;
    (void)len;
    if (xQueueSend(s_bt_q, &r, 0) != pdTRUE) {
        espcp_ctl_reply(type, seq, ESPCP_EBUSY, NULL, 0);
    }
}

void espcp_bt_init(void)
{
    s_send_ok = xSemaphoreCreateBinary();
    s_bt_q = xQueueCreate(4, sizeof(struct bt_req));
    xTaskCreate(bt_task, "espcp_bt", 3072, NULL, 5, NULL);
    ESP_ERROR_CHECK(esp_read_mac(espcp_shm->es_bt_addr, ESP_MAC_BT));
}
