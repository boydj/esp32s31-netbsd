/*
 * ESP32-S31 radio co-processor firmware: start-up on hart 1.
 *
 * The host OS on hart 0 (NetBSD) has already set up the clocks, the flash
 * and PSRAM, the caches and the MMU, and it keeps running while we do.  So
 * this replaces ESP-IDF's call_start_cpu0() with the per-hart part of it
 * only, then joins IDF's normal start-up (start_cpu0) as a single-core
 * application.  rv_utils_get_core_id() says 0 (CONFIG_ESPCP_FIRMWARE), so
 * everything that picks per-hart hardware by core number is redirected to
 * hart 1 here: the interrupt matrix and, in port_systick.c, the tick alarm.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"
#include "riscv/csr.h"
#include "riscv/rv_utils.h"
#include "riscv/interrupt.h"
#include "hal/interrupt_clic_ll.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "soc/interrupts.h"
#include "soc/rtc.h"
#include "heap_memory_layout.h"

#include "espcp_sys.h"

/* OpenSBI and what it needs, below CONFIG_ESPCP_SRAM_START. */
SOC_RESERVE_MEMORY_REGION(SOC_DIRAM_DRAM_LOW, CONFIG_ESPCP_SRAM_START, espcp_opensbi);

/* Filled in by the host; must be in a loaded segment, not bss. */
DRAM_ATTR struct espcp_bootparams espcp_bootparams = { .bp_reserved = { 1 } };

struct espcp_shm *espcp_shm;

/* Stack until FreeRTOS starts; entry.S knows the size. */
uint8_t espcp_boot_stack[4096] __attribute__((aligned(16)));

extern int _vector_table;
extern int _mtvt_table;
extern void start_cpu0(void) __attribute__((noreturn));
extern void intr_matrix_set(int cpu_no, uint32_t model_num, uint32_t intr_num);

static inline uint32_t hartid(void)
{
    return RV_READ_CSR(mhartid);
}

/*
 * Route peripheral interrupts to this hart, whatever core number IDF
 * thinks it is on.  Overrides the ROM alias in esp32s31.rom.api.ld.
 */
void esp_rom_route_intr_matrix(int cpu_core, uint32_t periph_intr_id, uint32_t cpu_intr_num)
{
    (void)cpu_core;
    intr_matrix_set(hartid(), periph_intr_id, cpu_intr_num);
}

/* ------------------------------------------------------------------------
 * Doorbells
 */

void espcp_doorbell_host(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
    *(volatile uint32_t *)ESPCP_FROM_CPU_REG(ESPCP_DOORBELL_HOST) = 1;
}

void espcp_doorbell_clear(void)
{
    *(volatile uint32_t *)ESPCP_FROM_CPU_REG(ESPCP_DOORBELL_FW) = 0;
    __asm__ volatile("fence rw, rw" ::: "memory");
}

/* ------------------------------------------------------------------------
 * Shared memory layout
 */

#define SHM_H2F_OFF     0x01000
#define SHM_H2F_SIZE    0x10000
#define SHM_F2H_OFF     (SHM_H2F_OFF + SHM_H2F_SIZE)
#define SHM_F2H_SIZE    0x20000
#define SHM_LOG_OFF     (SHM_F2H_OFF + SHM_F2H_SIZE)
#define SHM_LOG_SIZE    0x04000
#define SHM_NEEDED      (SHM_LOG_OFF + SHM_LOG_SIZE)

_Static_assert(sizeof(struct espcp_shm) <= SHM_H2F_OFF, "shm header too big");
_Static_assert(SHM_NEEDED <= CONFIG_ESPCP_SHM_SIZE, "rings don't fit");

static bool shm_init(void)
{
    const struct espcp_bootparams *bp = &espcp_bootparams;

    if (bp->bp_magic != ESPCP_BP_MAGIC || bp->bp_shm == 0 ||
            bp->bp_shmsize < SHM_NEEDED) {
        return false;
    }
    struct espcp_shm *shm = (struct espcp_shm *)bp->bp_shm;

    shm->es_version = ESPCP_PROTO_VERSION;
    shm->es_state = ESPCP_STATE_BOOT;
    shm->es_h2f.er_offset = SHM_H2F_OFF;
    shm->es_h2f.er_size = SHM_H2F_SIZE;
    shm->es_f2h.er_offset = SHM_F2H_OFF;
    shm->es_f2h.er_size = SHM_F2H_SIZE;
    shm->es_log.er_offset = SHM_LOG_OFF;
    shm->es_log.er_size = SHM_LOG_SIZE;
    __asm__ volatile("fence rw, rw" ::: "memory");
    shm->es_magic = ESPCP_SHM_MAGIC;
    espcp_shm = shm;
    return true;
}

/* ------------------------------------------------------------------------
 * Log: a byte ring the host drains.  When it is full, text is dropped.
 */

#define UART0_FIFO_REG      0x2038a000u
#define UART0_STATUS_REG    0x2038a01cu
#define UART_TXFIFO_CNT(s)  (((s) >> 16) & 0xff)

static uint32_t s_log_dropped;

static void IRAM_ATTR log_putc_locked(char c)
{
    struct espcp_shm *shm = espcp_shm;

    if (espcp_bootparams.bp_flags & ESPCP_BP_LOG_UART) {
        while (UART_TXFIFO_CNT(*(volatile uint32_t *)UART0_STATUS_REG) >= 120) {
            ;
        }
        *(volatile uint32_t *)UART0_FIFO_REG = (uint8_t)c;
    }
    if (shm == NULL) {
        return;
    }
    struct espcp_ring *r = &shm->es_log;
    const uint32_t head = r->er_head;
    if (head - r->er_tail >= r->er_size) {
        s_log_dropped++;
        return;
    }
    ((volatile char *)espcp_shm_ptr(r->er_offset))[head & (r->er_size - 1)] = c;
    __asm__ volatile("fence w, w" ::: "memory");
    r->er_head = head + 1;
}

void IRAM_ATTR espcp_log_write(const char *buf, size_t len)
{
    const unsigned mie = RV_CLEAR_CSR(mstatus, MSTATUS_MIE) & MSTATUS_MIE;
    while (len-- > 0) {
        log_putc_locked(*buf++);
    }
    RV_SET_CSR(mstatus, mie);
}

static void IRAM_ATTR log_putc(char c)
{
    espcp_log_write(&c, 1);
}

void IRAM_ATTR espcp_panic_putc(char c)
{
    if (c != '\r') {
        log_putc_locked(c);
    }
}

int espcp_log_vprintf(const char *fmt, va_list ap)
{
    char buf[192];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n < 0) {
        return n;
    }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    /* The host splits lines itself; drop the CRs. */
    char *p = buf, *e = buf + len, *q = buf;
    for (; p < e; p++) {
        if (*p != '\r') {
            *q++ = *p;
        }
    }
    espcp_log_write(buf, q - buf);
    return n;
}

/* ------------------------------------------------------------------------
 * Fatal errors: never reset the chip, the host is running on it.
 */

void IRAM_ATTR __attribute__((noreturn)) espcp_halt(const char *why)
{
    RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
    struct espcp_shm *shm = espcp_shm;
    if (shm != NULL) {
        size_t i;
        for (i = 0; why[i] != '\0' && i < sizeof(shm->es_panic) - 1; i++) {
            shm->es_panic[i] = why[i];
        }
        shm->es_panic[i] = '\0';
        __asm__ volatile("fence rw, rw" ::: "memory");
        shm->es_state = ESPCP_STATE_PANIC;
        espcp_doorbell_host();
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ------------------------------------------------------------------------
 * Start-up
 */

static int s_errno_tls;

void __attribute__((noreturn)) espcp_start(void)
{
    /* Per-hart CPU set-up, as call_start_cpu0()'s init_cpu(). */
#if SOC_BRANCH_PREDICTOR_SUPPORTED
    esp_cpu_branch_prediction_enable();
#endif
    esp_cpu_intr_set_ivt_addr(&_vector_table);
#if SOC_INT_CLIC_SUPPORTED
    esp_cpu_intr_set_xtvt_addr(&_mtvt_table);
#endif
#if SOC_CPU_SUPPORT_WFE
    esp_cpu_disable_wfe_mode();
#endif
#if CONFIG_LIBC_PICOLIBC
    esp_cpu_set_threadptr(&s_errno_tls);
#endif

    if (!shm_init()) {
        /* Nobody to tell. */
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
    esp_rom_install_channel_putc(1, log_putc);
    esp_rom_install_channel_putc(2, NULL);

    /* ROM delays count CPU cycles. */
    rtc_cpu_freq_config_t freq;
    rtc_clk_cpu_freq_get_config(&freq);
    if (freq.freq_mhz != 0) {
        esp_rom_set_cpu_ticks_per_us(freq.freq_mhz);
    }

    /* Nothing routed to this hart yet; CLIC lines vectored. */
    for (int i = 0; i < ETS_MAX_INTR_SOURCE; i++) {
        interrupt_clic_ll_route(hartid(), i, ETS_INVALID_INUM);
    }
    for (int i = 0; i < 32; i++) {
        esprv_int_set_vectored(i, true);
    }
    espcp_doorbell_clear();

    esp_rom_printf("espcp: hart %u, %u MHz, shm %p, %u KiB PSRAM at 0x%08x\n",
                   (unsigned)hartid(), (unsigned)freq.freq_mhz, espcp_shm,
                   (unsigned)(espcp_bootparams.bp_extmemsize / 1024),
                   (unsigned)espcp_bootparams.bp_extmem);

    start_cpu0();
}
