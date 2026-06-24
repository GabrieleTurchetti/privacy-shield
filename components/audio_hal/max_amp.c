#include "afe.h"
#include "audio_hal.h" // For function signatures
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_config.h" // For pin configurations
#include "log_tags.h"
#include <math.h>
#include <stdio.h>

static const char *TAG = LOG_TAG_AUDIO_AMP;
static i2s_chan_handle_t tx_chan;
extern QueueHandle_t audio_output_queue;

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

void audio_hal_speaker_task(void *pvParameters) {
	int feed_chunksize = afe_get_chunksize();
	int16_t buffer[feed_chunksize];
	size_t bytes_written;

	while (1) {
		xQueueReceive(audio_output_queue, buffer, portMAX_DELAY);
		// buffer is already scaled — volume was applied by the AFE task

		i2s_channel_write(tx_chan, buffer, feed_chunksize * sizeof(int16_t),
						  &bytes_written, portMAX_DELAY);
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
