#pragma once

#include "audio_hal.h"
#include "esp_err.h"
#include "volume.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	AUDIO_AFE_VAD_SILENCE = 0,
	AUDIO_AFE_VAD_SPEECH,
	AUDIO_AFE_VAD_UNKNOWN,
} audio_afe_vad_state_t;

typedef struct {
	int16_t *data;
	int samples;
	int channels;
	audio_afe_vad_state_t vad_state;
} audio_afe_result_t;

typedef struct {
	double avg_attack;
	double avg_release;
	int16_t max_attack;
	int16_t max_release;
	int16_t min_attack;
	int16_t min_release;
} afe_data_points_t;

/**
 * Initialize ESP-SR Audio Front End.
 *
 * input_format examples:
 *   "M"    = 1 mic, no AEC reference
 *   "MM"   = 2 mics, no AEC reference
 *   "MR"   = 1 mic + 1 playback reference
 *   "MMR"  = 2 mics + 1 playback reference
 *
 * For AEC, you need an R channel.
 */
esp_err_t audio_afe_init(const char *input_format);

/**
 * Returns the lates state of the AFE through either
 * Speech recognized, Silence or Unknown
 * */

bool is_afe_speech(void);

int64_t get_afe_delay(void);

int afe_feed_chunksize(void);

int afe_feed_channels(void);

int afe_fetch_chunksize(void);

int afe_fetch_channels(void);

void afe_calibration(void);

/**
 * Feed one frame of raw int16 PCM into the AFE.
 *
 * The buffer must contain:
 *   feed_chunksize * feed_channels samples
 *
 * For "MR", that means interleaved:
 *   mic0, ref0, mic1, ref1, ...
 */
void audio_afe_feed(void *pvParameters);

/**
 * Fetch one processed frame from AFE.
 *
 * The returned audio pointer is owned by ESP-SR.
 * Do not free it.
 */
void audio_afe_fetch(void *pvParameters);

static void send_to_speaker(audio_packet_t *packet);

/**
 * Audio Front End Destructor essentially
 * */
void audio_afe_destroy(void);

/**
 * @breif
 * ZNCC - Zero-Mean Normalized Cross-Correlation
 * compares reference frame wih the mic frame
 * returns a float
 * 1.0 -> very strong match
 * 0.0 -> no useful match
 * -1.0 -> inverted but strong match
 */
static float audio_zncc(const int16_t *mic, const int16_t *ref,
						size_t sample_count);

static int find_best_reference_frame(const int16_t *mic_frame,
									 size_t sample_size);

uint8_t afe_get_volume(void);

static void update_attack(int64_t timestamp);

static void update_release(int64_t timestamp);

bool has_valid_reference(void);

afe_data_points_t get_afe_delays(void);

#ifdef __cplusplus
}
#endif
