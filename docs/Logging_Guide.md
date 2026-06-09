# Logging Guide

> How to control what prints on which device — without editing code.

## Quick Reference

| Level | What it shows | Use for |
|---|---|---|
| `ERROR (1)` | Hardware failures, crashes | Always on |
| `WARN (2)` | + warnings (send failed, table full) | Quiet / Production |
| `INFO (3)` | + normal events (neighbor found, HELLO received) | Normal operation |
| `DEBUG (4)` | + detailed tracing (calibration data, buffer reads) | Development |
| `VERBOSE (5)` | + everything (rarely needed) | Deep debugging |

Levels are **cumulative** — DEBUG includes everything from DEBUG through ERROR.

---

## Build Mode

```bash
idf.py menuconfig → Privacy Shield Configuration → Build Mode
```

| Mode | Raw audio | Logs | Subsystem toggles |
|---|---|---|---|
| **Debug** | Dumped to serial (2M baud) | Configurable per subsystem | Visible in menu |
| **Production** | Off — routed to DSP | **All components at WARN** | Hidden (no effect) |

---

## Subsystem Toggles (Debug mode only)

In Debug build mode, toggle individual components:

```bash
idf.py menuconfig → Privacy Shield Configuration → Subsystem Log Levels

    [✓] Mesh log level: Debug    — mesh_core + discovery at DEBUG
    [✓] Audio log level: Debug   — AUDIO_HAL_MIC at DEBUG
```

| Toggle | Tags controlled | Level when ON | Level when OFF |
|---|---|---|---|
| Mesh Debug | `mesh_core`, `discovery` | DEBUG (4) | WARN (2) |
| Audio Debug | `AUDIO_HAL_MIC` | DEBUG (4) | WARN (2) |
| (always on) | `main` | INFO (3) | INFO (3) |

---

## Workflow examples

```bash
# Full development → Debug mode, both toggles ON
menuconfig:  Build: Debug  →  Mesh: ☑  Audio: ☑
idf.py build flash
# Output: everything from every component

# Debug mesh only → Debug mode, mesh ON, audio OFF
menuconfig:  Build: Debug  →  Mesh: ☑  Audio: ☐
idf.py build flash
# Output: mesh at DEBUG, audio at WARN (errors only)

# Debug audio only → Debug mode, mesh OFF, audio ON
menuconfig:  Build: Debug  →  Mesh: ☐  Audio: ☑
idf.py build flash
# Output: audio at DEBUG, mesh at WARN (errors only)

# Production → Production mode (subsystem menu disappears)
menuconfig:  Build: Production
idf.py build flash
# Output: main at INFO (banner + neighbor count), everything else WARN
```

---

## Under the hood

```c
// main.c — called first in app_main()
static void log_levels_init(void) {

#ifdef CONFIG_PRIVACY_SHIELD_BUILD_PRODUCTION
    esp_log_level_set("*", ESP_LOG_WARN);   // everything quiet
    return;                                  // skip subsystem toggles
#endif

    // Debug mode:
    esp_log_level_set("*", ESP_LOG_INFO);    // mute IDF internals

#ifdef CONFIG_PRIVACY_SHIELD_LOG_MESH         // menuconfig toggle
    esp_log_level_set("mesh_core", ESP_LOG_DEBUG);
    esp_log_level_set("discovery", ESP_LOG_DEBUG);
#else
    esp_log_level_set("mesh_core", ESP_LOG_WARN);
    esp_log_level_set("discovery", ESP_LOG_WARN);
#endif

#ifdef CONFIG_PRIVACY_SHIELD_LOG_AUDIO
    esp_log_level_set("AUDIO_HAL_MIC", ESP_LOG_DEBUG);
#else
    esp_log_level_set("AUDIO_HAL_MIC", ESP_LOG_WARN);
#endif

    esp_log_level_set("main", ESP_LOG_INFO);  // always on
    ESP_LOGI(TAG, "Log levels initialized");
}
```

---

## Tags by file

All tags are defined in **`main/include/log_tags.h`** — the single source of truth.
To add a new component, define its tag there first, then use it in the `.c` file.

| File | Constant | Value | What it logs |
|---|---|---|---|
| `esp_now_link.c` | `LOG_TAG_MESH_CORE` | `"mesh_core"` | Init, send failures, HELLO send |
| `node_discovery.c` | `LOG_TAG_DISCOVERY` | `"discovery"` | New neighbor, timeout, table overflow |
| `i2s_mic.c` | `LOG_TAG_AUDIO_MIC` | `"AUDIO_HAL_MIC"` | Calibration, buffer reads |
| `max_amp.c` | `LOG_TAG_AUDIO_AMP` | `"audio_amp"` | (future) Amplifier/transducer |
| `vad.c` | `LOG_TAG_VAD` | `"dsp_vad"` | (future) Voice activity detection |
| `noise_gen.c` | `LOG_TAG_NOISE_GEN` | `"dsp_noise"` | (future) Noise generation |
| `aec_filter.c` | `LOG_TAG_AEC` | `"dsp_aec"` | (future) Echo cancellation |
| `web_*.c` | `LOG_TAG_WEB` | `"web_dashboard"` | (future) Hub dashboard |
| `main.c` | `LOG_TAG_MAIN` | `"main"` | Banner, neighbor count, commands |

Usage in any `.c` file:
```c
#include "log_tags.h"
static const char *TAG = LOG_TAG_MESH_CORE;  // pick your tag
```

---

## Rules

1. **Always `menuconfig → build → flash`.** The toggles are compile-time. Changing menuconfig alone does nothing.

2. **In Production mode, subsystem toggles have no effect.** The menu disappears and all components run at WARN.

3. **ESP-IDF's `CONFIG_LOG_MAXIMUM_LEVEL`** in `sdkconfig.defaults` controls the compile-time ceiling. By default all levels are compiled in for development. For release, set it to `CONFIG_LOG_MAXIMUM_LEVEL_INFO` in `Component config → Log output → Maximum log verbosity` to strip debug strings from the binary.
