# Privacy Shield — Detailed Sprint Plan

> **Last updated:** 2026-05-19  
> **Repo:** [GabrieleTurchetti/privacy-shield](https://github.com/GabrieleTurchetti/privacy-shield)  
> **Branch:** `sprint-1` (active development)

---

## Progress Overview

| Sprint | Tasks | Completed | Remaining |
|---|---|---|---|
| Sprint 1 — Hardware Bring-up & Basic Mesh | 6 | 4 | 2 |
| Sprint 2 — Audio Pipeline & Adaptive Masking | 6 | 0 | 6 |
| Sprint 3 — Acoustic Echo Cancellation | 4 | 0 | 4 |
| Sprint 4 — Hub Dashboard & Hardware Finalization | 6 | 0 | 6 |

---

## Sprint 1 — Hardware Bring-up & Basic Mesh

**Goal:** Have two devices powered on, capturing audio, playing audio, and talking to each other over ESP-NOW.

| Task | Issue | Status | Implemented By |
|---|---|---|---|
| 1.1 Development Environment | [#3](https://github.com/GabrieleTurchetti/privacy-shield/issues/3) | ✅ Done | ESP-IDF v5.5.4, ESP32-S3, 8MB PSRAM, 240MHz, FreeRTOS 1000Hz |
| 1.2 I2S Microphone Driver | [#4](https://github.com/GabrieleTurchetti/privacy-shield/issues/4) | ✅ Done | `components/audio_hal/i2s_mic.c` — 16kHz, 32-bit I2S, DC calibration |
| 1.3 Amplifier + Transducer Driver | [#5](https://github.com/GabrieleTurchetti/privacy-shield/issues/5) | ⬜ Pending | — |
| 1.4 ESP-NOW Basic Communication | [#6](https://github.com/GabrieleTurchetti/privacy-shield/issues/6) | ✅ Done | `components/mesh_core/esp_now_link.c` — init, broadcast, send/recv callbacks |
| 1.5 Node Discovery | [#7](https://github.com/GabrieleTurchetti/privacy-shield/issues/7) | ✅ Done | `components/mesh_core/node_discovery.c` — neighbor table, HELLO, 30s timeout |
| 1.6 Mechanical Isolation Test | [#8](https://github.com/GabrieleTurchetti/privacy-shield/issues/8) | ⬜ Pending | — |

### What was actually delivered (Sprint 1 so far)

**Architecture:**

```
Core 0                              Core 1
─────────────────────────────────────────────────────
WiFi task (ESP-NOW callbacks)
  └─ recv/send event handling       Mic_Task (pri 5)
                                       └─ blocks on I2S DMA (32ms/buffer)
hello_task (pri 1, every 10s)            └─ DC offset calibration (1s)
  └─ mesh_send_hello()                   └─ raw sample dump (debug)
                                          └─ 16-bit queue → DSP (production)
prune_task (pri 2, every 2s)
  └─ mesh_discovery_prune()
  └─ neighbor count logging
```

**Beyond the task checklist:**
- Kconfig debug/production toggle (`idf.py menuconfig` → Privacy Shield Configuration)
- Centralized pin definitions in `main/include/global_config.h`
- Packed binary protocol: HELLO, STATUS, COMMAND, ACK packets
- Multi-core task architecture with separate cores for audio and mesh
- DC offset calibration (1-second auto-calibrate on boot)
- ESP-NOW v5.5.4 unified callback API
- `docs/ESP-NOW_Mesh_Core.md` — 300-line implementation guide
- Baud rate auto-configuration (2M baud only in debug mode)

**Known issues:**
- `prune_task` stack must be ≥4096 bytes (2048 caused stack overflow on neighbor timeout)
- `hello_task` docstring says "5s" but code runs at 10s (cosmetic)

**Deliverable status:** Two devices communicate over ESP-NOW, capture audio independently. ⬜ Amp/transducer still needed.

---

## Sprint 2 — Audio Pipeline & Adaptive Masking

**Goal:** Each device detects speech and emits adaptive Pink/Brown noise masking independently.

| Task | Issue | Status |
|---|---|---|
| 2.1 Audio Capture Buffer Pipeline | [#9](https://github.com/GabrieleTurchetti/privacy-shield/issues/9) | ⬜ Pending |
| 2.2 Voice Activity Detection (VAD) | [#10](https://github.com/GabrieleTurchetti/privacy-shield/issues/10) | ⬜ Pending |
| 2.3 Pink/Brown Noise Generation | [#11](https://github.com/GabrieleTurchetti/privacy-shield/issues/11) | ⬜ Pending |
| 2.4 Adaptive Masking Algorithm | [#12](https://github.com/GabrieleTurchetti/privacy-shield/issues/12) | ⬜ Pending |
| 2.5 Autonomous Mode | [#13](https://github.com/GabrieleTurchetti/privacy-shield/issues/13) | ⬜ Pending |
| 2.6 Real-Room Test | [#14](https://github.com/GabrieleTurchetti/privacy-shield/issues/14) | ⬜ Pending |

### Task 2.1 — Audio Capture Buffer Pipeline

The ESP32-S3 I2S peripheral supports hardware DMA (Direct Memory Access), meaning audio samples arrive in memory without CPU intervention.

**How it works:**
- The I2S peripheral continuously captures audio samples from the MEMS microphone
- DMA writes samples directly into a circular buffer in memory
- When one buffer fills, the DMA switches to the next buffer and fires an interrupt
- The CPU handles the filled buffer (e.g., feed to VAD) while DMA fills the other
- This is called a **ping-pong buffer** — two buffers swapped automatically

**Configuration details:**
- Sample rate: **16kHz** (good enough for speech, keeps CPU/Memory low)
- Bit depth: **16-bit** (standard for VAD and AEC)
- Channel: **Mono** (single mic per device)
- Buffer size: **512 samples per buffer** (= 32ms of audio at 16kHz)
- Number of buffers: **2** (ping-pong), plus 2 more behind DMA for safety = 4 total

**Memory usage per buffer:**
- 512 samples × 2 bytes per sample = 1KB per buffer
- 4 buffers = 4KB total — negligible on an 8MB PSRAM device

**Verification:**
- Log buffer timestamps — they should arrive every ~32ms with no gaps
- Dump raw audio and verify no silence gaps / dropped samples in a recording
- Test for 1 minute; any gap > 35ms indicates a problem

### Task 2.2 — Voice Activity Detection (VAD)
- Implement energy-based VAD (simple RMS threshold)
- Better: implement spectral VAD (look at frequency bands, speech is 300Hz–3kHz)
- State machine: silence → speech → silence with hold time (prevent flutter)
- Test with real speech vs background noise

### Task 2.3 — Pink/Brown Noise Generation
- Generate Pink noise (filter white noise with -3dB/octave)
- Generate Brown noise (filter white noise with -6dB/octave)
- Pre-compute noise buffers at boot to save CPU
- Play through transducer on VAD trigger

### Task 2.4 — Adaptive Masking Algorithm
- When VAD triggers, start masking
- Scale masking volume proportionally to input speech volume
- Smooth ramping (attack ~50ms, release ~500ms) to avoid sudden jumps
- Test: speak softly → quiet masking; shout → loud masking

### Task 2.5 — Autonomous Mode
- Each device masks based on what it hears locally
- Devices broadcast "masking state" (ON/OFF + volume level) over ESP-NOW
- Other devices use this for awareness

### Task 2.6 — Real-Room Test
- Set up in a real room
- Person speaks at normal volume inside
- Another person listens from outside the door
- Rate masking effectiveness subjectively (1-10)
- Tune parameters and re-test

**Deliverable:** A device placed on a door masks a conversation to some degree autonomously.

---

## Sprint 3 — Acoustic Echo Cancellation

**Goal:** Eliminate the feedback loop where the mic hears its own transducer output.

| Task | Issue | Status |
|---|---|---|
| 3.1 AEC Algorithm Implementation (NLMS) | [#15](https://github.com/GabrieleTurchetti/privacy-shield/issues/15) | ⬜ Pending |
| 3.2 Double-Talk Detection (Geigel) | [#16](https://github.com/GabrieleTurchetti/privacy-shield/issues/16) | ⬜ Pending |
| 3.3 AEC Integration with Pipeline | [#17](https://github.com/GabrieleTurchetti/privacy-shield/issues/17) | ⬜ Pending |
| 3.4 AEC Stability Testing | [#18](https://github.com/GabrieleTurchetti/privacy-shield/issues/18) | ⬜ Pending |

### Task 3.1 — AEC Algorithm Implementation

AEC solves a specific problem: the microphone hears not only the conversation, but also the masking noise coming from the transducer. Without AEC, the system would try to mask the masking noise itself — creating a feedback loop.

**How NLMS (Normalized Least Mean Squares) works:**

```
Reference (what we play) ──┬──► estimated echo path (filter) ──► estimated echo
                           │
Mic signal ─────────────────┴───────────► (+) ──► Error (clean speech)
                                              (-)
                                              ▲
                                        estimated echo
```

1. The adaptive filter models the **acoustic path** from transducer → surface → air → microphone
2. It convolves the reference signal with the filter to estimate the echo
3. The estimated echo is subtracted from the mic signal
4. The difference (error) is used to **update the filter**, making it converge toward the real acoustic path

**Filter length: 512–1024 taps**
- At 16kHz sample rate, 512 taps = 32ms of echo path
- At 16kHz sample rate, 1024 taps = 64ms of echo path
- Memory: 1024 taps × 4 bytes (float) = 4KB for the filter — plus internal buffers (~8KB total)

**Implementation on ESP32-S3:**
- All operations use fixed-point (int32) to avoid FPU overhead
- The AEC loop runs once per buffer (every 32ms)
- Budget: under 10ms of CPU time per 32ms buffer

### Task 3.2 — Double-Talk Detection (Geigel Algorithm)

Double-talk is when **both** the transducer is playing masking noise AND a person is speaking. During double-talk, AEC adaptation must be frozen to prevent filter divergence.

**The Geigel Algorithm:**
1. Compare the microphone signal level to the reference signal level
2. If `|mic_signal| > γ × |reference_signal|`, declare double-talk (γ = 0.5–0.7)
3. During double-talk: **freeze AEC adaptation**
4. When double-talk ends: resume adaptation

**Why this works:** The echo path has natural attenuation (6-20dB loss). If mic signal is nearly as loud as reference, there must be a local speaker.

### Task 3.3 — AEC Integration with Pipeline
- Mic → AEC → VAD → Masking decision
- AEC runs continuously in the background
- VAD operates on the AEC-clean signal
- Verify: no audible feedback/whistling at any masking volume

### Task 3.4 — Stability Testing
- Run AEC continuously for 2+ hours
- Test with various audio environments (music, TV, silence, conversation)
- Ensure filter doesn't diverge over time
- Test at max masking volume (worst-case feedback)

**Deliverable:** Devices mask without audible feedback, even at high volumes.

---

## Sprint 4 — Hub Dashboard & Hardware Finalization

**Goal:** Full control interface + final hardware design.

| Task | Issue | Status |
|---|---|---|
| 4.1 Hub ESP32 Firmware | [#19](https://github.com/GabrieleTurchetti/privacy-shield/issues/19) | ⬜ Pending |
| 4.2 Web Dashboard (HTML/CSS/JS) | [#20](https://github.com/GabrieleTurchetti/privacy-shield/issues/20) | ⬜ Pending |
| 4.3 Hub REST API | [#21](https://github.com/GabrieleTurchetti/privacy-shield/issues/21) | ⬜ Pending |
| 4.4 Mesh Nodes Receive Commands | [#22](https://github.com/GabrieleTurchetti/privacy-shield/issues/22) | ⬜ Pending |
| 4.5 3D-Printed Enclosure Design | [#23](https://github.com/GabrieleTurchetti/privacy-shield/issues/23) | ⬜ Pending |
| 4.6 Enclosure Assembly + Testing | [#24](https://github.com/GabrieleTurchetti/privacy-shield/issues/24) | ⬜ Pending |

### Task 4.1 — Hub ESP32 Firmware
- One ESP32-S3 configured as Hub
- Runs WiFi softAP + ESP-NOW simultaneously
- ESP-NOW: listens for all mesh node broadcasts
- WiFi: serves HTTP Web dashboard

### Task 4.2 — Web Dashboard (HTML/CSS/JS)
- Served directly from the Hub's SPIFFS filesystem
- Pages: Status (nodes, battery, uptime, masking state), Control (mute/unmute, volume), Settings
- Auto-refresh via JavaScript polling (~2s)
- Responsive design (phone, tablet, desktop)

### Task 4.3 — Hub REST API
- `GET /api/nodes` → JSON list of all nodes + stats
- `POST /api/node/{id}/mute` → mute a node
- `POST /api/node/{id}/unmute` → unmute a node
- `POST /api/node/{id}/volume?level=50` → set volume
- `POST /api/global/mute` → mute all
- `POST /api/global/unmute` → unmute all

### Task 4.4 — Mesh Nodes: Receive Commands
- Parse incoming ESP-NOW COMMAND messages from the Hub
- Respond to mute, unmute, volume commands
- Broadcast status updates (battery, uptime) periodically

### Task 4.5 — 3D-Printed Enclosure Design
- Two chambers: mic (isolated, foam-mounted, port to room) + transducer (surface-coupled)
- Physical isolation between chambers
- Battery compartment + ventilation for MAX98357A

### Task 4.6 — Enclosure Assembly + Testing
- 3D print, assemble, test masking effectiveness
- Long-duration test (8h+ continuous)

**Deliverable:** Full system: autonomous masking mesh + browser-controlled + 3D-printed enclosure.
