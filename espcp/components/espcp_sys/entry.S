/*
 * Entry point of the radio co-processor firmware.  OpenSBI starts hart 1
 * here, in M-mode, after the host has copied our RAM segments (bss cleared)
 * and filled in espcp_bootparams.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "sdkconfig.h"

    .section .iram1.espcp_entry, "ax"
    .global espcp_entry
    .type   espcp_entry, @function
    .balign 4
espcp_entry:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    csrci   mstatus, 0x8            /* MIE */
    la      sp, espcp_boot_stack + 4096
    call    espcp_start
1:  wfi
    j       1b
    .size   espcp_entry, . - espcp_entry
