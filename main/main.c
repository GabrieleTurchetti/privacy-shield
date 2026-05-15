#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "audio_hal.h"

static const char *TAG = "MAIN_APP";

void app_main(void)
{
    ESP_LOGI(TAG, "Booting Privacy Shield System...");

    // 1. Initialize Hardware Components
    audio_hal_mic_init();

    // 2. Start Background Tasks
    // Run the microphone reading logic on Core 1
    xTaskCreatePinnedToCore(audio_hal_mic_read_task, "Mic_Task", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System running.");
}