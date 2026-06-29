#include "afe.h"
#include "volume.h"
#include "audio_hal.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "global_config.h"
#include "log_tags.h"
#include "mesh_core.h"
#include "sdkconfig.h"
#include "web_dashboard.h"
#include <stdio.h>

static const char *TAG = LOG_TAG_MAIN;

// Shared Queue handling raw audio chunks between Core 1 and Core 0
QueueHandle_t audio_input_queue = NULL;
QueueHandle_t audio_output_queue = NULL;

/* -------------------------------------------------------------------------- */
/* Log level setup — see Kconfig.projbuild for per-subsystem toggles         */
/* -------------------------------------------------------------------------- */

static void log_levels_init(void) {
#ifdef CONFIG_PRIVACY_SHIELD_BUILD_PRODUCTION
    /* Production: everything quiet */
    esp_log_level_set("*", ESP_LOG_ERROR);
    /* Main always at INFO */
    esp_log_level_set(LOG_TAG_MAIN, ESP_LOG_INFO);
    return;
#endif

	/* Set global default to INFO — clean base level */
	esp_log_level_set("*", ESP_LOG_INFO);

	/* Mesh subsystem */
#ifdef CONFIG_PRIVACY_SHIELD_LOG_MESH
	esp_log_level_set(LOG_TAG_MESH_CORE, ESP_LOG_DEBUG);
	esp_log_level_set(LOG_TAG_DISCOVERY, ESP_LOG_DEBUG);
#else
	esp_log_level_set(LOG_TAG_MESH_CORE, ESP_LOG_WARN);
	esp_log_level_set(LOG_TAG_DISCOVERY, ESP_LOG_WARN);
#endif

	/* Audio subsystem */
#ifdef CONFIG_PRIVACY_SHIELD_LOG_AUDIO
    esp_log_level_set(LOG_TAG_AUDIO_MIC, ESP_LOG_DEBUG);
    esp_log_level_set(LOG_TAG_AUDIO_AMP, ESP_LOG_DEBUG);
#else
    esp_log_level_set(LOG_TAG_AUDIO_MIC, ESP_LOG_WARN);
    esp_log_level_set(LOG_TAG_AUDIO_AMP, ESP_LOG_WARN);
#endif

#ifdef CONFIG_PRIVACY_SHIELD_LOG_AUDIO_AFE
    esp_log_level_set(LOG_TAG_AUDIO_AFE, ESP_LOG_DEBUG);
    esp_log_level_set(LOG_TAG_VAD, ESP_LOG_DEBUG);
    esp_log_level_set(LOG_TAG_NOISE_GEN, ESP_LOG_DEBUG);
    esp_log_level_set(LOG_TAG_AEC, ESP_LOG_DEBUG);
#else
    esp_log_level_set(LOG_TAG_AUDIO_AFE, ESP_LOG_WARN);
    esp_log_level_set(LOG_TAG_VAD, ESP_LOG_WARN);
    esp_log_level_set(LOG_TAG_NOISE_GEN, ESP_LOG_WARN);
    esp_log_level_set(LOG_TAG_AEC, ESP_LOG_WARN);
#endif

#ifdef CONFIG_PRIVACY_SHIELD_LOG_WEB
    esp_log_level_set(LOG_TAG_WEB, ESP_LOG_DEBUG);
#else
    esp_log_level_set(LOG_TAG_WEB, ESP_LOG_WARN);
#endif

	/* Main always at INFO */
	esp_log_level_set(LOG_TAG_MAIN, ESP_LOG_INFO);

	ESP_LOGI(TAG, "Log levels initialized");
}

//This will be removed once we substitute with correct methods
static uint8_t stub_100(void) { return 100; }
/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */
void app_main(void) {
	uart_set_baudrate(UART_NUM_0, 2000000);
	log_levels_init();
	uint8_t node_id = get_node_id();

#ifdef CONFIG_PRIVACY_SHIELD_ROLE_HUB
    /* ================================================================
     *  HUB MODE — Dashboard controller, no audio hardware
     * ================================================================ */
    ESP_LOGI(TAG, "+------------------------------------------+");
    ESP_LOGI(TAG, "|        PRIVACY SHIELD v1.0               |");
    ESP_LOGI(TAG, "|        Hub Controller  |   ESP32-S3       |");
    ESP_LOGI(TAG, "+------------------------------------------+");

    vTaskDelay(pdMS_TO_TICKS(200));

    /* ── Web Dashboard state ── */
    web_dashboard_init();

    /* ── Mesh (ESP-NOW) — receives STATUS from nodes ── */
    ESP_LOGI(TAG, "  [..] Initializing ESP-NOW Mesh...");
    ESP_ERROR_CHECK(mesh_init(WIFI_MODE_AP, web_dashboard_update_status, NULL));
    xTaskCreate(hello_task, "hello", 2048, NULL, 1, NULL);
    xTaskCreate(prune_task, "prune", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "  [OK] ESP-NOW Mesh ........ " MACSTR,
             MAC2STR(mesh_get_state()->my_mac));

    /* ── WiFi AP + Web Dashboard (Tasks 4.1–4.3) ── */
    ESP_LOGI(TAG, "  [..] Starting WiFi AP + Web Server...");
    ESP_ERROR_CHECK(wifi_ap_init());
    ESP_ERROR_CHECK(web_server_init());
    ESP_LOGI(TAG, "  [OK] Web Dashboard ....... http://192.168.4.1");

    ESP_LOGI(TAG, "+------------------------------------------+");
    ESP_LOGI(TAG, "|          HUB READY                        |");
    ESP_LOGI(TAG, "+------------------------------------------+");

#else
    /* ================================================================
     *  NODE MODE — Full audio pipeline + masking
     * ================================================================ */
    ESP_LOGI(TAG, "+------------------------------------------+");
    ESP_LOGI(TAG, "|        PRIVACY SHIELD v1.0               |");
    ESP_LOGI(TAG, "|        Node %u   |   ESP32-S3              |", node_id);
    ESP_LOGI(TAG, "+------------------------------------------+");

	vTaskDelay(pdMS_TO_TICKS(200));

    /* ── Mesh (ESP-NOW) — receives STATUS from nodes ── */
    ESP_LOGI(TAG, "  [..] Initializing ESP-NOW Mesh...");
	volume_command_cb *commands = malloc(sizeof(*commands));
	commands->set_volume = volume_set_command;
	commands->set_masking = mask_set_command;
	commands->unlock = volume_unlock;
    ESP_ERROR_CHECK(mesh_init(WIFI_MODE_STA,NULL, commands));
    
    xTaskCreate(hello_task, "hello", 2048, NULL, 1, NULL);
    xTaskCreate(prune_task, "prune", 4096, NULL, 1, NULL);

	status_task_params_t *params = malloc(sizeof(*params));
	params->node_id     = node_id;
	params->is_speech   = is_afe_speech;
	params->get_volume  = get_volume;
	params->get_battery = stub_100;
	xTaskCreate(status_task, "status", 4096, params, 1, NULL);
    ESP_LOGI(TAG, "  [OK] ESP-NOW Mesh ........ node %u, " MACSTR,
             node_id, MAC2STR(mesh_get_state()->my_mac));

	/* ── Audio ───────────────────────────────────────────────── */
	audio_input_queue = xQueueCreate(1, AFE_FEED_SAMPLES * sizeof(int16_t));
	if (audio_input_queue == NULL) {
		ESP_LOGE(TAG, "  [!!] Audio queue creation failed!");
		return;
	}
	audio_output_queue = xQueueCreate(2, AFE_FEED_SAMPLES * sizeof(int16_t));
	if (audio_output_queue == NULL) {
		ESP_LOGE(TAG, "  [!!] Noise queue creation failed!");
		return;
	}

	/* ── Micriophone ──────────────────────────────────────────────── */
	ESP_LOGI(TAG, "  [..] Initializing I2S Microphone...");
	esp_err_t mic_init = audio_hal_mic_init();
	if (mic_init != ESP_OK) {
		ESP_LOGE(TAG, "  [!!] Microphone initialization failed!");
		return;
	}
	ESP_LOGI(TAG, "  [OK] I2S Microphone ...... 16 kHz, 32-bit, Mono");

	/* ── Amplifier ──────────────────────────────────────────────── */
	ESP_LOGI(TAG, "  [..] Initializing I2S Amplifier...");
	audio_hal_speaker_init(); // We should decide a standard: Function do o do
							  // not return esp_err_t? For now, it just logs and
							  // continues.
	ESP_LOGI(TAG, "  [OK] I2S Amplifier ...... 16 kHz, 32-bit, Mono");

	/* ── AFE ────────────────────────────────────────────────── */
	ESP_LOGI(TAG, "  [..] Initializing AFE Pipeline...");
	esp_err_t afe_err = audio_afe_init("MR");
	if (afe_err != ESP_OK) {
		ESP_LOGE(TAG, "  [!!] AFE Initialization failed!");
		return;
	}
	ESP_LOGI(TAG,
			 "  [OK] AFE Pipeline ........ VADNet1 Medium, %d samples/chunk",
			 AFE_FEED_SAMPLES);
	xTaskCreatePinnedToCore(audio_hal_mic_read_task, "Mic_Read", 4096, NULL, 5,
							NULL, 1);

	vTaskDelay(pdMS_TO_TICKS(1));

#if defined(CONFIG_PRIVACY_SHIELD_BUILD_DEBUG) &&                              \
	defined(CONFIG_PRIVACY_SHIELD_LOG_AUDIO)
	// Launch FreeRTOS tasks
	xTaskCreate(audio_hal_speaker_task, "SPEAKER_TASK", 4096, NULL, 5, NULL);
#endif

	xTaskCreatePinnedToCore(&audio_afe_feed, "AFE_TASK", 4096, NULL, 5, NULL,
							0);
	xTaskCreatePinnedToCore(audio_afe_fetch, "AFE_FETCH_TASK", 4096, NULL, 5,
							NULL, 1);
	ESP_LOGI(TAG, "  [OK] Tasks spawned ....... Mic_Read (Core 1, Pri 5)");
	ESP_LOGI(TAG, "                          . AFE_Proc (Core 0, Pri 5)");
	ESP_LOGI(TAG, "                          . hello + prune (Core 0, Pri 1)");

	/* ── Footer ─────────────────────────────────────────────── */
	ESP_LOGI(TAG, "+------------------------------------------+");
	ESP_LOGI(TAG, "|      SYSTEM READY! Running v0.301        |");
	ESP_LOGI(TAG, "+------------------------------------------+");
#endif
}
