# DSP Engine — Implementation Guide

> **Component:** `dsp_engine`  
> **Sprint:** 2  
> **ESP-IDF:** v5.5.4  

## Overview

The `dsp_engine` component runs the real-time audio processing pipeline: Voice Activity Detection (VAD), noise generation, and adaptive volume control.

All processing runs inside `afe_processing_task()` on Core 0.

---

## Pipeline

```
Every 32ms frame:

  xQueueReceive(audio_ai_queue)     → mic_frame (160 samples, 16-bit PCM)
       │
       ▼
  audio_afe_feed(mic_frame)         → ESP-SR VADNet1 processing
       │
       ▼
  audio_afe_fetch(&result)          → vad_state (SPEECH / SILENCE)
       │
       ▼
  [if SPEECH]
  noise_gen_fill(noise_buffer, 160) → pink/brown noise
       │
       ▼
  volume_process_frame(...)         → scale noise to match speech loudness
       │
       ▼
  xQueueSend(noise_queue)           → speaker task plays it
```

---

## Modules

### AFE / VAD (`afe.c`, `afe.h`)

Wraps Espressif's ESP-SR Audio Front End library.

**Init:** `audio_afe_init("M")` — single microphone mode, VADNet1 Medium model, no AEC reference channel.

**Feed:** `audio_afe_feed(pcm)` — sends 16-bit PCM to ESP-SR pipeline.

**Fetch:** `audio_afe_fetch(&result)` — retrieves processed output + VAD state.

**State:** `get_afe_state()` returns:
- `AUDIO_AFE_VAD_SPEECH` — person is talking
- `AUDIO_AFE_VAD_SILENCE` — silence
- `AUDIO_AFE_VAD_UNKNOWN` — indeterminate

**Task:** `afe_processing_task()` — Core 0, priority 5, 8192-byte stack. Runs the full pipeline loop.

### Noise Generator (`noise_gen.c`, `noise_gen.h`)

Generates masking noise to obscure speech.

**Init:** `noise_gen_init(NOISE_TYPE_PINK)` or `NOISE_TYPE_BROWN`

| Type | Character | -dB/oct | Best for |
|---|---|---|---|
| Pink | Waterfall-like | -3 | General speech masking |
| Brown | Deep rumble | -6 | Low-frequency masking |

**Fill:** `noise_gen_fill(buffer, count)` — fills buffer with int16_t noise samples.
- Pink: Voss-McCartney algorithm (7 octaves of white noise sources)
- Brown: Leaky integrator (integrated white noise)
- CPU cost: ~0.5ms per 160 samples at 240MHz

### Volume Control (`volume.c`, `volume.h`)

Adaptively scales masking output based on speech loudness.

**Init:** `volume_init(&state)` — sets default parameters

**Per-frame:** `volume_process_frame(&state, mic_in, noise_out, count, &masking)`:
1. `compute_rms(mic_in)` — measure speech energy
2. `rms_to_volume(rms, noise_floor)` — map to 0.0–1.0 (linear, ceiling at 5000 RMS)
3. `volume_ramp(&state, target)` — smooth transition (10ms attack, 50ms release)
4. `apply_volume(noise_out, level)` — scale every sample
5. Returns volume 0–100 (for STATUS packet)

**Calibration:** `volume_calibrate(&state, ambient_rms)` — auto-sets noise floor from ambient measurements.

**Key parameters:**

| Parameter | Default | Effect |
|---|---|---|
| `noise_floor` | 500 | RMS below this = silence |
| `attack_coeff` | 0.10 | 10ms ramp-up speed |
| `release_coeff` | 0.02 | 50ms fade-out speed |
| Mapping ceiling | 5000 | Max RMS for 100% volume (tune this!) |

---

## Tuning the Volume Ceiling

The linear mapping ceiling (5000) determines sensitivity. Adjust in `volume.c`:

| Ceiling | Quiet (RMS 1000) | Normal (RMS 2500) | Loud (RMS 4000) |
|---|---|---|---|
| 3000 | 33% | 83% | 100% |
| **5000 (default)** | **20%** | **50%** | **80%** |
| 8000 | 12% | 31% | 50% |

Lower ceiling = more sensitive. Higher ceiling = more dynamic range. Tune based on your mic model and mounting position.

---

## FreeRTOS Tasks

| Task | Core | Priority | Stack | Period |
|---|---|---|---|---|
| `Mic_Read` | 1 | 5 | 4096 | Continuous (DMA) |
| `AFE_Proc` | 0 | 5 | 8192 | ~32ms per frame |
| `Speaker` | 0 | 3 | 4096 | Event-driven (queue) |

---

## Expected Serial Output

```
I (2845) AUDIO_AFE: Volume calibrated — noise floor: 500 RMS
I (5087) AUDIO_AFE: [VAD] Speech detected!
I (5170) AUDIO_AFE: Masking active — volume 5%
I (5426) AUDIO_AFE: Masking active — volume 15%
I (8155) AUDIO_AFE: Masking active — volume 25%
I (13351) AUDIO_AFE: [VAD] Silence...
I (13359) AUDIO_AFE: Masking inactive — volume 0%
```

Volume changes only logged when it shifts by ≥5% to avoid serial flood.
