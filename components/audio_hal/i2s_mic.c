#include "afe.h"
#include "audio_hal.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "global_config.h"
#include "log_tags.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = LOG_TAG_AUDIO_MIC;
static i2s_chan_handle_t rx_handle;

extern QueueHandle_t audio_input_queue;

esp_err_t audio_hal_mic_init() { // TODO: If this always returns ESP_OK, should
								 // it be a void function instead?
	ESP_LOGI(TAG, "Initializing I2S microphone hardware...");
	i2s_chan_config_t chan_cfg =
		I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
	ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

	// Configure I2S for 16kHz, 32-bit, Mono
	i2s_std_config_t std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
			I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
		.gpio_cfg =
			{
				.mclk = -1,
				.bclk = PIN_I2S_MIC_BCLK,
				.ws = PIN_I2S_MIC_LRCLK,
				.dout = -1,
				.din = PIN_I2S_MIC_DIN,
				.invert_flags =
					{
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
	int feed_chunksize = afe_feed_chunksize();
	int32_t raw_samples[feed_chunksize];
	int16_t ai_buffer[feed_chunksize];
	int32_t dc_offset = 0;
	bool is_calibrated = false;
	int64_t calibration_sum = 0;
	int calibration_samples_read = 0;

	ESP_LOGI(TAG, "Stay completely quiet for 1 second. Calibrating...");
	int64_t last_read_time = esp_timer_get_time();
	int packet_count = 0;

	ESP_LOGI(TAG, "Starting DMA Audio Capture Test. Target: 32ms per frame...");

	while (1) {
		size_t bytes_read = 0;

		esp_err_t err =
			i2s_channel_read(rx_handle, raw_samples, sizeof(raw_samples),
							 &bytes_read, portMAX_DELAY);

		if (err == ESP_OK && bytes_read > 0) {
			int samples_read = bytes_read / 4;
			int64_t current_time = esp_timer_get_time();

			/* ── DC offset calibration (first 1 second) ── */
			if (!is_calibrated) {
				for (int i = 0; i < samples_read; i++) {
					calibration_sum += (raw_samples[i] >> 16);
					calibration_samples_read++;
				}
				if (calibration_samples_read >= 16000) {
					dc_offset = calibration_sum / calibration_samples_read;
					is_calibrated = true;
					ESP_LOGI(TAG, "Calibration complete! DC Offset: %ld",
							 dc_offset);
				}
				continue;
			}

#if defined(CONFIG_PRIVACY_SHIELD_BUILD_DEBUG) &&                              \
	defined(CONFIG_PRIVACY_SHIELD_LOG_AUDIO)
			int delta_ms = (current_time - last_read_time) / 1000;
			last_read_time = current_time;
			packet_count++;

			if (packet_count < 2000) {
				if (delta_ms > 35) {
					ESP_LOGW(TAG, "Underrun detected! Frame took %d ms",
							 delta_ms);
				} else {
					if (packet_count % 50 == 0) {
						ESP_LOGD(
							TAG,
							"[Pkg: %d] System stable. Frame ready in: %d ms",
							packet_count, delta_ms);
					}
				}
			} else if (packet_count == 2000) {
				ESP_LOGD(TAG,
						 "1 minute test completed! No underruns detected.");
			}
#endif

			/* ── Convert and apply DC offset correction ── */
			for (int i = 0; i < samples_read; i++) {
				ai_buffer[i] = (int16_t)((raw_samples[i] >> 16) - dc_offset);
			}

			/* ── Debug: raw dump ── */
#if defined(CONFIG_PRIVACY_SHIELD_BUILD_DEBUG) &&                              \
	defined(CONFIG_PRIVACY_SHIELD_LOG_AUDIO)
			for (int i = 0; i < samples_read; i++) {
				ESP_LOGD(TAG, "%ld\n", ai_buffer[i]);
			}
#endif
			audio_packet_t *packet = malloc(sizeof(*packet));
			if (packet == NULL) {
				ESP_LOGE(TAG, "Failed to allocate packet");
				continue;
			}

			packet->audio_sample =
				malloc(feed_chunksize * sizeof(packet->audio_sample[0]));
			if (packet->audio_sample == NULL) {
				ESP_LOGE(TAG, "Failed to allocate packet audio");
				free_audio_packet(packet);
				continue;
			}

			memcpy(packet->audio_sample, ai_buffer,
				   feed_chunksize * sizeof(packet->audio_sample[0]));
			packet->timestamp = current_time;
			packet->sample_amount = (size_t)feed_chunksize;

			/* ── Send to AFE pipeline ── */
			if (audio_input_queue != NULL) {
				if (xQueueSend(audio_input_queue, (void *)&packet, 1) !=
					pdPASS) {
					ESP_LOGE(TAG, "Failed to send Microphone data");
					free_audio_packet(packet);
				}
			} else {
				free_audio_packet(packet);
			}
		}
	}
}

void free_audio_packet(audio_packet_t *packet) {
	if (packet == NULL) {
		return;
	}

	free(packet->audio_sample);
	free(packet);
}
