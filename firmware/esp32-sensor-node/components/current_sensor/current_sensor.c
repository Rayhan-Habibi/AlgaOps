/* current_sensor.c — CT current on ESP32 ADC1 (esp_adc oneshot); see current_sensor.h */

#include "current_sensor.h"

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "sample_window.h"

#if CONFIG_ALGAOPS_CURRENT_ENABLE && CONFIG_FREERTOS_HZ < 1000
#error "current sampling needs CONFIG_FREERTOS_HZ=1000 (see sdkconfig.defaults)"
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#define CT_ATTEN ADC_ATTEN_DB_12      /* ~0.15-3.1 V input range */
#else
#define CT_ATTEN ADC_ATTEN_DB_11
#endif

#define ADC_RAW_MAX      ((1 << SOC_ADC_RTC_MAX_BITWIDTH) - 1)
#define CLIP_MARGIN      8             /* counts from either end of the range */
#define MIN_SAMPLES      50            /* a few mains cycles                   */

static const char *TAG = "current";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_chan;
static current_sensor_config_t   s_cfg;
static ac_rms_acc_t              s_acc;
static portMUX_TYPE              s_lock = portMUX_INITIALIZER_UNLOCKED;

static void sample_task(void *arg)
{
    (void)arg;
    TickType_t period = pdMS_TO_TICKS(1000 / s_cfg.sample_hz);
    if (period == 0) period = 1;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, period);
        int raw = 0;
        int mv = 0;
        esp_err_t err = adc_oneshot_read(s_adc, s_chan, &raw);
        if (err == ESP_OK) {
            if (s_cali) err = adc_cali_raw_to_voltage(s_cali, raw, &mv);
            else        mv = raw * 3100 / ADC_RAW_MAX;   /* uncalibrated estimate */
        }
        bool clipped = raw <= CLIP_MARGIN || raw >= ADC_RAW_MAX - CLIP_MARGIN;
        taskENTER_CRITICAL(&s_lock);
        if (err == ESP_OK) ac_rms_add(&s_acc, (double)mv, clipped);
        else               ac_rms_error(&s_acc);
        taskEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t current_sensor_start(const current_sensor_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->sample_hz >= 500 && cfg->sample_hz <= 1000 &&
                        cfg->ma_per_volt > 0, ESP_ERR_INVALID_ARG, TAG, "invalid configuration");
    s_cfg = *cfg;
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(cfg->gpio, &unit, &s_chan), TAG,
                        "GPIO %d is not an ADC pin", cfg->gpio);
    ESP_RETURN_ON_FALSE(unit == ADC_UNIT_1, ESP_ERR_INVALID_ARG, TAG,
                        "GPIO %d is on ADC2, which Wi-Fi blocks", cfg->gpio);
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC unit");
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = CT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "ADC channel");

    s_cali = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .chan = s_chan,
                                            .atten = CT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .atten = CT_ATTEN,
                                           .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    if (!s_cali) ESP_LOGW(TAG, "no ADC calibration data: using an uncalibrated estimate");

    ac_rms_reset(&s_acc);
    if (xTaskCreatePinnedToCore(sample_task, "ct_sample", 3072, NULL, 9, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CT on GPIO %d: %lu Hz, %lu mA/V", cfg->gpio, (unsigned long)cfg->sample_hz,
             (unsigned long)cfg->ma_per_volt);
    return ESP_OK;
}

void current_sensor_take_window(current_window_t *out)
{
    if (!out) return;
    ac_rms_acc_t acc;
    taskENTER_CRITICAL(&s_lock);
    acc = s_acc;
    ac_rms_reset(&s_acc);
    taskEXIT_CRITICAL(&s_lock);

    memset(out, 0, sizeof(*out));
    out->samples = acc.n;
    out->clipped = acc.clipped;
    out->errors = acc.errors;
    double rms_mv = 0.0;
    /* Clipped samples underestimate the RMS: such a window has no current value. */
    if (s_adc && acc.errors == 0 && acc.clipped == 0 && acc.n >= MIN_SAMPLES &&
        ac_rms_result(&acc, &rms_mv)) {
        out->valid = true;
        out->amps_rms = (float)(rms_mv / 1000.0 * (double)s_cfg.ma_per_volt / 1000.0);
    }
}
