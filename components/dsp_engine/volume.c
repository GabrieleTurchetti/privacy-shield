#include "volume.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "log_tags.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

static const char *TAG = LOG_TAG_AUDIO_AFE;
static bool is_volume_override = false;
static bool is_masking_override = false;
static bool cmd_mask = false;
static float cmd_volume_level = 0.0f;
static float level;
static SemaphoreHandle_t s_volume_mutex = NULL;

#define VOLUME_LOCK()    xSemaphoreTake(s_volume_mutex, portMAX_DELAY)
#define VOLUME_UNLOCK()  xSemaphoreGive(s_volume_mutex)

static void volume_ensure_mutex(void) {
    if (s_volume_mutex == NULL) {
        s_volume_mutex = xSemaphoreCreateMutex();
    }
}

void volume_init(volume_state_t *vol) {
    volume_ensure_mutex();
    vol->current = 0.0f;
    vol->attack_coeff = 0.10f;   /* 10ms  attack  (tracks voice dynamics) */
    vol->release_coeff = 0.01f;  /* 100ms  release (fast fade-out) */
    vol->noise_floor = DEFAULT_NOISE_FLOOR;   /* Suitable for DC-corrected MEMS mic */
}

void volume_calibrate(volume_state_t *vol, float measured_rms) {
    /* Set noise floor to 1.25x ambient RMS */
    vol->noise_floor = measured_rms * 1.25f;
    if (vol->noise_floor < 200.0f) vol->noise_floor = 200.0f;
    if (vol->noise_floor > 4000.0f) vol->noise_floor = 4000.0f;
}

float compute_rms(const int16_t *samples, int count) {
    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float s = (float)samples[i];
        sum += s * s;
    }
    return sqrtf(sum / (float)count);
}

float rms_to_volume(float rms, float noise_floor) {
    /* Below noise floor = silence */
    if (rms <= noise_floor) return 0.0f;

    float effective_rms = rms - noise_floor;
    float vol = effective_rms / MAX_RMS;

    if (vol < 0.0f) return 0.0f;
    if (vol > 1.0f) return 1.0f;
    return vol;
}

float volume_ramp(volume_state_t *vol, float target) {
    float coeff = (target > vol->current) ? vol->attack_coeff : vol->release_coeff;
    vol->current += coeff * (target - vol->current);
    return vol->current;
}

void apply_volume(int16_t *buffer, int count, float level) {
    if (level >= 1.0f) return;  /* No scaling needed */
    for (int i = 0; i < count; i++) {
        buffer[i] = (int16_t)((float)buffer[i] * level);
    }
}

uint8_t volume_process_frame(volume_state_t *vol,
                             const int16_t *mic_in, int16_t *noise_out,
                             int count, bool *masking_active, bool is_vad_speech) {
    volume_ensure_mutex();
    VOLUME_LOCK();
    bool vol_override = is_volume_override;
    bool mask_override = is_masking_override;
    float cmd_vol = cmd_volume_level;
    bool cmd_m = cmd_mask;
    VOLUME_UNLOCK();

    float target = 0.0f;
    float current_level = 0.0f;

    /* Calculate target volume: Silence has priority over volume override */
    if (!is_vad_speech) {
        /* VAD detects silence, force target to 0 */
        target = 0.0f;
    } else {
        /* VAD detects speech, check if we should use override or RMS */
        if (vol_override) {
            target = cmd_vol;
        } else {
            float rms = compute_rms(mic_in, count);
            target = rms_to_volume(rms, vol->noise_floor);
        }
    }

    /* Handle immediate cut (mute) vs smooth ramp */
    if (mask_override && cmd_m == false) {
        /* Hard cut to 0: bypass ramp and reset internal state to prevent glitches */
        current_level = 0.0f;
        vol->current = 0.0f;
    } else {
        /* Apply smooth transition (ramp) towards the target */
        current_level = volume_ramp(vol, target);
    }

    /* Set the final masking state to return to afe.c */
    if (mask_override) {
        *masking_active = cmd_m;
    } else {
        *masking_active = (current_level > 0.05f);  /* >5% = actively masking */
    }

    /* Apply the calculated volume to the output buffer */
    apply_volume(noise_out, count, current_level);

    VOLUME_LOCK();
    level = current_level;
    VOLUME_UNLOCK();

    return (uint8_t)(current_level * 100.0f);
}

void volume_set_command(uint8_t value) {
    volume_ensure_mutex();
    VOLUME_LOCK();
    is_volume_override = true;
    cmd_volume_level = value / 100.0f;
    VOLUME_UNLOCK();
}

void mask_set_command(uint8_t value) {
    volume_ensure_mutex();
    VOLUME_LOCK();
    is_masking_override = true;
    cmd_mask = value;
    VOLUME_UNLOCK();
}

void volume_unlock() {
    volume_ensure_mutex();
    VOLUME_LOCK();
    is_volume_override = false;
    is_masking_override = false;
    VOLUME_UNLOCK();
}

uint8_t get_volume() {
    volume_ensure_mutex();
    VOLUME_LOCK();
    uint8_t v = (uint8_t)(level * 100);
    VOLUME_UNLOCK();
    return v;
}
