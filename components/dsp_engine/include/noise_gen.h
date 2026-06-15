#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOISE_TYPE_PINK = 0,   /* -3dB/octave  — waterfall, good all-round masker */
    NOISE_TYPE_BROWN,      /* -6dB/octave  — deep rumble, masks low frequencies */
} noise_type_t;

/**
 * @brief Initialize the noise generator.
 *
 * Pre-computes filter coefficients. Call once at boot.
 *
 * @param type   NOISE_TYPE_PINK or NOISE_TYPE_BROWN
 */
void noise_gen_init(noise_type_t type);

/**
 * @brief Fill a buffer with noise samples.
 *
 * Fills buffer[0..count-1] with int16_t noise.
 * Call once per audio frame (32ms at 16kHz → 512 samples).
 *
 *
 * @param buffer  destination buffer (modified in place)
 * @param count   number of samples to generate
 */
void noise_gen_fill(int16_t *buffer, int count);

#ifdef __cplusplus
}
#endif
