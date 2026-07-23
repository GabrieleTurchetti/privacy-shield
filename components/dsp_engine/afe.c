#include "afe.h"

#include <math.h>
#include <stdbool.h>
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

#define AFE_DATA_BUFFER_SIZE 100
#define SPEAKER_HISTORY_SIZE 6
#define REF_CAL_MIN_CORRELATION 0.20

static const char *TAG = LOG_TAG_AUDIO_AFE;
static uint8_t last_volume = 0;

static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;

extern QueueHandle_t audio_input_queue, audio_output_queue,
	audio_intermediate_queue;

static audio_afe_vad_state_t AFE_STATE, LAST_AFE_STATE;
static bool valid_speaker = false, calibration_set = false;

static int16_t speaker_buffer[SPEAKER_HISTORY_SIZE][AUDIO_FRAME_MAX_LENGTH],
	feed_buffer[2 * AUDIO_FRAME_MAX_LENGTH];

static uint16_t min_attack = 9999, max_attack = 0, attack[AFE_DATA_BUFFER_SIZE],
				min_release = 9999, max_release = 0,
				release[AFE_DATA_BUFFER_SIZE];

static uint8_t attack_counter = 0, release_counter = 0,
			   speaker_buffer_counter = 0;
static int speaker_buffer_offset = 0;

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

bool get_calibration_status() { return calibration_set; }

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
		memset(speaker_buffer, 0, sizeof(speaker_buffer));
		memset(feed_buffer, 0, sizeof(feed_buffer));
		memset(attack, 0, sizeof(attack));
		memset(release, 0, sizeof(release));
		speaker_buffer_offset = 3;
	}

	// Allocating memory for data collection

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

void afe_calibration() {
	if (!valid_speaker) {
		return;
	}

	ESP_LOGI(TAG, "Calibrating AFE...");
	volume_state_t vol_state;
	volume_init(&vol_state);
	noise_gen_init(NOISE_TYPE_PINK);
	float volume_level = 0.70f;
	int history = -1;
	int expected_samples = afe_feed_chunksize();
	audio_packet_t *packet = NULL;

	for (size_t i = 0; i < SPEAKER_HISTORY_SIZE; i++) {
		packet = malloc(sizeof(*packet));
		if (packet == NULL) {
			continue;
		} else {
			packet->sample_size = expected_samples;
			packet->timestamp = esp_timer_get_time();

			noise_gen_fill(speaker_buffer[i], (int)packet->sample_size);

			apply_volume(speaker_buffer[i], expected_samples, volume_level);

			send_to_speaker(packet);
		}
	}

	while (history == -1) {
		if (xQueueReceive(audio_input_queue, &packet, portMAX_DELAY) ==
			pdTRUE) {
			if (packet == NULL) {
				ESP_LOGE(TAG, "Recieved invalid audio packet");
				free_audio_packet(packet);
				packet = NULL;
				continue;
			} else {
				if (packet->sample_size != (size_t)expected_samples) {
					ESP_LOGE(TAG, "Invalid packet size: got %zu, expected %d",
							 packet->sample_size, expected_samples);
					free_audio_packet(packet);
					packet = NULL;
					continue;
				} else {
					history = find_best_reference_frame(packet->audio_sample,
														packet->sample_size);
					if (history != -1) {
						speaker_buffer_offset =
							speaker_buffer_counter - history;
						if (speaker_buffer_offset < 0) {
							speaker_buffer_offset = -speaker_buffer_offset;
						}
						calibration_set = true;
					}

					noise_gen_fill(speaker_buffer[speaker_buffer_counter],
								   packet->sample_size);
					apply_volume(speaker_buffer[speaker_buffer_counter],
								 packet->sample_size, volume_level);
					send_to_speaker(packet);
				}
			}
		}
	}
	ESP_LOGI(TAG, "Done Calibrating speaker offset off: %d",
			 speaker_buffer_offset);
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
	int16_t zero_ref_sample[AFE_DATA_BUFFER_SIZE];
	memset(zero_ref_sample, 0, sizeof(zero_ref_sample));
	audio_packet_t *packet = NULL;
	while (1) {
		if (xQueueReceive(audio_input_queue, &packet, portMAX_DELAY) ==
			pdTRUE) {

			if (packet == NULL) {
				ESP_LOGE(TAG, "Recieved invalid audio packet");
				free_audio_packet(packet);
				packet = NULL;
				continue;
			} else {
				if (packet->sample_size != (size_t)expected_samples) {
					ESP_LOGE(TAG, "Invalid packet size: got %zu, expected %d",
							 packet->sample_size, expected_samples);

					free_audio_packet(packet);
					packet = NULL;
					continue;
				} else {
					int ret = 0;
					if (valid_speaker) {
						int offset = find_best_reference_frame(
							packet->audio_sample, packet->sample_size);
						/**
						 * Interweaving the Speaker sample witht hhe microphone
						 * for AEC purposes as required when necessarry
						 * */
						for (int i = 0; i < expected_samples; i++) {
							feed_buffer[2 * i] = packet->audio_sample[i];
							if (offset != -1) {

								feed_buffer[2 * i + 1] =
									speaker_buffer[offset][i];
							} else {
								feed_buffer[2 * i + 1] = zero_ref_sample[i];
							}
						}

						ret = afe_handle->feed(afe_data, feed_buffer);

					} else {
						ret = afe_handle->feed(afe_data, packet->audio_sample);
					}

					if (ret < 0) {
						ESP_LOGE(TAG, "AFE feed returned: %d", ret);
					}

					if (xQueueSend(audio_intermediate_queue, &packet, 1) !=
						pdPASS) {
						ESP_LOGE(TAG, "Failed to send to AFE queue");
						free_audio_packet(packet);
					}
				}
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

	uint8_t VOLUME_TOLERANCE_PCT = 2;
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

		if (packet == NULL) {
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
			noise_gen_fill(speaker_buffer[speaker_buffer_counter],
						   (int)packet->sample_size);
			bool masking;

			// Someone talking — normal volume from RMS
			volume_pct = volume_process_frame(
				&vol_state, packet->audio_sample,
				speaker_buffer[speaker_buffer_counter],
				(int)packet->sample_size, &masking, is_afe_speech());

			uint8_t target_vol_pct = get_target_volume_pct(&vol_state);
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
				release_start_us = esp_timer_get_time();
				if (attack_active) {
					int64_t attack_ms =
						(release_start_us - attack_start_us) / 1000;

					ESP_LOGI(TAG, "Attack time: %" PRId64 " ms", attack_ms);
					update_attack(attack_ms);
					attack_active = false;
				}

				release_active = true;
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
				update_attack(attack_ms);
				attack_active = false;
			}

			/*
			 * Release completes when current reaches target.
			 */
			if (release_active && (volume_pct <= release_target_pct)) {
				int64_t release_ms =
					(esp_timer_get_time() - release_start_us) / 1000;
				update_release(release_ms);

				ESP_LOGI(TAG, "Release time: %" PRId64 " ms", release_ms);
				release_active = false;
			}

			if (abs(volume_pct - last_volume) >= 5) {
				if (masking) {
					ESP_LOGI(TAG, "Masking active — volume %u%%", volume_pct);

				} else {
					ESP_LOGI(TAG, "Masking inactive — volume %u%%", volume_pct);
				}
				last_volume = volume_pct;
			}

			send_to_speaker(packet);
			if (AFE_STATE != LAST_AFE_STATE) {

				LAST_AFE_STATE = AFE_STATE;
			}

		} else {
			free_audio_packet(packet);
		}
		packet = NULL;
	}
}

static void send_to_speaker(audio_packet_t *packet) {
	if (packet == NULL) {
		return;
	}

	memcpy(packet->audio_sample, speaker_buffer[speaker_buffer_counter],
		   packet->sample_size * sizeof(packet->audio_sample[0]));

	if (xQueueSend(audio_output_queue, &packet, 1) != pdPASS) {
		ESP_LOGE(TAG, "Failed to send to Speaker");
		free_audio_packet(packet);
	}

	speaker_buffer_counter++;
	if (speaker_buffer_counter >= SPEAKER_HISTORY_SIZE) {
		speaker_buffer_counter = 0;
	}
}

/*
 * ZNCC - Zero-Mean Normalized Cross-Correlation
 */
static float audio_zncc(const int16_t *mic, const int16_t *ref,
						size_t sample_count) {
	if (mic == NULL || ref == NULL || sample_count == 0) {
		return 0.0f;
	}

	double mic_mean = 0.0;
	double ref_mean = 0.0;

	for (size_t i = 0; i < sample_count; i++) {
		mic_mean += (double)mic[i];
		ref_mean += (double)ref[i];
	}

	mic_mean /= (double)sample_count;
	ref_mean /= (double)sample_count;

	double cross_sum = 0.0;
	double mic_energy = 0.0;
	double ref_energy = 0.0;

	for (size_t i = 0; i < sample_count; i++) {
		double m = (double)mic[i] - mic_mean;
		double r = (double)ref[i] - ref_mean;

		cross_sum += m * r;
		mic_energy += m * m;
		ref_energy += r * r;
	}

	double denominator = sqrt(mic_energy * ref_energy);

	if (denominator < 1e-12) {
		return 0.0f;
	}

	return (float)(cross_sum / denominator);
}

/**
 * @brief
 * Tries to find the best frame for mic frame from speaker
 * @return either index of history frame or -1 indicating either no
 * good match found or could not find the history frames
 * */
static int find_best_reference_frame(const int16_t *mic_frame,
									 size_t sample_size) {

	if (mic_frame == NULL) {
		return -1;
	}

	float best_abs_score = 0.0f;
	int best_score_int = -1;

	for (size_t index = 0; index < SPEAKER_HISTORY_SIZE; index++) {
		const int16_t *ref_frame = speaker_buffer[index];

		if (ref_frame == NULL) {
			ESP_LOGW(TAG, "Reference frame not found");
			return -1;
		}

		float score = audio_zncc(mic_frame, ref_frame, sample_size);

		float abs_score = fabsf(score);

		if (abs_score > best_abs_score) {
			best_abs_score = abs_score;
			best_score_int = (int)index;
		}
	}

	if (best_abs_score >= REF_CAL_MIN_CORRELATION) {
		return best_score_int;
	}

	return -1;
}

static void update_attack(int64_t timestamp) {
	if (timestamp > max_attack) {
		max_attack = timestamp;
	}
	if (timestamp < min_attack) {
		min_attack = timestamp;
	}
	attack[attack_counter] = (uint16_t)timestamp;
	attack_counter++;
	if (attack_counter >= 100) {
		attack_counter = 0;
	}
}

static void update_release(int64_t timestamp) {
	if (timestamp > max_release) {
		max_release = timestamp;
	}
	if (timestamp < min_release) {
		min_release = timestamp;
	}
	release[release_counter] = (uint16_t)timestamp;
	release_counter++;
	if (release_counter >= 100) {
		release_counter = 0;
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

bool has_valid_reference() { return valid_speaker; }

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

	afe_data = NULL;
	afe_handle = NULL;
	ESP_LOGI(TAG, "AFE destroyed");
}
