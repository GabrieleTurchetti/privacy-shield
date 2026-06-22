#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "global_config.h"
#include "battery.h"
#include "log_tags.h"

static const char *TAG = LOG_TAG_BATTERY;
static adc_oneshot_unit_handle_t adc_handle;

void battery_init(void) {
    ESP_LOGI(TAG, "Initializing Battery sensors...");

    // Configure digital pins STAT1 and STAT2 as Input with internal Pull-Up
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BATT_STAT1) | (1ULL << PIN_BATT_STAT2),
        .pull_down_en = 0,
        .pull_up_en = 1 // Crucial for MCP73833
    };
    gpio_config(&io_conf);

    // Configure ADC to read analog voltage
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATT_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12, // Allows readings up to ~3.3V
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATT_ADC_CHAN, &config));

    ESP_LOGI(TAG, "Battery System Ready!");
}

batt_status_t battery_get_status(void) {
    int stat1 = gpio_get_level(PIN_BATT_STAT1);
    int stat2 = gpio_get_level(PIN_BATT_STAT2);

    // MCP73833 Truth Table
    if (stat1 == 0 && stat2 == 1) return BATT_CHARGING;
    if (stat1 == 1 && stat2 == 0) return BATT_FULL;
    if (stat1 == 0 && stat2 == 0) return BATT_ERROR;
    
    // If both are High, the USB cable is unplugged
    return BATT_DISCHARGING; 
}

float battery_get_voltage(void) {
    int adc_raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, BATT_ADC_CHAN, &adc_raw));

    // Raw conversion of ESP32 ADC pin
    float pin_voltage = ((float)adc_raw / 4095.0) * 3.3;

    // Multiply by 2.0 because our hardware voltage divider halved the signal
    float real_battery_voltage = pin_voltage * 2.0;
    
    return real_battery_voltage;
}

int battery_get_percentage(void) {
    float voltage = battery_get_voltage();
    
    // A standard LiPo is fully charged at 4.2V, and effectively empty at ~3.2V
    const float MAX_V = 4.2;
    const float MIN_V = 3.2;

    if (voltage >= MAX_V) return 100;
    if (voltage <= MIN_V) return 0;

    // Linear mapping from voltage to percentage
    int percentage = (int)(((voltage - MIN_V) / (MAX_V - MIN_V)) * 100.0);
    return percentage;
}

void battery_logger_task(void *pvParameters) {
    while (1) {
        // Fetch current metrics from the battery component
        batt_status_t status = battery_get_status();
        float voltage = battery_get_voltage();
        int percentage = battery_get_percentage();

        // Convert the state enum into a human-readable string with emojis
        const char *status_str = "UNKNOWN";
        switch (status) {
            case BATT_CHARGING:    status_str = "CHARGING \xE2\x9A\xA1"; break;
            case BATT_FULL:        status_str = "FULL \xE2\x9C\x85"; break;
            case BATT_DISCHARGING: status_str = "DISCHARGING \xF0\x9F\x94\x8B"; break;
            case BATT_ERROR:       status_str = "ERROR \xE2\x9A\xA0"; break;
        }

        // Print the formatted log message to the console
        ESP_LOGI(TAG, "Status: %s | Voltage: %.2fV | Level: %d%%", 
                status_str, voltage, percentage);

        // Block the task for 10 seconds to free up CPU
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}