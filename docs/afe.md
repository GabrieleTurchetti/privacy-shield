# ESP-SR Audio Front End Documentation

## Purpose

The `afe.c` and `afe.h` files implement a wrapper around Espressif's **ESP-SR Audio Front End (AFE)**.

This module connects the project’s I2S microphone pipeline to ESP-SR processing.

The current AFE module is responsible for:

- **VAD** — Voice Activity Detection
- **NS** — Noise Suppression
- **AEC** — Acoustic Echo Cancellation, only if a playback reference channel exists
- Tracking the latest VAD state through `get_afe_state()`
- Running `afe_processing_task()`, which receives microphone PCM frames from `audio_ai_queue`

The intended data flow is:

```text
I2S microphone
    ↓
raw int32_t I2S samples
    ↓
convert to int16_t PCM
    ↓
audio_ai_queue
    ↓
afe_processing_task()
    ↓
audio_afe_feed()
    ↓
ESP-SR AFE pipeline: NS / VAD / optional AEC
    ↓
audio_afe_fetch()
    ↓
update AFE_STATE
```

---

## File Layout

Recommended component structure:

```text
components/dsp_engine/
├── CMakeLists.txt
├── afe.c
├── i2s_mic.c
└── include/
    ├── afe.h
    └── audio_hal.h
```

`afe.c` owns the ESP-SR AFE instance.

`afe.h` exposes the public API used by the rest of the application.

---

# `afe.h`

## `AFE_FEED_SAMPLES`

```c
#define AFE_FEED_SAMPLES 160
```

`AFE_FEED_SAMPLES` defines how many microphone PCM samples are sent per AFE feed frame.

For the current setup:

```text
AFE_FEED_SAMPLES = 160 samples
sample rate      = 16000 Hz
duration         = 160 / 16000 = 10 ms
```

So each feed frame represents about **10 ms** of mono audio.

The microphone queue should use this size:

```c
audio_ai_queue = xQueueCreate(1, AFE_FEED_SAMPLES * sizeof(int16_t));
```

---

## `audio_afe_vad_state_t`

```c
typedef enum {
  AUDIO_AFE_VAD_SILENCE = 0,
  AUDIO_AFE_VAD_SPEECH,
  AUDIO_AFE_VAD_UNKNOWN,
} audio_afe_vad_state_t;
```

This enum represents the project-level VAD state.

```text
AUDIO_AFE_VAD_SILENCE = silence / non-speech
AUDIO_AFE_VAD_SPEECH  = speech detected
AUDIO_AFE_VAD_UNKNOWN = unmapped or unexpected state
```

---

## `audio_afe_result_t`

```c
typedef struct {
  int16_t *data;
  int samples;
  int channels;
  audio_afe_vad_state_t vad_state;
} audio_afe_result_t;
```

This struct stores the result returned from `audio_afe_fetch()`.

### Fields

```c
int16_t *data;
```

Pointer to processed audio returned by ESP-SR. This pointer is owned by ESP-SR. Do **not** free it.

```c
int samples;
```

Number of `int16_t` samples returned.

```c
int channels;
```

Number of output channels.

```c
audio_afe_vad_state_t vad_state;
```

Converted project-level VAD state.

---

# Public API

## `audio_afe_init`

```c
esp_err_t audio_afe_init(const char *input_format);
```

Initializes the ESP-SR AFE.

Example:

```c
esp_err_t ret = audio_afe_init("M");
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "AFE initialization failed");
    return;
}
```

---

## Input Format

The `input_format` string describes the input channel layout.

```text
"M"    = one microphone channel
"MM"   = two microphone channels
"MR"   = one microphone channel + one playback reference channel
"MMR"  = two microphone channels + one playback reference channel
```

For the current single MEMS microphone setup, use:

```c
audio_afe_init("M");
```

---

## AEC Requirement

AEC means **Acoustic Echo Cancellation**.

AEC requires a playback reference channel:

```text
M = microphone input
R = playback reference signal
```

The current code checks whether the input format contains `R`:

```c
bool has_reference_channel = strchr(input_format, 'R') != NULL;
```

If there is no `R`, AEC is disabled:

```c
afe_config->aec_init = false;
```

So for:

```c
audio_afe_init("M");
```

AEC is correctly disabled.

---

## `audio_afe_feed`

```c
esp_err_t audio_afe_feed(const int16_t *pcm);
```

Feeds one frame of raw `int16_t` PCM audio into ESP-SR AFE.

For the current setup:

```text
feed_chunksize = 160
feed_channels  = 1
total samples  = 160
```

So the input buffer should be:

```c
int16_t mic_frame[160];
```

Then:

```c
audio_afe_feed(mic_frame);
```

---

## Important `feed()` Return Behavior

The current code treats only negative return values as errors:

```c
int ret = afe_handle->feed(afe_data, pcm);

if (ret < 0) {
    ESP_LOGE(TAG, "AFE feed returned: %d", ret);
    return ESP_FAIL;
}
```

This is important because positive return values can indicate accepted data.

For example:

```text
320
```

can mean:

```text
160 samples * sizeof(int16_t) = 320 bytes accepted
```

So `320` should not be treated as failure.

---

## `audio_afe_fetch`

```c
esp_err_t audio_afe_fetch(audio_afe_result_t *out_result);
```

Fetches one processed output frame from ESP-SR AFE.

It returns:

- processed audio pointer
- sample count
- channel count
- VAD state

Example:

```c
audio_afe_result_t result;

if (audio_afe_fetch(&result) == ESP_OK) {
    if (result.vad_state == AUDIO_AFE_VAD_SPEECH) {
        ESP_LOGI(TAG, "Speech detected");
    }
}
```

---

## Feed Size vs Fetch Size

Feed and fetch sizes are not necessarily equal.

Current observed values:

```text
feed_chunksize  = 160
fetch_chunksize = 512
```

That means:

```text
feed()  consumes 160 input samples
fetch() returns 512 processed output samples
```

At 16 kHz:

```text
160 samples = 10 ms
512 samples = 32 ms
```

So one `feed()` call does not necessarily produce one successful `fetch()` result.

The processing task should accumulate enough fed samples before calling `fetch()`.

---

## `get_afe_state`

```c
audio_afe_vad_state_t get_afe_state(void);
```

Returns the latest known AFE VAD state.

The state is stored internally as:

```c
static audio_afe_vad_state_t AFE_STATE;
```

Example:

```c
if (get_afe_state() == AUDIO_AFE_VAD_SPEECH) {
    // Speech is currently detected
}
```

---

## `afe_processing_task`

```c
void afe_processing_task(void *pvParameters);
```

This FreeRTOS task:

1. Waits for a microphone PCM frame from `audio_ai_queue`
2. Calls `audio_afe_feed()`
3. Calls `audio_afe_fetch()` when enough input has accumulated
4. Updates `AFE_STATE`
5. Logs VAD state changes

The task assumes:

```c
audio_afe_init("M");
```

has already been called, and that `audio_ai_queue` has already been created.

---

## `audio_afe_destroy`

```c
void audio_afe_destroy(void);
```

Destroys the ESP-SR AFE instance and resets internal state.

---

# `afe.c`

## Internal ESP-SR State

`afe.c` owns:

```c
static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
```

These should remain private to `afe.c`.

It also stores:

```c
static int feed_chunksize = 0;
static int feed_channels = 0;
static int fetch_chunksize = 0;
static int fetch_channels = 0;
static audio_afe_vad_state_t AFE_STATE;
```

---

## External Queue

`afe.c` uses:

```c
extern QueueHandle_t audio_ai_queue;
```

This means the actual queue must be defined in one `.c` file only, usually `main.c`:

```c
QueueHandle_t audio_ai_queue = NULL;
```

All other files should use `extern`.

---

## VAD State Conversion

The helper:

```c
static audio_afe_vad_state_t convert_vad_state(vad_state_t state)
```

maps ESP-SR VAD states to project states:

```text
VAD_SPEECH  -> AUDIO_AFE_VAD_SPEECH
VAD_SILENCE -> AUDIO_AFE_VAD_SILENCE
other       -> AUDIO_AFE_VAD_UNKNOWN
```

---

# Initialization Details

## Model Loading

ESP-SR models are loaded with:

```c
srmodel_list_t *models = esp_srmodel_init("model");
```

Therefore, the partition table must contain a partition named:

```text
model
```

Example CSV line:

```csv
model,data,spiffs,,2M,
```

The name in code:

```c
esp_srmodel_init("model");
```

must match the partition name exactly.

---

## AFE Configuration

The AFE is created with:

```c
afe_config_t *afe_config =
    afe_config_init(input_format, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
```

Meaning:

```text
input_format       = "M", "MR", etc.
models             = ESP-SR model list loaded from flash
AFE_TYPE_SR        = speech-recognition front-end mode
AFE_MODE_HIGH_PERF = high-performance AFE mode
```

---

## VAD Configuration

The current code enables VAD with:

```c
afe_config->vad_init = true;
afe_config->vad_mode = VAD_MODE_1;
afe_config->vad_min_noise_ms = 500;
afe_config->vad_min_speech_ms = 64;
afe_config->vad_delay_ms = 128;
```

These settings are relatively sensitive.

```text
vad_min_noise_ms  = 500 ms  -> shorter noise adaptation window
vad_min_speech_ms = 64 ms   -> shorter speech trigger duration
vad_delay_ms      = 128 ms  -> speech-start cache/delay
```

Lower `vad_min_speech_ms` can make VAD react faster, but it may also increase false positives.

---

## Noise Suppression

The current code enables NS:

```c
afe_config->ns_init = true;
```

NS can reduce steady noise, but it may also affect speech recognition accuracy. For testing VAD sensitivity, compare:

```text
VAD only
```

against:

```text
NS + VAD
```

---

## AEC Configuration

The current code enables AEC only if `input_format` contains `R`.

For a single microphone:

```c
audio_afe_init("M");
```

AEC is disabled.

For mic plus playback reference:

```c
audio_afe_init("MR");
```

AEC can be enabled, but the feed buffer must contain interleaved mic/reference samples.

---

# Queue and Buffer Requirements

## Queue Creation

Create the queue after `audio_afe_init()`:

```c
int feed_samples =
    audio_afe_get_feed_chunksize() * audio_afe_get_feed_channels();

audio_ai_queue = xQueueCreate(1, feed_samples * sizeof(int16_t));
```

For the current setup:

```text
feed_samples = 160
item size    = 160 * 2 = 320 bytes
```

A queue length of `1` with `xQueueOverwrite()` is recommended for real-time VAD.

---

## Queue Send

The microphone task should send the buffer like this:

```c
xQueueOverwrite(audio_ai_queue, ai_buffer);
```

not:

```c
xQueueOverwrite(audio_ai_queue, &ai_buffer);
```

`ai_buffer` already points to the first element of the frame.

---

## Queue Receive

The AFE task should receive like this:

```c
int16_t mic_frame[AFE_FEED_SAMPLES];

xQueueReceive(audio_ai_queue, mic_frame, portMAX_DELAY);
```

---

# I2S Microphone Conversion

The microphone task should read raw I2S samples into:

```c
int32_t raw_samples[AFE_FEED_SAMPLES];
```

Then convert to:

```c
int16_t ai_buffer[AFE_FEED_SAMPLES];
```

Common conversion:

```c
ai_buffer[i] = (int16_t)(raw_samples[i] >> 16);
```

This assumes the MEMS microphone provides useful audio bits in the upper part of the 32-bit I2S slot.

If the signal is too quiet, test:

```c
ai_buffer[i] = (int16_t)(raw_samples[i] >> 14);
```

If it clips or distorts, return to:

```c
ai_buffer[i] = (int16_t)(raw_samples[i] >> 16);
```

---

# Processing Task Logic

The current task waits for mic frames, feeds AFE, and fetches only after enough input has accumulated.

The idea is:

```text
feed 160 samples
feed 160 samples
feed 160 samples
feed 160 samples
then fetch roughly 512 samples
```

Because:

```text
512 / 160 = 3.2
```

So roughly four feed frames are needed before a fetch is expected to succeed.

---

## Current Implementation Note

The current code uses:

```c
int feed_bytes = 0;
feed_bytes += AFE_FEED_SAMPLES;
```

Despite the name, this variable counts **samples**, not bytes.

A clearer name would be:

```c
int fed_samples = 0;
```

Recommended logic:

```c
fed_samples += AFE_FEED_SAMPLES;

if (fed_samples >= fetch_chunksize) {
    audio_afe_result_t result;
    err = audio_afe_fetch(&result);
    fed_samples -= fetch_chunksize;
}
```

Using `>=` is usually better than `>` because it also fetches when the accumulated sample count exactly equals the fetch size.

---

# Common Warnings

## `AEC disabled because input_format has no R reference channel`

Expected for:

```c
audio_afe_init("M");
```

This means the system has no playback reference channel, so AEC is off.

---

## `wakenet model not found`

Expected if WakeNet is not being used.

For VAD-only projects, this can be ignored.

---

## `Ringbuffer of AFE is empty`

Means `fetch()` was called before enough processed output was ready.

Usually caused by:

- fetching too often
- not enough successful `feed()` calls yet
- mic task too slow
- excessive logging

---

## `Ringbuffer of AFE(FEED) is full`

Means the AFE input side is being fed faster than output is fetched.

Usually caused by:

- skipping fetch too often
- treating a successful feed return value like `320` as an error
- logging too much in the audio path
- feed/fetch timing imbalance

---

# Recommended Main Setup

```c
QueueHandle_t audio_ai_queue = NULL;

void app_main(void)
{
    esp_err_t err = audio_afe_init("M");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AFE init failed");
        return;
    }

    int feed_samples =
        audio_afe_get_feed_chunksize() * audio_afe_get_feed_channels();

    if (feed_samples != AFE_FEED_SAMPLES) {
        ESP_LOGE(TAG, "AFE feed size mismatch: macro=%d, afe=%d",
                 AFE_FEED_SAMPLES, feed_samples);
        return;
    }

    audio_ai_queue = xQueueCreate(1, feed_samples * sizeof(int16_t));
    if (audio_ai_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return;
    }

    err = audio_hal_mic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mic init failed");
        return;
    }

    xTaskCreatePinnedToCore(audio_hal_mic_read_task, "Mic_Read_Task", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(afe_processing_task, "AFE_Proc_Task", 8192, NULL, 5, NULL, 0);
}
```

---

# Recommended Microphone Send

```c
int32_t raw_samples[AFE_FEED_SAMPLES];
int16_t ai_buffer[AFE_FEED_SAMPLES];

size_t bytes_read = 0;

esp_err_t err = i2s_channel_read(
    rx_handle,
    raw_samples,
    sizeof(raw_samples),
    &bytes_read,
    portMAX_DELAY
);

if (err == ESP_OK && bytes_read > 0) {
    int samples_read = bytes_read / sizeof(int32_t);

    for (int i = 0; i < samples_read && i < AFE_FEED_SAMPLES; i++) {
        ai_buffer[i] = (int16_t)(raw_samples[i] >> 16);
    }

    for (int i = samples_read; i < AFE_FEED_SAMPLES; i++) {
        ai_buffer[i] = 0;
    }

    xQueueOverwrite(audio_ai_queue, ai_buffer);
}
```

---

# Recommended AFE Task Loop

```c
void afe_processing_task(void *pvParameters)
{
    int16_t mic_frame[AFE_FEED_SAMPLES];
    int fed_samples = 0;

    AFE_STATE = AUDIO_AFE_VAD_SILENCE;

    while (1) {
        if (xQueueReceive(audio_ai_queue, mic_frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = audio_afe_feed(mic_frame);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to feed AFE: %s", esp_err_to_name(err));
            continue;
        }

        fed_samples += AFE_FEED_SAMPLES;

        if (fed_samples < fetch_chunksize) {
            continue;
        }

        fed_samples -= fetch_chunksize;

        audio_afe_result_t result;
        err = audio_afe_fetch(&result);

        if (err != ESP_OK) {
            continue;
        }

        if (result.vad_state != AFE_STATE) {
            if (result.vad_state == AUDIO_AFE_VAD_SPEECH) {
                ESP_LOGI(TAG, "[VAD] Speech detected!");
            } else if (result.vad_state == AUDIO_AFE_VAD_SILENCE) {
                ESP_LOGI(TAG, "[VAD] Silence...");
            } else {
                ESP_LOGW(TAG, "[VAD] Unknown VAD state.");
            }

            AFE_STATE = result.vad_state;
        }
    }
}
```

---

# Build Requirements

The component should require ESP-SR and I2S driver support:

```cmake
idf_component_register(
    SRCS
        "afe.c"
        "i2s_mic.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp-sr
        freertos
)
```

The ESP-SR dependency should be in:

```text
components/dsp_engine/idf_component.yml
```

Example:

```yaml
dependencies:
  espressif/esp-sr: "^2.4.4"
```

---

# Partition Requirement

ESP-SR models are loaded from the partition named:

```text
model
```

Example partition CSV:

```csv
# Name,Type,SubType,Offset,Size,Flags
nvs,data,nvs,0x9000,0x6000,
phy_init,data,phy,0xf000,0x1000,
factory,app,factory,0x10000,3M,
model,data,spiffs,,2M,
```

The code:

```c
esp_srmodel_init("model");
```

must match the partition name:

```text
model
```

---

# Summary

The current `afe.c` / `afe.h` design works as follows:

```text
audio_afe_init("M")
    initializes ESP-SR AFE

audio_afe_feed()
    sends one 160-sample int16_t PCM frame into AFE

audio_afe_fetch()
    retrieves one processed AFE frame and VAD state

afe_processing_task()
    receives PCM frames from audio_ai_queue
    feeds AFE
    fetches output after enough samples have accumulated
    updates AFE_STATE

get_afe_state()
    returns the latest VAD state
```

Important design rules:

```text
Use 160-sample int16_t PCM feed frames.
Do not treat positive feed return values as errors.
Do not fetch after every single feed when fetch size is larger.
Use queue item size AFE_FEED_SAMPLES * sizeof(int16_t).
Use xQueueOverwrite() with queue length 1 for real-time audio.
Keep logging minimal inside audio loops.
```
