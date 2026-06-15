#pragma once

/* =========================================================================
 *  Log Tags — Single source of truth for all component TAG strings
 *
 *  Every .c file that logs should use these instead of ad-hoc strings.
 *  This lets log_levels_init() reference tags reliably and keeps
 *  the Logging_Guide.md always accurate.
 *
 *  Usage:
 *    #include "log_tags.h"
 *    static const char *TAG = LOG_TAG_MESH_CORE;
 * ========================================================================= */

/* ---- Mesh ---- */
// This falls under CONFIG_PRIVACY_SHIELD_LOG_MESH
#define LOG_TAG_MESH_CORE   "MESH_CORE"
#define LOG_TAG_DISCOVERY   "DISCOVERY"

/* ---- Audio ---- */
// This falls under CONFIG_PRIVACY_SHIELD_LOG_AUDIO
#define LOG_TAG_AUDIO_MIC   "AUDIO_HAL_MIC"
#define LOG_TAG_AUDIO_AMP   "AUDIO_AMP"       /* future: MAX98357A driver */

/* ---- DSP Engine ---- */
// These fall under CONFIG_PRIVACY_SHIELD_LOG_AUDIO_AFE
#define LOG_TAG_VAD         "DSP_VAD"         /* future: voice activity detection */
#define LOG_TAG_NOISE_GEN   "DSP_NOISE"       /* future: pink/brown noise gen */
#define LOG_TAG_AEC         "DSP_AEC"         /* future: acoustic echo cancellation */
#define LOG_TAG_AUDIO_AFE   "AUDIO_AFE"

/* ---- Web Dashboard ---- */
// This falls under CONFIG_PRIVACY_SHIELD_LOG_WEB
#define LOG_TAG_WEB         "WEB_DASHBOARD"   /* future: Hub HTTP server + API */

/* ---- Main ---- */
#define LOG_TAG_MAIN        "MAIN"
