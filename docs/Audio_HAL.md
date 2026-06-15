# Audio HAL Implementation Guide

> **Component:** `audio_hal`  
> **ESP-IDF:** v5.5.4  
> **Last updated:** 2026-06-09  

## Overview

The Audio HAL handles all direct hardware interactions: I2S microphone capture (`i2s_mic.c`) and I2S amplifier output (`max_amp.c`). It bridges raw audio to the DSP pipeline via FreeRTOS queues.

---

## Audio Pipeline

```
Core 1 (Pri 5)                         Core 0 (Pri 5)
┌──────────────────┐                   ┌──────────────────────┐
│ Mic_Read Task    │                   │ AFE_Proc Task        │
│                  │   audio_ai_queue  │                      │
│ I2S DMA 16kHz    │ ────────────────→ │ AFE/VAD              │
│ 32-bit→16-bit    │                   │ noise_gen_fill()     │
│ DC calibration   │                   │ volume_process()     │
│                  │                   │                      │
└──────────────────┘                   └─────────┬────────────┘
                                                 │ noise_queue
                                                 ▼
                                        ┌──────────────────┐
                                        │ Speaker Task     │
                                        │                  │
                                        │ I2S TX 16kHz     │
                                        │ MAX98357A → DAEX │
                                        └──────────────────┘
```

---

## I2S Microphone (`i2s_mic.c`)

### Initialization (`audio_hal_mic_init()`)

- I2S master mode, 16kHz, 32-bit data width, mono, left channel
- Pin mapping from `global_config.h`: `PIN_I2S_MIC_BCLK`, `PIN_I2S_MIC_LRCLK`, `PIN_I2S_MIC_DIN`
- Debug mode: UART baud boosted to 2,000,000 for raw sample streaming
- Returns `ESP_OK` on success, `esp_err_t` on failure

### Read Task (`audio_hal_mic_read_task()`)

Runs on Core 1 at priority 5. Continuous DMA-driven capture:

1. **DC Offset Calibration** (first 1 second / 16,000 samples)
   - Accumulates raw I2S samples during a silent period
   - Computes average DC offset
   - Logs: `Calibration complete! DC Offset: -1046`

2. **Data Conversion** (after calibration)
   - 32-bit I2S → 16-bit audio: `(raw_sample >> 16) - dc_offset`
   - DC offset subtraction centers the signal at zero

3. **Queue Dispatch**
   - Sends 16-bit PCM frames to `audio_ai_queue` via `xQueueOverwrite()`
   - Queue depth = 1 (real-time audio — only latest frame matters)

4. **Debug Monitoring** (build-time gated)
   - Underrun detection: logs if frame interval exceeds 35ms
   - Periodic stability reports: every 50 frames

---

## I2S Amplifier (`max_amp.c`)

### Initialization (`audio_hal_speaker_init()`)

- I2S TX master, 16kHz, 16-bit data width, mono
- Pin mapping: `PIN_AMP_BCLK`, `PIN_AMP_LRCLK`, `PIN_AMP_DOUT`

### Speaker Task (`audio_hal_speaker_task()`)

Runs on Core 0. Receives noise buffers from the DSP pipeline via `noise_queue`:

1. `xQueueReceive(noise_queue, buffer)` — wait for processed audio
2. `i2s_channel_write(tx_chan, buffer)` — send to MAX98357A amplifier

The buffer is already volume-scaled and noise-filled by the DSP engine.

### Test Task (`sine_wave_task()`)

Validation utility — generates a 1000Hz sine wave (amplitude 15000 at 16kHz) and continuously streams to the amplifier. Useful for verifying hardware connections before the DSP pipeline is active.

---

## Queues

| Queue | Size | Direction | Purpose |
|---|---|---|---|
| `audio_ai_queue` | 1 × `AFE_FEED_SAMPLES` × int16_t | Mic → AFE | Raw audio frames |
| `noise_queue` | 2 × `AFE_FEED_SAMPLES` × int16_t | DSP → Speaker | Processed noise output |

Both use `extern` declarations — defined in `main.c`, consumed by component tasks.

---

## Pin Mapping

| Signal | GPIO | Connected To |
|---|---|---|
| `PIN_I2S_MIC_BCLK` | 6 | MEMS mic bit clock |
| `PIN_I2S_MIC_LRCLK` | 4 | MEMS mic word select |
| `PIN_I2S_MIC_DIN` | 5 | MEMS mic data out |
| `PIN_AMP_BCLK` | 15 | MAX98357A bit clock |
| `PIN_AMP_LRCLK` | 16 | MAX98357A word select |
| `PIN_AMP_DOUT` | 17 | MAX98357A data in |
| `PIN_AMP_SD` | 18 | MAX98357A shutdown (high=enabled) |

All pin definitions live in `main/include/global_config.h`.
