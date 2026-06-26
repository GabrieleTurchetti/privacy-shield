#include "volume.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "log_tags.h"
/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

static const char *TAG = LOG_TAG_AUDIO_AFE;
static bool is_volume_override = false;
static bool is_masking_override = false;
static bool cmd_mask = false;
static float cmd_volume_level = 0.0f;
float level;


void volume_init(volume_state_t *vol) {
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
                             int count, bool *masking_active) {
    
    if (is_volume_override){
        /* Apply to output */
        level = cmd_volume_level;
        apply_volume(noise_out, count, level);
        // we set command if hub sent command
        // note: if masking_active = fase, afe.c will disbale the audio, setting it to false is sufficient
        *masking_active = is_masking_override ? cmd_mask : (level > 0.05f);
        return (uint8_t)(level * 100.0f);
    }
    
    
    /* Measure speech level from mic */
    float rms = compute_rms(mic_in, count);
    //ESP_LOGI(TAG, "RMS: %.1f (noise floor: %.1f)", rms, vol->noise_floor);

    /* Convert to target volume (log scale, respect noise floor) */
    float target = rms_to_volume(rms, vol->noise_floor);

    /* Smooth transition */
    level = volume_ramp(vol, target);

    /* Apply to output */
    apply_volume(noise_out, count, level);

    /* Set masking state based on meaningful volume */
    *masking_active = (level > 0.05f);  /* >5% = actively masking */

    return (uint8_t)(level * 100.0f);
}


void volume_set_command(uint8_t value){
    is_volume_override = true;
    cmd_volume_level = value / 100.0f;
}

void mask_set_command(uint8_t value){
    is_masking_override = true;
    cmd_mask = value;
}

void volume_unlock(){
    is_volume_override = false;
    is_masking_override = false;
}

uint8_t get_volume(){
    return (uint8_t) (level * 100);
}
