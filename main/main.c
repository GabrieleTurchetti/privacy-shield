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
#include "battery.h"
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

#ifdef CONFIG_PRIVACY_SHIELD_LOG_BATTERY
    esp_log_level_set(LOG_TAG_BATTERY, ESP_LOG_DEBUG);
#else
    esp_log_level_set(LOG_TAG_BATTERY, ESP_LOG_WARN);
#endif

	/* Main always at INFO */
	esp_log_level_set(LOG_TAG_MAIN, ESP_LOG_INFO);

	ESP_LOGI(TAG, "Log levels initialized");
}

/* -------------------------------------------------------------------------- */
/* System Metrics Tracking                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t prev_total_run_time = 0;
static uint32_t prev_idle0_time = 0;
static uint32_t prev_idle1_time = 0;
static uint8_t cpu0_utilization = 0;
static uint8_t cpu1_utilization = 0;

// Call this function periodically
void update_system_metrics(void) {
    TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize;
    uint32_t ulTotalRunTime;

    uxArraySize = uxTaskGetNumberOfTasks();
    pxTaskStatusArray = pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));

    if (pxTaskStatusArray != NULL) {
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);

        uint32_t idle0_time = 0;
        uint32_t idle1_time = 0;

        // Find the run times of the idle tasks for both cores
        for (UBaseType_t x = 0; x < uxArraySize; x++) {
            if (strncmp(pxTaskStatusArray[x].pcTaskName, "IDLE0", 5) == 0) {
                idle0_time = pxTaskStatusArray[x].ulRunTimeCounter;
            } else if (strncmp(pxTaskStatusArray[x].pcTaskName, "IDLE1", 5) == 0) {
                idle1_time = pxTaskStatusArray[x].ulRunTimeCounter;
            }
        }
        vPortFree(pxTaskStatusArray);

        uint32_t total_delta = ulTotalRunTime - prev_total_run_time;
        if (total_delta > 0 && prev_total_run_time > 0) {
            uint32_t idle0_delta = idle0_time - prev_idle0_time;
            uint32_t idle1_delta = idle1_time - prev_idle1_time;

            // In SMP FreeRTOS, total run time encompasses both cores.
            // We scale up by 2 to get the percentage per core (since total_delta is 2x real time)
            uint32_t idle0_pct = (idle0_delta * 100 * 2) / total_delta;
            uint32_t idle1_pct = (idle1_delta * 100 * 2) / total_delta;
            
            // Utilization is the inverse of the idle percentage
            cpu0_utilization = 100 - (idle0_pct > 100 ? 100 : idle0_pct);
            cpu1_utilization = 100 - (idle1_pct > 100 ? 100 : idle1_pct);
        }

        prev_total_run_time = ulTotalRunTime;
        prev_idle0_time = idle0_time;
        prev_idle1_time = idle1_time;
    }
}

// Getters to be passed to the STATUS packet compiler
uint8_t get_cpu0_utilization(void) { return cpu0_utilization; }
uint8_t get_cpu1_utilization(void) { return cpu1_utilization; }
uint32_t get_heap_free(void) { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
uint32_t get_heap_largest_block(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

//This will be removed once we substitute with correct methods
static uint8_t stub_100(void) { return 100; }
/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */
void app_main(void) {
	uart_set_baudrate(UART_NUM_0, 2000000);
	log_levels_init();
	battery_init();
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
    xTaskCreate(hello_task, "hello", 4096, NULL, 1, NULL);
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

	vTaskDelay(pdMS_TO_TICKS(1));

    /* ── Mesh (ESP-NOW) — receives STATUS from nodes ── */
    ESP_LOGI(TAG, "  [..] Initializing ESP-NOW Mesh...");
	volume_command_cb *commands = malloc(sizeof(*commands));
	if(commands == NULL){
		ESP_LOGE(TAG, "Failed to allocate memory for volume_command_cb");
		return;
	}
	commands->set_volume = volume_set_command;
	commands->set_masking = mask_set_command;
	commands->unlock = volume_unlock;
	commands->set_volume_percentage = set_volume_percentage;
    ESP_ERROR_CHECK(mesh_init(WIFI_MODE_STA,NULL, commands));
    
    xTaskCreate(hello_task, "hello", 4096, NULL, 1, NULL);
    xTaskCreate(prune_task, "prune", 4096, NULL, 1, NULL);

	status_task_params_t *params = malloc(sizeof(*params));
	if(params == NULL){
		ESP_LOGE(TAG, "Failed to allocate memory for status_task_params_t");
		return;
	}
	params->node_id     = node_id;
	params->is_speech   = is_afe_speech;
	params->get_volume  = get_volume;
	params->get_volume_percentage = get_volume_percentage;
	params->get_battery = stub_100;
	params->update_system_metrics        = update_system_metrics;
    params->get_cpu0_utilization              = get_cpu0_utilization;
    params->get_cpu1_utilization              = get_cpu1_utilization;
    params->get_heap_free         = get_heap_free;
    params->get_heap_largest_block    = get_heap_largest_block;
	xTaskCreate(status_task, "status", 4096, params, 1, NULL);
	ESP_LOGI(TAG, "  [OK] ESP-NOW Mesh ........ node %u, " MACSTR,
             node_id, MAC2STR(mesh_get_state()->my_mac));

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
	audio_hal_speaker_init();
	ESP_LOGI(TAG, "  [OK] I2S Amplifier ...... 16 kHz, 32-bit, Mono");

	/* ── AFE ────────────────────────────────────────────────── */
	ESP_LOGI(TAG, "  [..] Initializing AFE Pipeline...");
	esp_err_t afe_err = audio_afe_init("MR");
	if (afe_err != ESP_OK) {
		ESP_LOGE(TAG, "  [!!] AFE Initialization failed!");
		return;
	}

	int feed_chunksize = afe_feed_chunksize();
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
	xTaskCreatePinnedToCore(audio_hal_mic_read_task, "Mic_Read", 4096, NULL, 5,
							NULL, 1);

	vTaskDelay(pdMS_TO_TICKS(1000));

	/* ── Battery ──────────────────────────────────────────────── */
	if (battery_get_status() == BATT_DISCONNECTED) {
		ESP_LOGW(TAG, "  [!!] Battery is disconnected! Please connect a battery.");
	} else {
		ESP_LOGI(TAG, "  [..] Initializing Battery Logger...");
		xTaskCreatePinnedToCore(battery_logger_task, "battery_logger_task", 4096, NULL, 5, NULL, 1);
		vTaskDelay(pdMS_TO_TICKS(10));
	}

	xTaskCreate(audio_hal_speaker_task, "SPEAKER_TASK", 4096, NULL, 5, NULL);
	xTaskCreatePinnedToCore(&audio_afe_feed, "AFE_TASK", 4096, NULL, 5, NULL,
							0);
	xTaskCreatePinnedToCore(audio_afe_fetch, "AFE_FETCH_TASK", 4096, NULL, 5,
							NULL, 1);
	ESP_LOGI(TAG, "  [OK] Tasks spawned ....... Mic_Read (Core 1, Pri 5)");
	ESP_LOGI(TAG, "                          . AFE_Proc (Core 0, Pri 5)");
	ESP_LOGI(TAG, "                          . hello + prune (Core 0, Pri 1)");

	/* ── Footer ─────────────────────────────────────────────── */
	ESP_LOGI(TAG, "+------------------------------------------+");
	ESP_LOGI(TAG, "|      SYSTEM READY! Running v0.305        |");
	ESP_LOGI(TAG, "+------------------------------------------+");
#endif
}
