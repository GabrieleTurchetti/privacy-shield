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
#define LOG_TAG_MESH_CORE   "MESH_CORE"
#define LOG_TAG_DISCOVERY   "DISCOVERY"

/* ---- Audio ---- */
#define LOG_TAG_AUDIO_MIC   "AUDIO_HAL_MIC"
#define LOG_TAG_AUDIO_AMP   "AUDIO_AMP"       /* future: MAX98357A driver */

/* ---- DSP Engine ---- */
#define LOG_TAG_VAD         "DSP_VAD"         /* future: voice activity detection */
#define LOG_TAG_NOISE_GEN   "DSP_NOISE"       /* future: pink/brown noise gen */
#define LOG_TAG_AEC         "DSP_AEC"         /* future: acoustic echo cancellation */

/* ---- Web Dashboard ---- */
#define LOG_TAG_WEB         "WEB_DASHBOARD"   /* future: Hub HTTP server + API */

/* ---- Main ---- */
#define LOG_TAG_MAIN        "MAIN"
