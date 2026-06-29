#include "afe.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_vad.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "global_config.h"
#include "log_tags.h"
#include "model_path.h"
#include "noise_gen.h"
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

static audio_afe_vad_state_t AFE_STATE;
static bool valid_speaker = false;

static int16_t *microphone_buffer;
static int16_t *speaker_buffer;
static int16_t *feed_buffer;

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

	/*
	 * Loads ESP-SR model partition.
	 *
	 * Your partition table must contain a model partition,
	 * and sdkconfig/menuconfig must include the selected AFE/VAD/NS models.
	 */
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

	/*
	 * NS: Noise Suppression.
	 *
	 * In current ESP-SR AFE, NS/NSNet availability also depends on
	 * selected models/options in menuconfig.
	 *
	 * Depending on the exact ESP-SR version, this field may be:
	 * afe_config->ns_init
	 * or model/menuconfig-controlled.
	 *
	 * If this line fails to compile, comment it out and enable NS in:
	 * idf.py menuconfig -> ESP Speech Recognition
	 */
#ifdef CONFIG_SR_NSN_MODEL_QUANT
	afe_config->ns_init = true;
#else
	/*
	 * Leave this as-is if your ESP-SR version does not expose ns_init.
	 * You can still enable NS model selection from menuconfig.
	 */
	afe_config->ns_init = true;
#endif

	/*
	 * AEC: Acoustic Echo Cancellation.
	 *
	 * AEC only makes sense if input_format contains an R reference channel,
	 * for example "MR" or "MMR".
	 *
	 * The R channel should be the audio you are sending to the speaker,
	 * time-aligned as well as possible with the mic input.
	 */
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

	int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
	int feed_channels = afe_handle->get_feed_channel_num(afe_data);
	int fetch_chunksize = afe_handle->get_fetch_chunksize(afe_data);
	int fetch_channels = afe_handle->get_fetch_channel_num(afe_data);

	// Allocating Buffers
	int feed_chunksize = get_feed_chunksize();
	int fetch_chunksize = get_feed_chunksize();
	int feed_channels = get_feed_channels();

	microphone_buffer = (int16_t *)malloc(feed_chunksize * sizeof(int16_t));

	if (valid_speaker) {
		speaker_buffer = (int16_t *)malloc(feed_chunksize * sizeof(int16_t));
		feed_buffer =
			(int16_t *)malloc(feed_chunksize * feed_channels * sizeof(int16_t));
	}

	memset(microphone_buffer, 0, feed_chunksize * sizeof(int16_t));
	memset(speaker_buffer, 0, feed_chunksize * sizeof(int16_t));

	// Track VAD state change to avoid spamming the log console
	AFE_STATE = AUDIO_AFE_VAD_SILENCE;

	ESP_LOGI(TAG, "AFE initialized");
	ESP_LOGI(TAG, "input_format=%s", input_format);
	ESP_LOGI(TAG, "feed_chunksize=%d, feed_channels=%d", feed_chunksize,
			 feed_channels);
	ESP_LOGI(TAG, "fetch_chunksize=%d, fetch_channels=%d", fetch_chunksize,
			 get_fetch_channels());
	ESP_LOGI(TAG, "VAD enabled");
	ESP_LOGI(TAG, "AEC %s", valid_speaker ? "enabled" : "disabled");

	return ESP_OK;
}

void audio_afe_feed(void *pvParameters) {
	ESP_LOGI(TAG, "AFE feed processing task started on Core %d",
			 xPortGetCoreID());

	if (afe_handle == NULL || afe_data == NULL) {
		ESP_LOGE(TAG, "AFE not initialized");
	}

	// Inital values for speaker buffer for purposes of AEC
	int feed_chunksize = afe_get_chunksize();
	if (valid_speaker) {
		memset(speaker_buffer, 0, feed_chunksize * sizeof(int16_t));
	}
	while (1) {
		if (xQueueReceive(audio_input_queue, microphone_buffer,
						  portMAX_DELAY) == pdTRUE) {

			int ret = 0;
			if (valid_speaker) {

				for (int i = 0; i < feed_chunksize; i++) {
					feed_buffer[2 * i] = microphone_buffer[i];
					feed_buffer[2 * i + 1] = speaker_buffer[i];
				}

				ret = afe_handle->feed(afe_data, feed_buffer);

			} else {
				ret = afe_handle->feed(afe_data, microphone_buffer);
			}

			if (ret < 0) {
				ESP_LOGE(TAG, "AFE feed returned: %d", ret);
			}
		}
	}
}

void audio_afe_fetch(void *pvParameters) {
	ESP_LOGI(TAG, "AFE feed processing task started on Core %d",
			 xPortGetCoreID());

	if (afe_handle == NULL || afe_data == NULL) {
		ESP_LOGE(TAG, "Uninitialized AFE_HANDLER or AFE_DATA");
		return;
	}
	volume_state_t vol_state;
	volume_init(&vol_state);
	noise_gen_init(NOISE_TYPE_PINK);
	// uint8_t frame_count = 0;
	// float calibration_sum = 0.0f;
	int feed_chunksize = afe_get_chunksize();
	while (1) {

		afe_fetch_result_t *result = afe_handle->fetch(afe_data);

		if (result == NULL) {
			ESP_LOGE(TAG, "AFE fetch returned NULL");
		}

		if (result->ret_value == ESP_FAIL) {
			ESP_LOGE(TAG, "AFE fetch failed");
		}

		if (result != NULL && result->ret_value == ESP_OK) {
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

				noise_gen_fill(speaker_buffer, feed_chunksize);
				bool masking;
				uint8_t volume_pct = 0;

				if (AFE_STATE == AUDIO_AFE_VAD_SPEECH) {
					// Someone talking — normal volume from RMS
					volume_pct = volume_process_frame(
						&vol_state, microphone_buffer, speaker_buffer,
						feed_chunksize, &masking);
				} else {
					// Silence — force ramp to zero
					float level = volume_ramp(&vol_state, 0.0f);
					apply_volume(speaker_buffer, feed_chunksize, level);
					masking = false;
				}
				if (abs(volume_pct - last_volume) >= 5) {
					if (masking) {
						ESP_LOGI(TAG, "Masking active — volume %u%%",
								 volume_pct);
					} else {
						ESP_LOGI(TAG, "Masking inactive — volume %u%%",
								 volume_pct);
					}
					last_volume = volume_pct;
				}

				xQueueSend(audio_output_queue, speaker_buffer, portMAX_DELAY);
			}
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

bool is_afe_speech() { return AFE_STATE == AUDIO_AFE_VAD_SPEECH; }

int afe_get_chunksize() {
	if (afe_data == NULL || afe_handle == NULL) {
		ESP_LOGE(TAG,
				 "AFE not initialized, can't fetch chunksize will return 0");
		return 0;
	}
	return afe_handle->get_feed_chunksize(afe_data);
}

int afe_get_channels() {
	if (afe_data == NULL || afe_handle == NULL) {
		ESP_LOGE(TAG, "AFE not initialized, can't get feed channels");
		return 0;
	}
	return afe_handle->get_feed_channel_num(afe_data);
}


void audio_afe_destroy(void) {
	if (afe_handle != NULL && afe_data != NULL) {
		afe_handle->destroy(afe_data);
	}

	afe_data = NULL;
	afe_handle = NULL;
	ESP_LOGI(TAG, "AFE destroyed");
}
