#include "afe.h"
#include "audio_hal.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "global_config.h"
#include "log_tags.h"
#include "mesh_core.h"
#include "sdkconfig.h"
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

/* -------------------------------------------------------------------------- */
/* Hello task — broadcast our presence every 10 seconds                      */
/* -------------------------------------------------------------------------- */

static void hello_task(void *arg) {
	TickType_t last_wake = xTaskGetTickCount();
	while (1) {
		mesh_send_hello();
		vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10000));
	}
}

/* -------------------------------------------------------------------------- */
/* Prune task — clean up timed-out neighbors every 10 seconds                 */
/* -------------------------------------------------------------------------- */

static void prune_task(void *arg) {
	while (1) {
		mesh_discovery_prune();
		int count = mesh_discovery_count();
		if (count > 0) {
			ESP_LOGI(LOG_TAG_MESH_CORE, "%d neighbor(s) online", count);
		}
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

/* -------------------------------------------------------------------------- */
/* Status task — Broadcast status of the node, like masking, volume etc       */
/* -------------------------------------------------------------------------- */

static void status_task(void *arg) {
	TickType_t last_wake = xTaskGetTickCount();
	while (1) {
		mesh_status_pkt_t status = {0};
		status.header.type = MESH_PKT_STATUS;
		status.header.src_id = DEFAULT_NODE_ID;
		status.header.timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount());
		status.masking_active = is_afe_speech() /* read from VAD state */;
		status.volume = 100 /* read from current volume */;
		status.battery_pct = 85; // placeholder, real sensor later
		status.uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

		mesh_broadcast(&status, sizeof(status));
		vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5000));
	}
}

/* -------------------------------------------------------------------------- */
/* Packet received callback — handle incoming mesh packets                   */
/* -------------------------------------------------------------------------- */

static void on_mesh_packet(const uint8_t *src_mac, const void *data,
						   size_t len) {
	const mesh_header_t *hdr = (const mesh_header_t *)data;

	switch (hdr->type) {
	case MESH_PKT_HELLO:
		ESP_LOGI(LOG_TAG_DISCOVERY, "HELLO from node %u (" MACSTR ")",
				 hdr->src_id, MAC2STR(src_mac));
		break;

	case MESH_PKT_STATUS:
		if (len >= sizeof(mesh_status_pkt_t)) {
			const mesh_status_pkt_t *status = (const mesh_status_pkt_t *)data;
			ESP_LOGI(LOG_TAG_DISCOVERY,
					 "STATUS from node %u: masking=%s vol=%u batt=%u%%",
					 status->header.src_id,
					 status->masking_active ? "ON" : "OFF", status->volume,
					 status->battery_pct);
		}
		break;

	case MESH_PKT_COMMAND:
		if (len >= sizeof(mesh_command_pkt_t)) {
			const mesh_command_pkt_t *cmd = (const mesh_command_pkt_t *)data;
			ESP_LOGI(LOG_TAG_DISCOVERY, "COMMAND from node %u: cmd=%u val=%u",
					 cmd->header.src_id, cmd->command, cmd->value);
			/* Future: act on mute/unmute/volume commands here */
		}
		break;

	default:
		ESP_LOGD(LOG_TAG_DISCOVERY, "Unknown packet type 0x%02X from node %u",
				 hdr->type, hdr->src_id);
		break;
	}
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */
void app_main(void) {
	uart_set_baudrate(UART_NUM_0, 2000000);
	log_levels_init();

	/* ── Header ──────────────────────────────────────────────── */
	ESP_LOGI(TAG, "+------------------------------------------+");
	ESP_LOGI(TAG, "|        PRIVACY SHIELD v0.301             |");
	ESP_LOGI(TAG, "|        Node %u   |   ESP32-S3            |",
			 DEFAULT_NODE_ID);
	ESP_LOGI(TAG, "+------------------------------------------+");

	vTaskDelay(pdMS_TO_TICKS(200));

	/* ── Mesh ───────────────────────────────────────────────── */
	ESP_LOGI(TAG, "  [..] Initializing ESP-NOW Mesh...");
	ESP_ERROR_CHECK(mesh_init(DEFAULT_NODE_ID));
	mesh_register_recv_callback(on_mesh_packet);
	xTaskCreate(hello_task, "hello", 2048, NULL, 1, NULL);
	xTaskCreate(prune_task, "prune", 4096, NULL, 1, NULL);
	xTaskCreate(status_task, "status", 4096, NULL, 1, NULL);
	ESP_LOGI(TAG, "  [OK] ESP-NOW Mesh ........ node %u, " MACSTR,
			 DEFAULT_NODE_ID, MAC2STR(mesh_get_state()->my_mac));

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
							  // continues
	/* ── AFE ────────────────────────────────────────────────── */
	ESP_LOGI(TAG, "  [..] Initializing AFE Pipeline...");
	esp_err_t afe_err = audio_afe_init("MR");
	if (afe_err != ESP_OK) {
		ESP_LOGE(TAG, "  [!!] AFE Initialization failed!");
		return;
	}

	int feed_chunksize = afe_get_chunksize();
	ESP_LOGI(TAG,
			 "  [OK] AFE Pipeline ........ VADNet1 Medium, %d samples/chunk",
			 feed_chunksize);

	/* ── Audio Queues ───────────────────────────────────────────────── */
	audio_input_queue = xQueueCreate(1, feed_chunksize * sizeof(int16_t));
	if (audio_input_queue == NULL) {
		ESP_LOGE(TAG, "  [!!] Audio queue creation failed!");
		return;
	}
	audio_output_queue = xQueueCreate(2, feed_chunksize * sizeof(int16_t));
	if (audio_output_queue == NULL) {
		ESP_LOGE(TAG, "  [!!] Noise queue creation failed!");
		return;
	}
	ESP_LOGI(TAG, "  [OK] I2S Amplifier ...... 16 kHz, 32-bit, Mono");
	xTaskCreatePinnedToCore(audio_hal_mic_read_task, "Mic_Read", 4096, NULL, 5,
							NULL, 1);

	vTaskDelay(pdMS_TO_TICKS(1000));
	// Launch FreeRTOS tasks
	xTaskCreate(audio_hal_speaker_task, "SPEAKER_TASK", 4096, NULL, 5, NULL);

	xTaskCreatePinnedToCore(&audio_afe_feed, "AFE_FEED_TASK", 4096, NULL, 5,
							NULL, 0);
	xTaskCreatePinnedToCore(audio_afe_fetch, "AFE_FETCH_TASK", 4096, NULL, 5,
							NULL, 1);
	ESP_LOGI(TAG, "  [OK] Tasks spawned ....... Mic_Read (Core 1, Pri 5)");
	ESP_LOGI(TAG, "                          . AFE_Proc (Core 0, Pri 5)");
	ESP_LOGI(TAG, "                          . hello + prune (Core 0, Pri 1)");

	/* ── Footer ─────────────────────────────────────────────── */
	ESP_LOGI(TAG, "+------------------------------------------+");
	ESP_LOGI(TAG, "|      SYSTEM READY! Running v0.301        |");
	ESP_LOGI(TAG, "+------------------------------------------+");
}
