#include "afe.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "global_config.h"
#include "model_path.h"
#include "log_tags.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = LOG_TAG_AUDIO_AFE;

static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
extern QueueHandle_t audio_ai_queue;

static int feed_chunksize = 0;
static int feed_channels = 0;
static int fetch_chunksize = 0;
static int fetch_channels = 0;
static audio_afe_vad_state_t AFE_STATE;

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
    afe_config_t *afe_config = afe_config_init(input_format, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);

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
    afe_config->vad_min_speech_ms = 64;
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
    bool has_reference_channel = strchr(input_format, 'R') != NULL;

    if (has_reference_channel) {
        afe_config->aec_init = true;
    } else {
        afe_config->aec_init = false;
        ESP_LOGW(TAG, "AEC disabled because input_format has no R reference channel");
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

    feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    feed_channels = afe_handle->get_feed_channel_num(afe_data);
    fetch_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    fetch_channels = afe_handle->get_fetch_channel_num(afe_data);

    ESP_LOGI(TAG, "AFE initialized");
    ESP_LOGI(TAG, "input_format=%s", input_format);
    ESP_LOGI(TAG, "feed_chunksize=%d, feed_channels=%d", feed_chunksize, feed_channels);
    ESP_LOGI(TAG, "fetch_chunksize=%d, fetch_channels=%d", fetch_chunksize, fetch_channels);
    ESP_LOGI(TAG, "VAD enabled");
    ESP_LOGI(TAG, "AEC %s", has_reference_channel ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t audio_afe_feed(const int16_t *pcm) {
    if (afe_handle == NULL || afe_data == NULL) {
        ESP_LOGE(TAG, "AFE not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pcm == NULL) {
        ESP_LOGE(TAG, "pcm is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    int ret = afe_handle->feed(afe_data, pcm);

    if (ret < 0) {
        ESP_LOGE(TAG, "AFE feed returned: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t audio_afe_fetch(audio_afe_result_t *out_result) {
    if (afe_handle == NULL || afe_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    afe_fetch_result_t *result = afe_handle->fetch(afe_data);

    if (result == NULL) {
        ESP_LOGE(TAG, "AFE fetch returned NULL");
        return ESP_FAIL;
    }

    if (result->ret_value == ESP_FAIL) {
        ESP_LOGE(TAG, "AFE fetch failed");
        return ESP_FAIL;
    }

    out_result->data = result->data;
    out_result->samples = result->data_size / sizeof(int16_t);
    out_result->channels = fetch_channels;
    out_result->vad_state = convert_vad_state(result->vad_state);

    return ESP_OK;
}

audio_afe_vad_state_t get_afe_state() { 
    return AFE_STATE; 
}

void audio_afe_destroy(void) {
    if (afe_handle != NULL && afe_data != NULL) {
        afe_handle->destroy(afe_data);
    }

    afe_data = NULL;
    afe_handle = NULL;

    feed_chunksize = 0;
    feed_channels = 0;
    fetch_chunksize = 0;
    fetch_channels = 0;

    ESP_LOGI(TAG, "AFE destroyed");
}

/* -------------------------------------------------------------------------- */
/* AFE Processing Task — Pulls from queue, processes VAD / NS / AEC           */
/* -------------------------------------------------------------------------- */
void afe_processing_task(void *pvParameters) {

    int16_t mic_frame[AFE_FEED_SAMPLES];
    int feed_bytes = 0;

    // Track VAD state change to avoid spamming the log console
    AFE_STATE = AUDIO_AFE_VAD_SILENCE;

    ESP_LOGI(TAG, "AFE processing task started on Core %d", xPortGetCoreID());

    while (1) {

        // Wait indefinitely until a raw mic chunk arrives from Core 1
        if (xQueueReceive(audio_ai_queue, mic_frame, portMAX_DELAY) == pdTRUE) {

            feed_bytes += AFE_FEED_SAMPLES;
            
            // 1. Feed the 16-bit PCM block into the ESP Front End Engine
            esp_err_t err = audio_afe_feed(mic_frame);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to feed AFE: %s", esp_err_to_name(err));
                continue;
            }

            if (feed_bytes > fetch_chunksize) {

                audio_afe_result_t result;
                err = audio_afe_fetch(&result);
                feed_bytes -= fetch_chunksize;

                if (err != ESP_OK) {
                    // Not fatal. AFE may not have enough output ready yet.
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }

                if (result.vad_state != AFE_STATE) {
                    if (result.vad_state == AUDIO_AFE_VAD_SPEECH) {
                        ESP_LOGI(TAG, "[VAD] Speech detected!");
                    } else if (result.vad_state == AUDIO_AFE_VAD_SILENCE) {
                        ESP_LOGI(TAG, "[VAD] Silence...");
                    } else {
                        ESP_LOGW(TAG, "[VAD] Unknown VAD state.");
                    }

                    AFE_STATE = result.vad_state;
                }
            }
        }
    }
}