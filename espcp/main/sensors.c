/*
 * ESP32-S31 radio co-processor firmware: sensors for the host.
 *
 * The temperature sensor and the SAR ADCs need the analog I2C bus, which
 * the Wi-Fi PHY uses too, and calibration that ESP-IDF's drivers already
 * do; so the host asks us.  Requests are served by a task of their own,
 * so that a sensor that never answers (an emulator without one) doesn't
 * hold up the control task.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#include "espcp.h"

static const char *TAG = "sensors";

struct sensors_req {
    uint32_t seq;
    uint32_t mask;
    struct espcp_sensors r;     /* the temperature, already read */
};

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_done;
static bool s_stuck;            /* the ADC never came back */

static temperature_sensor_handle_t s_tsens;
static bool s_tsens_failed;
static adc_oneshot_unit_handle_t s_adc[2];
static adc_cali_handle_t s_cali[2];
static uint32_t s_adc_configured, s_adc_failed;

static bool tsens_read(int32_t *mc)
{
    float c;

    if (s_tsens == NULL && !s_tsens_failed) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            ESP_LOGE(TAG, "no temperature sensor");
            s_tsens = NULL;
            s_tsens_failed = true;
        }
    }
    if (s_tsens == NULL || temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return false;
    }
    *mc = (int32_t)(c * 1000.0f);
    return true;
}

static bool adc_setup(int u, int ch)
{
    const uint32_t bit = 1u << (u * 8 + ch);

    if (s_adc_failed & bit) {
        return false;
    }
    if (s_adc[u] == NULL) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 + u };

        if (adc_oneshot_new_unit(&ucfg, &s_adc[u]) != ESP_OK) {
            ESP_LOGE(TAG, "no ADC%d", u + 1);
            s_adc[u] = NULL;
            s_adc_failed |= 0xffu << (u * 8);
            return false;
        }
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_line_fitting_config_t ccfg = {
            .unit_id = ADC_UNIT_1 + u,
            .atten = ADC_ATTEN_DB_0,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&ccfg, &s_cali[u]) != ESP_OK) {
            s_cali[u] = NULL;
        }
#endif
    }
    if ((s_adc_configured & bit) == 0) {
        adc_oneshot_chan_cfg_t ccfg = {
            .atten = ADC_ATTEN_DB_0,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };

        if (adc_oneshot_config_channel(s_adc[u], ch, &ccfg) != ESP_OK) {
            s_adc_failed |= bit;
            return false;
        }
        s_adc_configured |= bit;
    }
    return true;
}

static struct sensors_req s_req;

/* ADC reads, which may take a while (or, on an emulator, forever). */
static void sensors_task(void *arg)
{
    uint32_t mask;

    for (;;) {
        xQueueReceive(s_queue, &mask, portMAX_DELAY);

        for (int i = 0; i < ESPCP_ADC_CHANNELS; i++) {
            const int u = i / 8, ch = i % 8;
            int raw, mv;

            if ((mask & (1u << i)) == 0 || !adc_setup(u, ch)) {
                continue;
            }
            if (adc_oneshot_read(s_adc[u], ch, &raw) != ESP_OK) {
                continue;
            }
            s_req.r.ss_adc_valid |= 1u << i;
            s_req.r.ss_adc_raw[i] = raw;
            if (s_cali[u] != NULL &&
                adc_cali_raw_to_voltage(s_cali[u], raw, &mv) == ESP_OK) {
                s_req.r.ss_adc_mv[i] = mv;
                s_req.r.ss_adc_cal |= 1u << i;
            }
        }
        xSemaphoreGive(s_done);
    }
}

void espcp_sensors_init(void)
{
    s_queue = xQueueCreate(1, sizeof(uint32_t));
    s_done = xSemaphoreCreateBinary();
    xTaskCreate(sensors_task, "sensors", 4096, NULL, 2, NULL);
}

/*
 * Run in the control task.  The ADC channels are read by the sensor task,
 * given half a second; if it doesn't come back (an emulator without the
 * ADC), the reply has the temperature only, now and from then on.
 */
void espcp_sensors_ctl(uint8_t type, uint32_t seq, const void *data, size_t len)
{
    const struct espcp_sensors_req *sr = data;
    uint32_t mask = 0;

    memset(&s_req, 0, sizeof(s_req));
    if (len >= sizeof(*sr)) {
        mask = sr->sr_adc_mask & ((1u << ESPCP_ADC_CHANNELS) - 1);
    }
    if (tsens_read(&s_req.r.ss_temp)) {
        s_req.r.ss_flags |= ESPCP_SENS_TEMP;
    }
    if (mask != 0 && !s_stuck && xQueueSend(s_queue, &mask, 0) == pdTRUE) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGE(TAG, "ADC not answering");
            s_stuck = true;
        }
    }
    if (s_stuck) {
        s_req.r.ss_adc_valid = s_req.r.ss_adc_cal = 0;
    }
    espcp_ctl_reply(type, seq, 0, &s_req.r, sizeof(s_req.r));
}
