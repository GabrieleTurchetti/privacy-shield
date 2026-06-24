#pragma once

#include "esp_err.h"
#include <stdbool.h>
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

int get_feed_chunksize(void);

int get_fetch_chunksize(void);

int get_feed_channels(void);

int get_fetch_channels(void);

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

/**
 * Audio Front End Destructor essentially
 * */
void audio_afe_destroy(void);

#ifdef __cplusplus
}
#endif
