/*
 * ESP32-S31 radio co-processor firmware: the message rings shared with
 * the host.
 *
 * Firmware to host: any task (or an ISR, without waiting) may send; a
 * spinlock serialises producers.  Host to firmware: the doorbell ISR wakes
 * the rx task, which passes Ethernet frames and HCI packets straight on and
 * queues control messages for the control task, since those can take
 * seconds (a scan).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "soc/interrupts.h"

#include "espcp.h"

static const char *TAG = "ring";

static portMUX_TYPE s_tx_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_space;
static TaskHandle_t s_rx_task;
static QueueHandle_t s_ctl_q;

struct ctl_req {
    uint8_t type;
    uint32_t seq;
    size_t len;
    uint8_t data[];
};

static inline void fence(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

/* Put one message in the ring, or return false if there is no room. */
static bool ring_put(struct espcp_ring *r, uint8_t chan, uint8_t type,
                     const void *a, size_t alen, const void *b, size_t blen)
{
    const size_t len = alen + blen;
    const uint32_t need = ESPCP_MSG_ALIGN(sizeof(struct espcp_msg) + len);
    const uint32_t size = r->er_size;
    uint32_t head = r->er_head;
    uint32_t off = head & (size - 1);
    const uint32_t contig = size - off;
    const uint32_t total = contig < need ? contig + need : need;
    char *base = espcp_shm_ptr(r->er_offset);

    if (size - (head - r->er_tail) < total) {
        return false;
    }
    if (contig < need) {
        struct espcp_msg *pad = (struct espcp_msg *)(base + off);
        pad->em_len = 0;
        pad->em_chan = ESPCP_CHAN_PAD;
        pad->em_type = 0;
        head += contig;
        off = 0;
    }
    struct espcp_msg *m = (struct espcp_msg *)(base + off);
    m->em_len = len;
    m->em_chan = chan;
    m->em_type = type;
    if (alen) {
        memcpy(m + 1, a, alen);
    }
    if (blen) {
        memcpy((char *)(m + 1) + alen, b, blen);
    }
    fence();
    r->er_head = head + need;
    return true;
}

bool espcp_send(uint8_t chan, uint8_t type, const void *a, size_t alen,
                const void *b, size_t blen, TickType_t wait)
{
    struct espcp_ring *r = &espcp_shm->es_f2h;
    const bool isr = xPortInIsrContext();

    if (alen + blen > ESPCP_MSG_MAX) {
        return false;
    }
    for (;;) {
        portENTER_CRITICAL_SAFE(&s_tx_lock);
        bool ok = ring_put(r, chan, type, a, alen, b, blen);
        if (!ok && wait != 0 && !isr) {
            r->er_wait = 1;
        }
        portEXIT_CRITICAL_SAFE(&s_tx_lock);
        if (ok) {
            espcp_doorbell_host();
            return true;
        }
        if (wait == 0 || isr) {
            return false;
        }
        espcp_doorbell_host();
        if (xSemaphoreTake(s_space, wait) != pdTRUE) {
            return false;
        }
    }
}

void espcp_ctl_reply(uint8_t type, uint32_t seq, int status,
                     const void *data, size_t len)
{
    struct espcp_ctl c = { .ec_seq = seq, .ec_status = status };

    if (!espcp_send(ESPCP_CHAN_CTL, type, &c, sizeof(c), data, len, portMAX_DELAY)) {
        ESP_LOGE(TAG, "lost reply %u", type);
    }
}

void espcp_ctl_event(uint8_t type, const void *data, size_t len)
{
    espcp_ctl_reply(type, 0, 0, data, len);
}

/* ------------------------------------------------------------------------ */

static void IRAM_ATTR doorbell_isr(void *arg)
{
    BaseType_t woken = pdFALSE;

    (void)arg;
    espcp_doorbell_clear();
    if (s_rx_task != NULL) {
        vTaskNotifyGiveFromISR(s_rx_task, &woken);
    }
    xSemaphoreGiveFromISR(s_space, &woken);
    portYIELD_FROM_ISR(woken);
}

static void queue_ctl(uint8_t type, const void *payload, size_t len)
{
    const struct espcp_ctl *c = payload;

    if (len < sizeof(*c)) {
        ESP_LOGW(TAG, "short control message %u", type);
        return;
    }
    struct ctl_req *q = malloc(sizeof(*q) + len - sizeof(*c));
    if (q == NULL) {
        espcp_ctl_reply(type, c->ec_seq, ESPCP_ENOMEM, NULL, 0);
        return;
    }
    q->type = type;
    q->seq = c->ec_seq;
    q->len = len - sizeof(*c);
    memcpy(q->data, c + 1, q->len);
    if (xQueueSend(s_ctl_q, &q, 0) != pdTRUE) {
        espcp_ctl_reply(type, q->seq, ESPCP_EBUSY, NULL, 0);
        free(q);
    }
}

static void rx_drain(void)
{
    struct espcp_ring *r = &espcp_shm->es_h2f;
    const uint32_t size = r->er_size;
    char *base = espcp_shm_ptr(r->er_offset);
    uint32_t tail = r->er_tail;

    while (tail != r->er_head) {
        fence();
        const uint32_t off = tail & (size - 1);
        const struct espcp_msg *m = (const struct espcp_msg *)(base + off);

        if (m->em_chan == ESPCP_CHAN_PAD) {
            tail += size - off;
            continue;
        }
        const uint32_t step = ESPCP_MSG_ALIGN(sizeof(*m) + m->em_len);
        if (m->em_len > ESPCP_MSG_MAX || step > size - off) {
            ESP_LOGE(TAG, "bad message at %u (len %u chan %u), resyncing",
                     (unsigned)tail, m->em_len, m->em_chan);
            tail = r->er_head;
            break;
        }
        const void *payload = m + 1;
        switch (m->em_chan) {
        case ESPCP_CHAN_WIFI:
            espcp_wifi_tx(payload, m->em_len);
            break;
        case ESPCP_CHAN_HCI:
            espcp_bt_tx(payload, m->em_len);
            break;
        case ESPCP_CHAN_CTL:
            queue_ctl(m->em_type, payload, m->em_len);
            break;
        default:
            ESP_LOGW(TAG, "message on unknown channel %u", m->em_chan);
            break;
        }
        tail += step;
        fence();
        r->er_tail = tail;
    }
    fence();
    r->er_tail = tail;
    if (r->er_wait) {
        r->er_wait = 0;
        espcp_doorbell_host();
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* The timeout covers a doorbell lost to a race. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        rx_drain();
    }
}

static void ctl_task(void *arg)
{
    struct ctl_req *q;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_ctl_q, &q, portMAX_DELAY) == pdTRUE) {
            espcp_ctl_dispatch(q->type, q->seq, q->data, q->len);
            free(q);
        }
    }
}

void espcp_ring_init(void)
{
    s_space = xSemaphoreCreateBinary();
    s_ctl_q = xQueueCreate(8, sizeof(struct ctl_req *));
    xTaskCreate(ctl_task, "espcp_ctl", 4096, NULL, 5, NULL);
    xTaskCreate(rx_task, "espcp_rx", 4096, NULL, 19, &s_rx_task);
    ESP_ERROR_CHECK(esp_intr_alloc(ESPCP_FROM_CPU_SOURCE(ESPCP_DOORBELL_FW),
                                   ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
                                   doorbell_isr, NULL, NULL));
}
