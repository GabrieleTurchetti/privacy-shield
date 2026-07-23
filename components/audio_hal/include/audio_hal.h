#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include "afe.h"
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// ==========================================================
// AUDIO HARDWARE ABSTRACTION LAYER (HAL)
// ==========================================================

#define SPK_SAMPLE_RATE 16000
#define AUDIO_FRAME_MAX_LENGTH 512

// Initialize the I2S microphone hardware
esp_err_t audio_hal_mic_init(void);

// FreeRTOS task for continuously reading microphone data
void audio_hal_mic_read_task(void *pvParameters);

// Initialize the amplifier hardware (I2S TX)
void audio_hal_speaker_init(void);

// Task to test the exciter with a pure sine wave tone
// Todo: Remove this
// void sine_wave_task(void *pvParameters);

void audio_hal_speaker_task(void *pvParameters);

typedef struct {
	int64_t timestamp;
	size_t sample_size;
	int16_t audio_sample[AUDIO_FRAME_MAX_LENGTH];
} audio_packet_t;

void free_audio_packet(audio_packet_t *packet);

static void update_delays(int16_t timestamp);

double get_avg_delay(void);

double get_max_delay(void);

double get_min_delay(void);

#endif // AUDIO_HAL_H
