#include "afe.h"
#include "audio_hal.h" // For function signatures
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "global_config.h" // For pin configurations
#include "log_tags.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static const char *TAG = LOG_TAG_AUDIO_AMP;
static i2s_chan_handle_t tx_chan;
extern QueueHandle_t audio_output_queue;
static uint16_t min_delay = 9999, max_delay = 0, counter = 0, avg_delay[1000];

void audio_hal_speaker_init(void) {
	ESP_LOGI(TAG, "Initializing I2S TX for MAX98357A...");

	// 1. Configure the TX channel (Transmission to the amplifier)
	i2s_chan_config_t tx_chan_cfg =
		I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

	// 2. Configure the standard (16000 Hz, 16-bit, Mono)
	i2s_std_config_t tx_std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
			I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
		.gpio_cfg =
			{
				.mclk = I2S_GPIO_UNUSED,
				.bclk = PIN_AMP_BCLK,
				.ws = PIN_AMP_LRCLK,
				.dout = PIN_AMP_DOUT,
				.din = I2S_GPIO_UNUSED,
				.invert_flags =
					{
						.mclk_inv = false,
						.bclk_inv = false,
						.ws_inv = false,
					},
			},
	};

	// 3. Apply the configuration and enable the amplifier
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
	ESP_LOGI(TAG, "Amplifier ready to use.");
}

static void update_delays(int16_t timestamp) {

	avg_delay[counter] = timestamp;

	if (avg_delay[counter] > max_delay) {
		max_delay = avg_delay[counter];
	}
	if (avg_delay[counter] < min_delay) {
		min_delay = avg_delay[counter];
	}
	counter++;
	if (counter >= 1000) {
		counter = 0;
	}
}

double get_avg_delay() {
	double sum = 0.0;
	int counter = 0;
	for (int i = 0; i < 1000; i++) {
		if (avg_delay[i] != 0) {
			sum += (double)avg_delay[i];
			counter++;
		}
	}
	return (sum / counter);
}

double get_max_delay() { return (double)max_delay; }

double get_min_delay() { return (double)min_delay; }

void audio_hal_speaker_task(void *pvParameters) {

	size_t bytes_written = 0;
	audio_packet_t *packet = NULL;

	while (1) {
		if (xQueueReceive(audio_output_queue, &packet, portMAX_DELAY) !=
			pdTRUE) {
			continue;
		}

		if (packet == NULL) {
			free_audio_packet(packet);
			packet = NULL;
			continue;
		}

		int16_t timestamp =
			(int16_t)((esp_timer_get_time() - packet->timestamp) / 1000);
		update_delays(timestamp);
		esp_err_t err = i2s_channel_write(tx_chan, packet->audio_sample,
										  packet->sample_size *
											  sizeof(packet->audio_sample[0]),
										  &bytes_written, portMAX_DELAY);

		if (bytes_written == 0 || err != ESP_OK) {
			ESP_LOGW(TAG, "Failed to wrtie to AMP/Speaker");
		}

		free_audio_packet(packet);
		packet = NULL;
	}
}

// TODO: Remove this
/*void sine_wave_task(void *pvParameters) {
	const int SINE_FREQ_HZ = 1000;
	const int AMPLITUDE = 15000; // Max amplitude for 16-bit is 32767

	// Calculate the exact length of 1 wave: 16000Hz / 1000Hz = 16 samples
	int samples_per_wave = SPK_SAMPLE_RATE / SINE_FREQ_HZ;

	// Allocate memory for the wave buffer
	int16_t *sine_buffer = (int16_t *)calloc(samples_per_wave, sizeof(int16_t));

	// Pre-calculate the sine wave mathematically (saves CPU cycles)
	for (int i = 0; i < samples_per_wave; i++) {
		double time = (double)i / (double)SPK_SAMPLE_RATE;
		// Formula: y(t) = A * sin(2 * PI * f * t)
		sine_buffer[i] =
			(int16_t)(AMPLITUDE * sin(2.0 * M_PI * SINE_FREQ_HZ * time));
	}

	ESP_LOGI(TAG, "Starting 1kHz Tone Test. Place the DAEX25 on surfaces!");

	size_t bytes_written = 0;

	while (1) {
		// Infinitely stream the pre-calculated wave to the I2S amplifier
		i2s_channel_write(tx_chan, sine_buffer,
						  samples_per_wave * sizeof(int16_t), &bytes_written,
						  portMAX_DELAY);
	}
}*/
