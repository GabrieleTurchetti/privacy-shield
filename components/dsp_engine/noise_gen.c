#include "noise_gen.h"
#include "esp_random.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */

static noise_type_t s_type = NOISE_TYPE_PINK;
static bool s_initialized = false;

/* -------------------------------------------------------------------------- */
/*  Pink noise — Voss-McCartney algorithm                                     */
/* -------------------------------------------------------------------------- */

#define PINK_OCTAVES  7            /* Number of noise sources to mix */
#define PINK_AMP      8000         /* Output amplitude (max 32767)   */
#define PINK_AMP_MAX  32767

static int16_t pink_sample(void) {
    /*
     * Voss-McCartney algorithm:
     *   Maintain N independent white noise sources.
     *   On each sample, replace one source and sum all.
     *
     * Result: -3 dB/octave roll-off (pink noise).
     */

    static int16_t sources[PINK_OCTAVES];
    static uint32_t counter = 0;

    /* Every Nth sample, update the Nth source.
     * The bit-reversal pattern ensures each source updates
     * at half the rate of the previous one. */
    uint32_t change_mask = counter ^ (counter + 1);
    for (int i = 0; i < PINK_OCTAVES; i++) {
        if (change_mask & (1U << i)) {
            sources[i] = (int16_t)(((int32_t)(esp_random() & 0x1FFFF) - 0x10000) / PINK_OCTAVES);
        }
    }
    counter++;

    /* Sum all sources */
    int32_t sum = 0;
    for (int i = 0; i < PINK_OCTAVES; i++) {
        sum += sources[i];
    }

    /* Scale to PINK_AMP and clamp */
    sum = (sum * PINK_AMP) / PINK_AMP_MAX;
    if (sum > 32767) sum = 32767;
    if (sum < -32768) sum = -32768;
    return (int16_t)sum;
}

/* -------------------------------------------------------------------------- */
/*  Brown noise — leaky integrator                                            */
/* -------------------------------------------------------------------------- */

#define BROWN_ALPHA  0.01f         /* Smoothing factor (lower = deeper bass) */
#define BROWN_AMP    12000         /* Output amplitude */

static int16_t brown_sample(void) {
    /*
     * Brown noise = integrated white noise.
     * Each sample is the previous one plus a small random delta.
     *
     * Result: -6 dB/octave roll-off (brown/red noise).
     */

    static float brown_state = 0.0f;

    /* White noise: -1.0 .. +1.0 */
    float white = (float)((int32_t)(esp_random() & 0xFFFF) - 0x8000) / 32768.0f;

    /* Leaky integrator: state = alpha * white + (1 - alpha) * state */
    brown_state = BROWN_ALPHA * white + (1.0f - BROWN_ALPHA) * brown_state;

    /* Scale and clamp */
    float out = brown_state * BROWN_AMP;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

void noise_gen_init(noise_type_t type) {
    s_type = type;
    s_initialized = true;

    /* Warm up the generator to avoid startup transients */
    int16_t dummy;
    for (int i = 0; i < 100; i++) {
        if (type == NOISE_TYPE_PINK) {
            dummy = pink_sample();
        } else {
            dummy = brown_sample();
        }
    }
    (void)dummy;
}

void noise_gen_fill(int16_t *buffer, int count) {
    if (!s_initialized || buffer == NULL) return;

    if (s_type == NOISE_TYPE_PINK) {
        for (int i = 0; i < count; i++) {
            buffer[i] = pink_sample();
        }
    } else {
        for (int i = 0; i < count; i++) {
            buffer[i] = brown_sample();
        }
    }
}
