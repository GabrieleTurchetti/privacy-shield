#include "afe.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_log_timestamp.h"
#include "esp_timer.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_vad.h"

#include "audio_hal.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "global_config.h"
#include "log_tags.h"
#include "model_path.h"
#include "noise_gen.h"
#include "portmacro.h"
#include "volume.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "xtensa/hal.h"

static const char *TAG = LOG_TAG_AUDIO_AFE;
static uint8_t last_volume = 0;

static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;

extern QueueHandle_t audio_input_queue;
extern QueueHandle_t audio_output_queue;
extern QueueHandle_t audio_intermediate_queue;

static audio_afe_vad_state_t AFE_STATE, LAST_AFE_STATE;
static bool valid_speaker = false;

static int16_t *speaker_buffer, *feed_buffer;
static uint16_t min_attack = 9999, max_attack = 0, attack_counter = 0, *attack,
				min_release = 9999, max_release = 0, release_counter = 0,
				*release;

static audio_afe_vad_state_t convert_vad_state(vad_state_t state) {
	switch (state) {
	case VAD_SPEECH:
		return AUDIO_AFE_VAD_SPEECH;

	case VAD_SILENCE:
		return AUDIO_AFE_VAD_SILENCE;

	default:
		return AUDIO_AFE_VAD_UNKNOWN;
	}
}

esp_err_t audio_afe_init(const char *input_format) {
	if (afe_data != NULL) {
		ESP_LOGW(TAG, "AFE already initialized");
		return ESP_OK;
	}

	if (input_format == NULL) {
		ESP_LOGE(TAG, "input_format is NULL");
		return ESP_ERR_INVALID_ARG;
	}

	// Loads ESP-SR model partition.
	srmodel_list_t *models = esp_srmodel_init("model");
	if (models == NULL) {
		ESP_LOGE(TAG, "Failed to initialize SR models. Check model partition.");
		return ESP_FAIL;
	}

	/*
	 * Examples:
	 * "M"   : one mic
	 * "MR"  : one mic + reference channel for AEC
	 * "MMR" : two mics + reference channel for AEC
	 *
	 * AFE_TYPE_SR is the speech-recognition front-end mode.
	 */
	afe_config_t *afe_config =
		afe_config_init(input_format, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);

	if (afe_config == NULL) {
		ESP_LOGE(TAG, "Failed to create AFE config");
		return ESP_FAIL;
	}

	/*
	 * VAD: Voice Activity Detection.
	 */
	afe_config->vad_init = true;
	afe_config->vad_mode = VAD_MODE_1;
	afe_config->vad_min_noise_ms = 500;
	afe_config->vad_min_speech_ms = 200;
	afe_config->vad_delay_ms = 128;

#ifdef CONFIG_SR_NSN_MODEL_QUANT
	afe_config->ns_init = true;
#else
	/*
	 * Leave this as-is if your ESP-SR version does not expose ns_init.
	 * You can still enable NS model selection from menuconfig.
	 */
	afe_config->ns_init = true;
#endif

	// AEC: Acoustic Echo Cancellation.
	valid_speaker = strchr(input_format, 'R') != NULL;
	if (valid_speaker) {
		afe_config->aec_init = true;
		ESP_LOGI(TAG, "AEC Enabled");
	} else {
		afe_config->aec_init = false;
		ESP_LOGW(
			TAG,
			"AEC disabled because input_format has no R reference channel");
	}

	afe_handle = esp_afe_handle_from_config(afe_config);
	if (afe_handle == NULL) {
		ESP_LOGE(TAG, "Failed to get AFE handle");
		afe_config_free(afe_config);
		return ESP_FAIL;
	}

	afe_data = afe_handle->create_from_config(afe_config);
	afe_config_free(afe_config);

	if (afe_data == NULL) {
		ESP_LOGE(TAG, "Failed to create AFE data");
		afe_handle = NULL;
		return ESP_FAIL;
	}

	// Allocating Buffers
	int feed_chunksize = afe_feed_chunksize();
	int fetch_chunksize = afe_feed_chunksize();
	int feed_channels = afe_feed_channels();

	if (valid_speaker) {
		speaker_buffer = (int16_t *)malloc(feed_chunksize * sizeof(int16_t));
		feed_buffer =
			(int16_t *)malloc(feed_chunksize * feed_channels * sizeof(int16_t));

		memset(speaker_buffer, 0, feed_chunksize * sizeof(int16_t));
	}

	// Allocating memory for data collection
	attack = malloc(100 * sizeof(int16_t));
	release = malloc(100 * sizeof(int16_t));

	memset(attack, 0, 100 * sizeof(int16_t));
	memset(release, 0, 100 * sizeof(int16_t));

	// Track VAD state change to avoid spamming the log console
	AFE_STATE = AUDIO_AFE_VAD_SILENCE;

	ESP_LOGI(TAG, "AFE initialized");
	ESP_LOGI(TAG, "input_format=%s", input_format);
	ESP_LOGI(TAG, "feed_chunksize=%d, feed_channels=%d", feed_chunksize,
			 feed_channels);
	ESP_LOGI(TAG, "fetch_chunksize=%d, fetch_channels=%d", fetch_chunksize,
			 afe_fetch_channels());
	ESP_LOGI(TAG, "VAD enabled");
	ESP_LOGI(TAG, "AEC %s", valid_speaker ? "enabled" : "disabled");

	return ESP_OK;
}

void audio_afe_feed(void *pvParameters) {
	ESP_LOGI(TAG, "AFE feed processing task started on Core %d",
			 xPortGetCoreID());

	if (afe_handle == NULL || afe_data == NULL) {
		ESP_LOGE(TAG, "AFE not initialized");
		vTaskDelete(NULL);
		return;
	}

	// Inital values for speaker buffer for purposes of AEC
	const int expected_samples = afe_feed_chunksize();
	audio_packet_t *packet = NULL;
	while (1) {
		if (xQueueReceive(audio_input_queue, &packet, portMAX_DELAY) ==
			pdTRUE) {

			if (packet == NULL || packet->audio_sample == NULL) {
				ESP_LOGE(TAG, "Recieved invalid audio packet");
				free_audio_packet(packet);
				packet = NULL;
				continue;
			}

			if (packet->sample_amount != (size_t)expected_samples) {
				ESP_LOGE(TAG, "Invalid packet size: got %zu, expected %d",
						 packet->sample_amount, expected_samples);

				free_audio_packet(packet);
				packet = NULL;
				continue;
			}

			int ret = 0;
			if (valid_speaker) {

				/**
				 * Interweaving the Speaker sample witht hhe microphone
				 * for AEC purposes as required when necessarry
				 * */
				for (int i = 0; i < expected_samples; i++) {
					feed_buffer[2 * i] = packet->audio_sample[i];
					feed_buffer[2 * i + 1] = speaker_buffer[i];
				}

				ret = afe_handle->feed(afe_data, feed_buffer);

			} else {
				ret = afe_handle->feed(afe_data, packet->audio_sample);
			}

			if (ret < 0) {
				ESP_LOGE(TAG, "AFE feed returned: %d", ret);
			}

			if (xQueueSend(audio_intermediate_queue, &packet, 1) != pdPASS) {
				ESP_LOGE(TAG, "Failed to send to AFE queue");
				free_audio_packet(packet);
			}
		}
		packet = NULL;
	}
}

void audio_afe_fetch(void *pvParameters) {
	ESP_LOGI(TAG, "AFE feed processing task started on Core %d",
			 xPortGetCoreID());

	if (afe_handle == NULL || afe_data == NULL) {
		ESP_LOGE(TAG, "Uninitialized AFE_HANDLER or AFE_DATA");
		vTaskDelete(NULL);
		return;
	}

	volume_state_t vol_state;
	volume_init(&vol_state);
	noise_gen_init(NOISE_TYPE_PINK);
	int64_t attack_start_us = 0;
	int64_t release_start_us = 0;
	bool release_active = false;
	bool attack_active = false;
	uint8_t attack_target_pct = 0, release_target_pct = 0;

	uint8_t VOLUME_TOLERANCE_PCT = 2, MIN_VOLUME_PCT = 2;
	uint8_t prev_vol_target = 0;
	uint8_t volume_pct = 0;

	audio_packet_t *packet;
	while (1) {
		afe_fetch_result_t *result = afe_handle->fetch(afe_data);

		if (result == NULL) {
			ESP_LOGE(TAG, "AFE fetch returned NULL");
			continue;
		}

		if (result->ret_value != ESP_OK) {
			ESP_LOGE(TAG, "AFE fetch failed :%d", result->ret_value);
			continue;
		}

		if (xQueueReceive(audio_intermediate_queue, &packet, portMAX_DELAY) !=
			pdTRUE) {
			ESP_LOGE(TAG, "Failed to Recieve audio packet from "
						  "AFE_INTERMEDIATE_QUEUE");
			continue;
		}

		if (packet == NULL || packet->audio_sample == NULL) {
			ESP_LOGE(TAG,
					 "Recieved invalid audio packet from Intermediate Queue");
			free_audio_packet(packet);
			packet = NULL;
			continue;
		}

		audio_afe_vad_state_t state = convert_vad_state(result->vad_state);
		if (state != AFE_STATE) {
			if (state == AUDIO_AFE_VAD_SPEECH) {
				ESP_LOGI(TAG, "[VAD] Speech detected!");
			} else if (state == AUDIO_AFE_VAD_SILENCE) {
				ESP_LOGI(TAG, "[VAD] Silence...");
			} else {
				ESP_LOGW(TAG, "[VAD] Unknown VAD state.");
			}

			AFE_STATE = result->vad_state;
		}

		if (valid_speaker) {

			noise_gen_fill(speaker_buffer, (int)packet->sample_amount);
			bool masking;

			last_volume = volume_pct;
			if (AFE_STATE == AUDIO_AFE_VAD_SPEECH) {

				// Someone talking — normal volume from RMS
				volume_pct = volume_process_frame(
					&vol_state, packet->audio_sample, speaker_buffer,
					(int)packet->sample_amount, &masking);

			} else {
				// Silence — force ramp to zero
				vol_state.target = 0.0f;
				float level = volume_ramp(&vol_state, vol_state.target);
				apply_volume(speaker_buffer, (int)packet->sample_amount, level);
				volume_pct = (uint8_t)(level * 100.0f);
				masking = false;
			}

			uint8_t target_vol_pct = get_target_volume_pct(&vol_state);
			if (prev_vol_target != target_vol_pct) {
				if (esp_timer_get_time() % 1000000) {

					ESP_LOGI(TAG, "New Volume Target = %u%%", target_vol_pct);
				}
			}
			prev_vol_target = target_vol_pct;

			/*
			 * ATTACK: target is above current volume.
			 */
			if (!attack_active &&
				target_vol_pct > volume_pct + VOLUME_TOLERANCE_PCT) {

				attack_active = true;
				release_active = false;

				attack_start_us = esp_timer_get_time();
				attack_target_pct = target_vol_pct;
			}

			/*
			 * RELEASE: target is below current volume.
			 */
			if (!release_active && target_vol_pct < volume_pct) {

				release_active = true;

				release_start_us = esp_timer_get_time();
				release_target_pct = target_vol_pct;
			}

			/*
			 * Attack completes when current reaches target.
			 */
			if (attack_active &&
				(volume_pct + VOLUME_TOLERANCE_PCT >= attack_target_pct)) {

				int64_t attack_ms =
					(esp_timer_get_time() - attack_start_us) / 1000;

				ESP_LOGI(TAG, "Attack time: %" PRId64 " ms", attack_ms);
				update_afe_values(attack_ms, attack);
				// attack_target_pct = 0;
				attack_active = false;
				attack_start_us = 0;
			}

			/*
			 * Release completes when current reaches target.
			 */
			if (release_active && (volume_pct <= release_target_pct)) {
				int64_t release_ms =
					(esp_timer_get_time() - release_start_us) / 1000;

				ESP_LOGI(TAG, "Release time: %" PRId64 " ms", release_ms);
				update_afe_values(release_ms, release);

				release_active = false;
			}

			if (abs(volume_pct - last_volume) >= 5) {
				if (masking) {
					ESP_LOGI(TAG, "Masking active — volume %u%%", volume_pct);

				} else {
					ESP_LOGI(TAG, "Masking inactive — volume %u%%", volume_pct);
				}
			}

			memcpy(packet->audio_sample, speaker_buffer,
				   packet->sample_amount * sizeof(packet->audio_sample[0]));

			if (xQueueSend(audio_output_queue, &packet, 1) != pdPASS) {
				ESP_LOGE(TAG, "Failed to send to Speaker");
				free_audio_packet(packet);
			}

			LAST_AFE_STATE = AFE_STATE;
		} else {
			free_audio_packet(packet);
		}
		packet = NULL;
	}
}

static void update_afe_values(int64_t timestamp, uint16_t *buffer) {

	if (&buffer == &attack) {
		if (timestamp > max_attack) {
			max_attack = timestamp;
		}
		if (timestamp <= min_attack) {
			min_attack = timestamp;
		}

		buffer[attack_counter] = timestamp;
		attack_counter++;
	} else {
		if (timestamp > max_release) {
			max_release = timestamp;
		}
		if (timestamp <= min_release) {
			min_release = timestamp;
		}
		buffer[release_counter] = timestamp;
		release_counter++;
	}
}

afe_data_points_t get_afe_delays() {
	double att = 0.0;
	double rel = 0.0;
	int counter_att = 0;
	int counter_rel = 0;

	for (int i = 0; i < 100; i++) {
		if (attack[i] != 0) {
			att += (double)attack[i];
			counter_att++;
		}
		if (release[i] != 0) {
			rel += (double)release[i];
			counter_rel++;
		}
	}
	afe_data_points_t points;
	points.avg_attack = att / 100;
	points.avg_release = rel / 100;
	points.max_attack = max_attack;
	points.max_release = max_release;
	points.min_attack = min_attack;
	points.min_release = min_release;
	return points;
}

bool is_afe_speech() { return AFE_STATE == AUDIO_AFE_VAD_SPEECH; }

int afe_feed_chunksize() {
	if (afe_data == NULL || afe_handle == NULL) {
		ESP_LOGE(TAG,
				 "AFE not initialized, can't fetch chunksize will return 0");
		return 0;
	}
	return afe_handle->get_feed_chunksize(afe_data);
}

int afe_feed_channels() {
	if (afe_data == NULL || afe_handle == NULL) {
		ESP_LOGE(TAG, "AFE not initialized, can't get feed channels");
		return 0;
	}
	return afe_handle->get_feed_channel_num(afe_data);
}

int afe_fetch_chunksize() {
	if (afe_data == NULL || afe_handle == NULL) {
		ESP_LOGE(TAG,
				 "AFE not initialized, can't fetch chunksize will return 0");
		return 0;
	}
	return afe_handle->get_fetch_chunksize(afe_data);
}

int afe_fetch_channels() {
	if (afe_data == NULL || afe_handle == NULL) {
		ESP_LOGE(TAG, "AFE not initialized, can't get feed channels");
		return 0;
	}
	return afe_handle->get_fetch_channel_num(afe_data);
}

void audio_afe_destroy(void) {
	if (afe_handle != NULL && afe_data != NULL) {
		afe_handle->destroy(afe_data);
	}
	if (speaker_buffer != NULL) {
		free(speaker_buffer);
	}
	if (feed_buffer != NULL) {
		free(feed_buffer);
	}
	free(attack);
	free(release);

	afe_data = NULL;
	afe_handle = NULL;
	ESP_LOGI(TAG, "AFE destroyed");
}
