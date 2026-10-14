/*
 * ESP32-S31 radio co-processor firmware.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "espcp_sys.h"

/* ring.c */
void espcp_ring_init(void);
/* Queue a message for the host; false if it doesn't fit within `wait'. */
bool espcp_send(uint8_t chan, uint8_t type, const void *a, size_t alen,
                const void *b, size_t blen, TickType_t wait);
void espcp_ctl_reply(uint8_t type, uint32_t seq, int status,
                     const void *data, size_t len);
void espcp_ctl_event(uint8_t type, const void *data, size_t len);

/* main.c: a control message, run in the control task. */
void espcp_ctl_dispatch(uint8_t type, uint32_t seq, const void *data, size_t len);

/* wifi.c */
void espcp_wifi_init(void);
void espcp_wifi_ctl(uint8_t type, uint32_t seq, const void *data, size_t len);
void espcp_wifi_tx(const void *frame, size_t len);

/* bt.c */
void espcp_bt_init(void);
void espcp_bt_ctl(uint8_t type, uint32_t seq, const void *data, size_t len);
void espcp_bt_tx(const void *h4, size_t len);

/* sensors.c */
void espcp_sensors_init(void);
void espcp_sensors_ctl(uint8_t type, uint32_t seq, const void *data, size_t len);

/* power.c */
void espcp_power_ctl(uint8_t type, uint32_t seq, const void *data, size_t len);

extern uint32_t espcp_stat_wifi_rx_drop, espcp_stat_wifi_tx_err;
