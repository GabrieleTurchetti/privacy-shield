# Audio HAL Implementation Guide

## Overview

This document outlines the Hardware Abstraction Layer (HAL) for the ESP32-S3 audio intake and output subsystems. It covers the implementation details of the I2S microphone pipeline (`i2s_mic.c`) and the I2S amplifier test module (`max_amp.c`). 

These scripts handle the direct hardware interactions required to ingest environmental sound for the Digital Signal Processing (DSP) engine and to emit masking audio through connected transducers.

---

## I2S Microphone Subsystem (`i2s_mic.c`)

The `i2s_mic.c` module handles the configuration, calibration, and continuous reading of an I2S MEMS microphone. It is responsible for gathering raw audio, converting the bit depth, and feeding the AI engine via a FreeRTOS queue.

### Initialization

The `audio_hal_mic_init()` function initializes the I2S peripheral for audio capture.
* **I2S Configuration**: The channel is set to `I2S_ROLE_MASTER` targeting a default sample rate of 16000 Hz.
* **Data Format**: Hardware is configured for a 32-bit data width (`I2S_DATA_BIT_WIDTH_32BIT`) and `I2S_SLOT_MODE_MONO`. 
* **Slot Mask**: It specifically reads from the left channel mask (`I2S_STD_SLOT_LEFT`).
* **Pin Mapping**: The module utilizes `PIN_I2S_MIC_BCLK` for the bit clock, `PIN_I2S_MIC_LRCLK` for the word select, and `PIN_I2S_MIC_DIN` for data input.
* **Debug Mode**: If compiled with both `CONFIG_PRIVACY_SHIELD_BUILD_DEBUG` and `CONFIG_PRIVACY_SHIELD_LOG_AUDIO`, the UART baud rate on `UART_NUM_0` is boosted to 2000000 to prevent serial buffer overflow during raw sample streaming.

### Read Task

The `audio_hal_mic_read_task(void *pvParameters)` is an infinite FreeRTOS task designed to continuously poll the DMA buffer.
* **Calibration Delay**: The task initially requests the user to stay completely quiet for 1 second to perform a baseline calibration.
* **Underrun Detection**: In debug mode, the task tracks the time delta between frames using `esp_timer_get_time()`. It targets roughly 32ms per frame. If the delta exceeds 35ms, an underrun error is immediately logged.
* **Data Conversion**: Raw 32-bit I2S samples (`raw_samples`) are bit-shifted down (`>> 16`) to standard 16-bit audio format (`ai_buffer`).
* **Queue Dispatch**: The down-sampled 16-bit buffer is dispatched to the external `audio_ai_queue` using `xQueueOverwrite()`. If the write fails, an error is logged.

---

## I2S Amplifier Subsystem (`max_amp.c`)

The `max_amp.c` module handles the transmission channel for the MAX98357A I2S amplifier and DAEX25 exciter. It currently serves as a testing utility to validate the output hardware by generating a continuous mathematical sine wave.

### Initialization

The `audio_hal_speaker_init()` function prepares the hardware for audio playback.
* **I2S Configuration**: The TX channel is configured in `I2S_ROLE_MASTER` at the predefined `SPK_SAMPLE_RATE`.
* **Data Format**: It uses a standard Philips slot configuration with 16-bit data width (`I2S_DATA_BIT_WIDTH_16BIT`) and `I2S_SLOT_MODE_MONO`.
* **Pin Mapping**: The module assigns `PIN_AMP_BCLK` for the bit clock, `PIN_AMP_LRCLK` for the word select, and `PIN_AMP_DOUT` for data output.

### Sine Wave Generation Task

The `sine_wave_task(void *pvParameters)` creates and streams a test tone to the transducer.
* **Tone Properties**: The task generates a 1000 Hz sine wave. The amplitude is set to 15000, remaining safely below the 16-bit maximum amplitude of 32767 to avoid clipping.
* **Buffer Pre-calculation**: To conserve CPU cycles during runtime, exactly one full wave cycle (16 samples at a 16000 Hz sample rate) is mathematically pre-calculated and stored in memory allocated by `calloc`.
* **Mathematical Formula**: The discrete points of the waveform are generated using the standard formula `y(t) = A * sin(2 * PI * f * t)`.
* **Streaming**: Once calculated, the task enters an infinite loop, using `i2s_channel_write()` with `portMAX_DELAY` to endlessly push the pre-calculated buffer to the amplifier.