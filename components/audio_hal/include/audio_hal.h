#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include <stdint.h>

// ==========================================================
// AUDIO HARDWARE ABSTRACTION LAYER (HAL)
// ==========================================================

// Initialize the I2S microphone hardware
esp_err_t audio_hal_mic_init(void);

// FreeRTOS task for continuously reading microphone data
void audio_hal_mic_read_task(void *pvParameters);

#endif // AUDIO_HAL_H
