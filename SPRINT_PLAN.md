# Privacy Shield — Detailed Sprint Plan

## Sprint 1 — Hardware Bring-up & Basic Mesh

**Goal:** Have two devices powered on, capturing audio, playing audio, and talking to each other over ESP-NOW.

### Task 1.1 — Development Environment
- Install ESP-IDF v5.x with FreeRTOS
- Configure for ESP32-S3 with 8MB PSRAM
- Blink test on two boards
- Set up project structure (components/, main/, sdkconfig)

### Task 1.2 — I2S Microphone Driver
- Wire I2S MEMS mic to ESP32-S3 (BCLK, LRCLK, DIN)
- Configure I2S peripheral (16-bit, 16kHz)
- Capture audio samples and dump over serial
- Verify audio capture quality (no clipping, no hiss)

### Task 1.3 — Amplifier + Transducer Driver
- Wire MAX98357A to ESP32-S3 (BCLK, LRCLK, DOUT)
- Wire DAEX25 to MAX98357A output
- Generate simple tones (1kHz sine) and play through transducer
- Verify transducer vibrates audibly
- Test on different surfaces (door, drywall, window)

### Task 1.4 — ESP-NOW Basic Communication
- Set up ESP-NOW on both devices
- Send simple broadcast packets (device ID, timestamp)
- Receive and display packets on the other device
- Test round-trip latency

### Task 1.5 — Node Discovery
- Devices broadcast HELLO with their ID on boot
- Each device maintains a list of known neighbors
- Handle device leaving the mesh (timeout after ~30s of silence)

### Task 1.6 — Mechanical Isolation Test
- Mount transducer on a surface
- Place mic in different positions relative to the transducer
- Test foam/silicone decoupling between mic and enclosure
- Document which configuration gives the least vibration pick-up

**Deliverable:** Two devices communicate over ESP-NOW, capture and play audio independently.

---

## Sprint 2 — Audio Pipeline & Adaptive Masking

**Goal:** Each device detects speech and emits adaptive Pink/Brown noise masking independently.

### Task 2.1 — Audio Capture Buffer Pipeline

This is the foundation of the entire audio pipeline. The ESP32-S3 I2S peripheral supports hardware DMA (Direct Memory Access), meaning audio samples arrive in memory without CPU intervention. We need to set this up correctly.

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

**Why this matters:**
- If the CPU doesn't process a buffer before DMA fills the next one, samples are lost (underrun)
- 32ms per buffer gives the CPU ample time (at 240MHz) to run VAD + AEC between interrupts
- The double-buffer design ensures **zero audio drops** as long as processing stays under 32ms

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

### Task 3.1 — AEC Algorithm Implementation

AEC solves a specific problem: the microphone hears not only the conversation, but also the masking noise coming from the transducer. Without AEC, the system would try to mask the masking noise itself — creating a feedback loop.

**How NLMS (Normalized Least Mean Squares) works:**

The core idea is we know exactly what audio we're sending to the transducer (the "reference signal"). The mic hears a delayed, distorted version of that reference (echo) mixed with the actual speech. We want to subtract the echo.

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
- A door-mounted transducer → mic path might have reflections up to ~40ms
- 1024 taps gives a safety margin
- Memory: 1024 taps × 4 bytes (float) = 4KB for the filter — plus internal buffers (~8KB total)
- Well within ESP32-S3 PSRAM limits

**Convergence considerations:**
- NLMS converges within a few hundred milliseconds in a quiet room
- Convergence speed is controlled by `mu` (step size):
  - Higher mu = faster convergence but noisier filter (less echo removal)
  - Lower mu = slower but cleaner
  - Start with `mu = 0.1` and tune
- The "normalized" part means the update is scaled by the reference signal power, making it stable regardless of volume

**Implementation on ESP32-S3:**
- All operations use fixed-point (int32) to avoid FPU overhead
- ESP32-S3 has no hardware FPU for single-precision; fixed-point is 5-10x faster
- The AEC loop runs once per buffer (every 32ms)
- Budget: under 10ms of CPU time per 32ms buffer is achievable

**Output:**
- The error signal = microphone input with the masking echo subtracted
- This clean signal is fed to VAD (Sprint 2)
- Result: VAD correctly detects speech, not the masking noise

### Task 3.2 — Double-Talk Detection

**What is double-talk?**

Double-talk is when **both** things happen at the same time:
- The transducer is playing masking noise
- A person in the room is speaking

**Why it's a problem:**

The AEC filter needs to differentiate between:
- "Echo" (masking noise re-captured by the mic) — should be cancelled
- "Near-end speech" (a person talking) — should be preserved

During double-talk, if the AEC filter keeps adapting, it mistakes the speech for echo and tries to cancel it. This causes the filter coefficients to drift in the wrong direction — called **filter divergence**. Once diverged, the filter needs to re-converge, causing a period of poor echo cancellation or even audible artifacts.

**The Geigel Algorithm (standard approach):**

This is a simple and effective double-talk detector commonly used with AEC:

1. Compare the microphone signal level to the reference signal level
2. If `|mic_signal| > γ × |reference_signal|`, declare **double-talk**
   - γ (gamma) is typically 0.5 to 0.7
   - This means: if mic signal is more than ~60% of reference, speech must be happening
3. During double-talk: **freeze AEC adaptation** (keep filter coefficients unchanged)
4. When double-talk ends: resume adaptation

**Why this works:**
- The echo path has a natural attenuation (transducer → air → mic typically loses 6-20dB)
- So the echo coming back is always quieter than what was sent
- If the mic signal is nearly as loud as the reference, **there must be a local speaker**

**Implementation details:**
- Compute short-term energy (RMS) of last 32ms for both reference and mic signal
- Compare every buffer cycle (every 32ms)
- Add a hold time (~100ms) — once double-talk is detected, stay in double-talk mode briefly
- This prevents rapid toggling when speech starts/stops
- If double-talk persists for > 30 seconds, force a 1-step update (prevents permanent freeze on mis-detection)

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

### Task 4.1 — Hub ESP32 Firmware
- One ESP32-S3 configured as Hub
- Runs WiFi softAP + ESP-NOW simultaneously
- ESP-NOW: listens for all mesh node broadcasts
- WiFi: serves HTTP Web dashboard

### Task 4.2 — Web Dashboard (HTML/CSS/JS)
- Served directly from the Hub's SPIFFS filesystem
- Pages:
  - **Status page:** list all nodes in mesh with ID, battery level, uptime, masking state
  - **Control page:** tap to mute/unmute per node, global mute, set volume per node
  - **Settings:** optional (thresholds, sensitivity, etc.)
- Auto-refresh via JavaScript polling (every ~2s)
- Responsive design (works on phone, tablet, desktop)

### Task 4.3 — Hub REST API
- `GET /api/nodes` → JSON list of all nodes + stats
- `POST /api/node/{id}/mute` → mute a node
- `POST /api/node/{id}/unmute` → unmute a node
- `POST /api/node/{id}/volume?level=50` → set volume (0-100)
- `POST /api/global/mute` → mute all nodes
- `POST /api/global/unmute` → unmute all nodes
- Hub translates HTTP commands into ESP-NOW messages to target nodes

### Task 4.4 — Mesh Nodes: Receive Commands
- All nodes parse incoming ESP-NOW messages from the Hub
- Respond to mute, unmute, volume commands
- Broadcast status updates (battery, uptime) on request or periodically

### Task 4.5 — 3D-Printed Enclosure Design
- Design enclosure with two chambers:
  - Mic chamber (isolated, foam-mounted, small port to room)
  - Transducer chamber (firmly coupled to surface)
- Physical isolation between chambers
- Battery compartment accessible
- Ventilation for MAX98357A heatsink

### Task 4.6 — Enclosure Assembly + Testing
- 3D print prototype enclosures
- Assemble electronics inside
- Test masking effectiveness with enclosure
- Verify isolation is sufficient (no vibration pick-up)
- Long-duration test (8h+ continuous)

**Deliverable:** Full system: autonomous masking mesh + controlled from any browser + enclosed in a 3D-printed case.
