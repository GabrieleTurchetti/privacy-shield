#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Volume State                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    float current;      /* Smoothed volume level (0.0 = mute, 1.0 = full) */
    float attack_coeff; /* Ramp-up speed   (e.g. 0.02 = 50ms attack)      */
    float release_coeff;/* Ramp-down speed (e.g. 0.002 = 500ms release)   */
    float noise_floor;  /* RMS below this = silence (calibrate at boot)    */
} volume_state_t;


#define DEFAULT_NOISE_FLOOR 500.0f  /* Initial guess for noise floor RMS */
#define MAX_RMS 2500.0f              /* RMS that maps to full volume (1.0) - Basically it's sensibility*/

/* -------------------------------------------------------------------------- */
/*  API                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize a volume state with default attack/release.
 *
 * Default: 10ms attack, 100ms release, noise_floor=500.
 * Call once at boot.
 */
void volume_init(volume_state_t *vol);

/**
 * @brief Calibrate the noise floor from ambient measurements.
 *
 * Call after 1 second of silence capture at boot.
 * Pass the average RMS of the silent period.
 *
 * @param vol           volume state to calibrate
 * @param measured_rms  average RMS during silence
 */
void volume_calibrate(volume_state_t *vol, float measured_rms);

/**
 * @brief Compute RMS (root mean square) of an audio buffer.
 *
 * @param samples  int16_t audio samples
 * @param count    number of samples
 * @return RMS value (0 to 32767)
 */
float compute_rms(const int16_t *samples, int count);

/**
 * @brief Map an RMS audio level to a target volume (0.0–1.0).
 *
 *
 * Linear mapping: noise_floor..5000 → 0.0..1.0
 *
 * Adjust max_rms to match your mic + distance:
 *   5000 = sensitive (whisper hits 30%, normal 60%, loud 100%)
 *   3000 = less sensitive (only loud speech triggers high volume)
 *
 *
 * @param rms          RMS value from compute_rms()
 * @param noise_floor  RMS threshold below which output is muted
 * @return target volume 0.0 to 1.0
 */
float rms_to_volume(float rms, float noise_floor);

/**
 * @brief Smoothly ramp volume toward a target value.
 *
 * Uses asymmetric smoothing: fast attack, slow release.
 * This prevents sudden jumps while ensuring speech starts are not clipped.
 *
 * @param vol     volume state (modified in place)
 * @param target  desired volume (0.0–1.0)
 * @return new smoothed volume
 */
float volume_ramp(volume_state_t *vol, float target);

/**
 * @brief Apply volume scaling to an int16_t audio buffer.
 *
 * Multiplies every sample by the given level.
 *
 * @param buffer   int16_t samples (modified in place)
 * @param count    number of samples
 * @param level    volume multiplier (0.0 = mute, 1.0 = pass-through)
 */
void apply_volume(int16_t *buffer, int count, float level);

/**
 * @brief Convenience: full pipeline for one audio frame.
 *
 * 1. Compute RMS of mic input
 * 2. Map to target volume
 * 3. Smoothly ramp volume state
 * 4. Apply to noise output buffer
 *
 * @param vol         volume state (persistent across frames)
 * @param mic_in      raw microphone samples
 * @param noise_out   noise buffer to scale (modified in place)
 * @param count       samples in each buffer
 * @param masking_active  [OUT] set to true if speech is detected
 * @return uint8_t    current volume 0–100 (for STATUS packet)
 */
uint8_t volume_process_frame(volume_state_t *vol,
                             const int16_t *mic_in, int16_t *noise_out,
                             int count, bool *masking_active);

#ifdef __cplusplus
}
#endif
