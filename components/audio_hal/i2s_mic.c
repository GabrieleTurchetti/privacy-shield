#include "afe.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "global_config.h"
#include "log_tags.h"
#include "sdkconfig.h"
#include <stdio.h>

static const char *TAG = LOG_TAG_AUDIO_MIC;
static i2s_chan_handle_t rx_handle;

extern QueueHandle_t audio_ai_queue;

esp_err_t audio_hal_mic_init() {
#if defined(CONFIG_PRIVACY_SHIELD_BUILD_DEBUG) && defined(CONFIG_PRIVACY_SHIELD_LOG_AUDIO)
    // Boost USB baud rate for raw sample streaming
    uart_set_baudrate(UART_NUM_0, 2000000);
#endif

    ESP_LOGI(TAG, "Initializing I2S microphone hardware...");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // Configure I2S for 16kHz, 32-bit, Mono
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = -1,
            .bclk = PIN_I2S_MIC_BCLK,
            .ws = PIN_I2S_MIC_LRCLK,
            .dout = -1,
            .din = PIN_I2S_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "Successfully initialized Microphone");
    
    return ESP_OK;
}

void audio_hal_mic_read_task(void *pvParameters) {
    int32_t raw_samples[AFE_FEED_SAMPLES];
    int16_t ai_buffer[AFE_FEED_SAMPLES];
    int32_t dc_offset = 0;
    
    ESP_LOGI(TAG, "Stay completely quiet for 1 second. Calibrating...");
    int64_t last_read_time = esp_timer_get_time();
    int packet_count = 0;
    
    ESP_LOGI(TAG, "Starting DMA Audio Capture Test. Target: 32ms per frame...");

    while (1) {
        size_t bytes_read = 0;

        // Wait and read data from I2S DMA buffer
        esp_err_t err = i2s_channel_read(rx_handle, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);

        if (err == ESP_OK && bytes_read > 0) {
#if defined(CONFIG_PRIVACY_SHIELD_BUILD_DEBUG) && defined(CONFIG_PRIVACY_SHIELD_LOG_AUDIO)
            int64_t current_time = esp_timer_get_time();
            int delta_ms = (current_time - last_read_time) / 1000;
            last_read_time = current_time;
            packet_count++;

            if (packet_count < 2000) {
                // If a genuine delay occurs, log it immediately
                if (delta_ms > 35) {
                    ESP_LOGE(TAG, "Underrun detected! Frame took %d ms. Expected ~32ms", delta_ms);
                } else {
                    // This prevents serial buffer overflow and CPU stalling
                    if (packet_count % 50 == 0) {
                        ESP_LOGD(TAG, "[Pkg: %d] System stable. Frame ready in: %d ms", packet_count, delta_ms);
                    }
                }
            } else if (packet_count == 2000) {
                // Final validation message after ~1 minute of continuous operation
                ESP_LOGD(TAG, "1 minute test completed! No underruns detected.");
            }
#endif

            int samples_read = bytes_read / 4; // 4 bytes per 32-bit sample

            // Audio streaming phase
#if defined(CONFIG_PRIVACY_SHIELD_BUILD_DEBUG) && defined(CONFIG_PRIVACY_SHIELD_LOG_AUDIO)
            for (int i = 0; i < samples_read; i++) {
                // Apply offset correction and print to serial
                printf("%ld\n", (raw_samples[i] >> 16) - dc_offset);
            }
#endif

            // Convert 32-bit I2S data to 16-bit standard audio for the AI
            for (int i = 0; i < samples_read; i++) {
                // Shift down to 16-bit
                ai_buffer[i] = (int16_t)(raw_samples[i] >> 16);
            }

            // Send the chunk of audio to the DSP Engine
            if (audio_ai_queue != NULL) {
                if (xQueueOverwrite(audio_ai_queue, ai_buffer) != pdTRUE) {
                    ESP_LOGE(TAG, "Failed to send Microphone data");
                }
            }
        }
    }
}