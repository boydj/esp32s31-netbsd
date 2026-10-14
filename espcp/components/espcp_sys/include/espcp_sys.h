/*
 * ESP32-S31 radio co-processor firmware: system glue for running ESP-IDF
 * on hart 1 under a host OS on hart 0.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include "esp_cpproto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* From the host. */
extern struct espcp_bootparams espcp_bootparams;

/* The shared memory, in PSRAM given to us by the host. */
extern struct espcp_shm *espcp_shm;

static inline void *espcp_shm_ptr(uint32_t offset)
{
    return (char *)espcp_shm + offset;
}

/* Ring the host's doorbell. */
void espcp_doorbell_host(void);

/* Acknowledge our own doorbell. */
void espcp_doorbell_clear(void);

/* Log text to the host (and UART0 if asked to). */
void espcp_log_write(const char *buf, size_t len);
int espcp_log_vprintf(const char *fmt, va_list ap);

/* Report a fatal error to the host and stop. */
void espcp_halt(const char *why) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
