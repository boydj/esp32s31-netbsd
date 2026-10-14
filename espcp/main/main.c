/*
 * ESP32-S31 radio co-processor firmware: ESP-IDF's Wi-Fi and Bluetooth
 * controller on hart 1, for a host OS (NetBSD) on hart 0.
 *
 * The host sees a station-mode Ethernet interface and an HCI transport;
 * see esp_cpproto.h for the protocol.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"

#include "espcp.h"

static const char *TAG = "espcp";

void espcp_ctl_dispatch(uint8_t type, uint32_t seq, const void *data, size_t len)
{
    switch (type) {
    case ESPCP_CTL_PING:
        espcp_ctl_reply(type, seq, 0, data, len);
        break;
    case ESPCP_CTL_WIFI_START:
    case ESPCP_CTL_WIFI_STOP:
    case ESPCP_CTL_WIFI_SCAN:
    case ESPCP_CTL_WIFI_CONNECT:
    case ESPCP_CTL_WIFI_DISCONNECT:
    case ESPCP_CTL_WIFI_STATUS:
        espcp_wifi_ctl(type, seq, data, len);
        break;
    case ESPCP_CTL_BT_ENABLE:
    case ESPCP_CTL_BT_DISABLE:
        espcp_bt_ctl(type, seq, data, len);
        break;
    default:
        espcp_ctl_reply(type, seq, ESPCP_EOPNOTSUPP, NULL, 0);
        break;
    }
}

/* The PSRAM the host gave us, after the shared memory, is heap. */
static void extmem_heap_init(void)
{
    const struct espcp_bootparams *bp = &espcp_bootparams;
    const intptr_t start = bp->bp_extmem + bp->bp_shmsize;
    const intptr_t end = bp->bp_extmem + bp->bp_extmemsize;
    const uint32_t caps[3] = {
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT,
        0,
        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT,
    };

    if (end - start < 16 * 1024) {
        return;
    }
    esp_err_t err = heap_caps_add_region_with_caps(caps, start, end);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't add PSRAM heap: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    struct espcp_shm *shm = espcp_shm;

    esp_log_set_vprintf(espcp_log_vprintf);
    extmem_heap_init();
    snprintf(shm->es_ident, sizeof(shm->es_ident), "espcp %s, ESP-IDF %s",
             esp_app_get_description()->version, esp_get_idf_version());
    ESP_LOGI(TAG, "%s", shm->es_ident);

    espcp_ring_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    espcp_wifi_init();
    espcp_bt_init();

    shm->es_caps = ESPCP_CAP_WIFI | ESPCP_CAP_BT;
    shm->es_heap_free = esp_get_free_heap_size();
    __asm__ volatile("fence rw, rw" ::: "memory");
    shm->es_state = ESPCP_STATE_RUNNING;
    espcp_doorbell_host();
    ESP_LOGI(TAG, "running; %u bytes free (%u internal)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        shm->es_heartbeat++;
        shm->es_heap_free = esp_get_free_heap_size();
        shm->es_heap_min = esp_get_minimum_free_heap_size();
    }
}
