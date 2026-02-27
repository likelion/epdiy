#include "epd_board.h"
#include "esp_log.h"
#include "esp_system.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
/* IDF v5: driver and esp_adc components may be excluded by ESPHome.
 * Use the HAL low-level API which talks directly to registers. */
#include "hal/adc_ll.h"
#include "hal/adc_types.h"

#define ADC_CHANNEL  ADC_CHANNEL_7
#define NUMBER_OF_SAMPLES 100

void epd_board_temperature_init_v2() {
  /* Configure ADC1 for RTC (oneshot) mode */
  adc_ll_set_controller(ADC_UNIT_1, ADC_LL_CTRL_RTC);
  adc_oneshot_ll_set_output_bits(ADC_UNIT_1, ADC_BITWIDTH_12);
  adc_oneshot_ll_set_atten(ADC_UNIT_1, ADC_CHANNEL, ADC_ATTEN_DB_6);
  adc_ll_set_sar_clk_div(ADC_UNIT_1, 1);
  adc_ll_amp_disable();
}

static uint32_t adc1_read_raw(void) {
  adc_oneshot_ll_set_channel(ADC_UNIT_1, ADC_CHANNEL);
  adc_oneshot_ll_start(ADC_UNIT_1);
  while (!adc_oneshot_ll_get_event(ADC_LL_EVENT_ADC1_ONESHOT_DONE)) {}
  adc_oneshot_ll_clear_event(ADC_LL_EVENT_ADC1_ONESHOT_DONE);
  return adc_oneshot_ll_get_raw_result(ADC_UNIT_1);
}

float epd_board_ambient_temperature_v2() {
  uint32_t value = 0;
  for (int i = 0; i < NUMBER_OF_SAMPLES; i++) {
    value += adc1_read_raw();
  }
  value /= NUMBER_OF_SAMPLES;
  /* Approximate mV without calibration.
   * ADC_ATTEN_DB_6 ≈ 0–2200 mV range, 12-bit (0–4095).
   * voltage ≈ raw * 2200 / 4095  */
  float voltage = (float)value * 2200.0f / 4095.0f;
  return (voltage - 500.0f) / 10.0f;
}

#else
/* IDF v4: use the legacy driver API */
#include "driver/adc.h"
#include "esp_adc_cal.h"

static const adc1_channel_t channel = ADC1_CHANNEL_7;
static esp_adc_cal_characteristics_t adc_chars;

#define NUMBER_OF_SAMPLES 100

void epd_board_temperature_init_v2() {
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
    ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_12, 1100, &adc_chars
  );
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
    ESP_LOGI("epd_temperature", "Characterized using Two Point Value\n");
  } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    ESP_LOGI("esp_temperature", "Characterized using eFuse Vref\n");
  } else {
    ESP_LOGI("esp_temperature", "Characterized using Default Vref\n");
  }
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(channel, ADC_ATTEN_DB_6);
}

float epd_board_ambient_temperature_v2() {
  uint32_t value = 0;
  for (int i = 0; i < NUMBER_OF_SAMPLES; i++) {
    value += adc1_get_raw(channel);
  }
  value /= NUMBER_OF_SAMPLES;
  // voltage in mV
  float voltage = esp_adc_cal_raw_to_voltage(value, &adc_chars);
  return (voltage - 500.0) / 10.0;
}
#endif
