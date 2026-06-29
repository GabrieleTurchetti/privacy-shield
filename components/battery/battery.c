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
    float voltage = battery_get_voltage();

    if (voltage < 2.5f) return BATT_DISCONNECTED;

    // MCP73833 Truth Table
        if (stat1 == 0 && stat2 == 1) return BATT_CHARGING;
    if (stat1 == 1 && stat2 == 0) return BATT_FULL;
    if (stat1 == 0 && stat2 == 0) return BATT_CHARGING; // This state is not expected, but we treat it as charging
    
    // If both are High, the USB cable is unplugged
    return BATT_DISCHARGING; 
}

#define WINDOW_SIZE 60
static float percentage_window[WINDOW_SIZE];
static int window_index = 0;

float battery_get_voltage(void) {
    int adc_raw = 0;

    adc_oneshot_read(adc_handle, BATT_ADC_CHAN, &adc_raw);

    float pin_voltage = (adc_raw / 4095.0f) * 3.3f;

    /*
     * The battery voltage (up to 4.2V) exceeds the ESP32 ADC's 3.3V limit,
     * so we halved it with a voltage divider: two equal resistors (R1 = R2).
     * The ADC reads the midpoint -> half the real value -> multiply by 2 to recover.
     *
     * Formula: real = pin x (R1 + R2) / R2
     * With R1 = R2: multiplier = (R1 + R2) / R2 = 2.0
     *
     * If resistors differ, change the multiplier accordingly.
     */
    float real_battery_voltage = pin_voltage * 2.0f;

    return real_battery_voltage;
}

int battery_get_percentage(void) {
    float voltage = battery_get_voltage();
    window_index = (window_index + 1) % WINDOW_SIZE;
    int avg_percentage = 0;
    
    // A standard LiPo is fully charged at 4.2V, and effectively empty at ~3.2V
    const float MAX_V = 4.2;
    const float MIN_V = 3.2;

    // Linear mapping from voltage to percentage
    int percentage = (int)(((voltage - MIN_V) / (MAX_V - MIN_V)) * 100.0);
    if (voltage >= MAX_V) percentage = 100;
    if (voltage <= MIN_V) percentage = 0;
    percentage_window[window_index] = percentage;

    for (int i = 0; i < WINDOW_SIZE; i++) {
        avg_percentage += percentage_window[i];
    }

    percentage = avg_percentage / WINDOW_SIZE;

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
            case BATT_DISCONNECTED:status_str = "DISCONNECTED \xE2\x9A\xA0"; break;
        }

        // Print the formatted log message to the console
        ESP_LOGI(TAG, "Status: %s | Voltage: %.2fV | Level: %d%%", 
                status_str, voltage, percentage);

        // Block the task for 1 seconds to free up CPU
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}