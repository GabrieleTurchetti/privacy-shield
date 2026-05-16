#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "driver/uart.h"

// Pin configuration
#define I2S_WS_PIN    4  
#define I2S_DOUT_PIN  5  
#define I2S_BCLK_PIN  6

#define TEST_MIC true

static const char *TAG = "AUDIO_HAL_MIC";
static i2s_chan_handle_t rx_handle;

// Assume you created this queue in main.c to connect the Mic to the AI
extern QueueHandle_t audio_ai_queue;

void audio_hal_mic_init(void) {
    // Boost USB baud rate for high data throughput
    uart_set_baudrate(UART_NUM_0, 2000000);
    
    ESP_LOGI(TAG, "Initializing I2S microphone hardware...");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // Configure I2S for 16kHz, 32-bit, Mono
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000), 
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = -1,             
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = -1,             
            .din  = I2S_DOUT_PIN,   
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; 
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

void audio_hal_mic_read_task(void *pvParameters) {
    int32_t raw_samples[512];
    int16_t ai_buffer[512];
    int32_t dc_offset = 0;
    bool is_calibrated = false;
    long long calibration_sum = 0;
    int calibration_samples_read = 0;
    ESP_LOGI(TAG, "Stay completely quiet for 1 second. Calibrating...");

    while(1) {
        size_t bytes_read = 0;
        
        // Wait and read data from I2S DMA buffer
        esp_err_t err = i2s_channel_read(rx_handle, raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);
        
        if (err == ESP_OK && bytes_read > 0) {
            int samples_read = bytes_read / 4; // 4 bytes per 32-bit sample

            // DC offset calibration phase
            if (!is_calibrated) {
                for (int i = 0; i < samples_read; i++) {
                    calibration_sum += (raw_samples[i] >> 16);
                    calibration_samples_read++;
                }
                
                // After 1 second of audio (16000 samples at 16kHz)
                if (calibration_samples_read >= 16000) {
                    dc_offset = calibration_sum / calibration_samples_read;
                    is_calibrated = true;
                    ESP_LOGI(TAG, "Calibration complete! DC Offset: %ld", dc_offset);
                }
            } 
            // Audio streaming phase
            else {
                if (TEST_MIC) {
                    for (int i = 0; i < samples_read; i++) {
                        // Apply offset correction and print to serial
                        printf("%ld\n", (raw_samples[i] >> 16) - dc_offset);
                    }
                }
                else {
                    // Convert 32-bit I2S data to 16-bit standard audio for the AI
                    for (int i = 0; i < samples_read; i++) {
                        // Shift down to 16-bit
                        ai_buffer[i] = (int16_t)(raw_samples[i] >> 16); 
                    }

                    // Send the chunk of audio to the DSP Engine
                    /* if (audio_ai_queue != NULL) {
                        xQueueSend(audio_ai_queue, &ai_buffer, 0);
                    } */
                }
            }
        }
    }
}