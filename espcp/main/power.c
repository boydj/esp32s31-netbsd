/*
 * Power for the host: the CPU clock, which both harts share, and deep
 * sleep, which powers the whole chip down.
 *
 * The clock moves among the CPLL's 320, 160 and 80 MHz the way ESP-IDF's
 * own frequency scaling does (dividers and the root mux only, no PLL
 * power or calibration); not below 80, as PSRAM at 200 MHz wants the bus
 * at a quarter of that.  Timekeeping on both harts is the SYSTIMER's,
 * which runs off the XTAL; the ROM's busy-wait is told the new rate.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "soc/rtc.h"
#include "soc/soc_caps.h"

#include "espcp.h"

static const char *TAG = "power";

static const uint32_t cpu_mhz[] = { 320, 160, 80 };
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool cpufreq_ok(uint32_t mhz)
{
    for (size_t i = 0; i < sizeof(cpu_mhz) / sizeof(cpu_mhz[0]); i++) {
        if (cpu_mhz[i] == mhz) {
            return true;
        }
    }
    return false;
}

static int cpufreq_set(uint32_t mhz)
{
    rtc_cpu_freq_config_t cfg;

    if (!cpufreq_ok(mhz) || !rtc_clk_cpu_freq_mhz_to_config(mhz, &cfg)) {
        return ESPCP_EINVAL;
    }
    taskENTER_CRITICAL(&s_mux);
    rtc_clk_cpu_freq_set_config_fast(&cfg);
    esp_rom_set_cpu_ticks_per_us(mhz);
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "CPU clock %u MHz", (unsigned)mhz);
    return 0;
}

static void power_off(uint32_t secs, uint32_t gpios)
{
    ESP_LOGI(TAG, "deep sleep; waking after %u s, on LP GPIOs %#x",
             (unsigned)secs, (unsigned)gpios);
    if (secs != 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000);
    }
    gpios &= (1u << SOC_RTCIO_PIN_COUNT) - 1;
    if (gpios != 0) {
        esp_sleep_enable_ext1_wakeup_io(gpios, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    esp_deep_sleep_start();
}

void espcp_power_ctl(uint8_t type, uint32_t seq, const void *data, size_t len)
{
    struct espcp_power_req req;
    struct espcp_power ps;
    rtc_cpu_freq_config_t cur;
    int error = 0;

    memset(&req, 0, sizeof(req));
    memcpy(&req, data, len < sizeof(req) ? len : sizeof(req));
    switch (req.pr_op) {
    case ESPCP_POWER_CPUFREQ:
        if (req.pr_arg != 0) {
            error = cpufreq_set(req.pr_arg);
        }
        break;
    case ESPCP_POWER_OFF:
        power_off(req.pr_arg, req.pr_arg2);
        return;                         /* not reached */
    default:
        espcp_ctl_reply(type, seq, ESPCP_EOPNOTSUPP, NULL, 0);
        return;
    }
    memset(&ps, 0, sizeof(ps));
    rtc_clk_cpu_freq_get_config(&cur);
    ps.ps_mhz = cur.freq_mhz;
    memcpy(ps.ps_avail, cpu_mhz, sizeof(cpu_mhz));
    espcp_ctl_reply(type, seq, error, &ps, sizeof(ps));
}
