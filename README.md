# Privacy Shield

## Description

Privacy Shield is an IoT hardware ecosystem designed to be attached to doors, windows, and other surfaces to mask ambient room noise. The primary objective is to obfuscate private conversations and activities inside a room, preventing eavesdropping or disturbance from the outside. This is achieved using one or more localized devices that capture ambient sound, detect conversations (or specific noise profiles like power tools), and dynamically emit an optimized masking signal (such as Pink or Brown noise) via a surface-mounted transducer.

In a multi-device mesh network, a **Leader** node is dynamically elected based on which device detects the highest noise intensity. The Leader classifies the sound type and, if it matches a target profile, coordinates the **Follower** nodes to emit a synchronized masking audio signal. 

The ecosystem includes a cloud backend and a web application, allowing users to configure target noise profiles, monitor network health (e.g., battery levels, node reachability), and view system analytics.

## Development Roadmap

We have structured the development into three iterative sprints to progressively build and enhance the system:

**Sprint 1: Core Implementation & Static Topology**
- Establish a static Master/Slave network topology.
- Implement Voice Activity Detection (VAD) on the Master node.
- Enable basic Master-to-Slave command routing.
- Implement the **Adaptive Masking Algorithm**: The system plays a targeted masking sound, scaling the output volume proportionally to the perceived input noise.
- Implement **Acoustic Echo Cancellation (AEC)** on all devices. This computationally subtracts the device's own emitted audio from the microphone's input stream to prevent infinite positive feedback loops.
- Implement client acknowledgment (ACK) mechanisms.

**Sprint 2: Cloud Integration & Dynamic Networking**
- Integrate Cloud/MQTT functionalities: Devices report telemetry data (battery life, connection strength, uptime, etc.).
- The Master node handles all outbound cloud communication.
- **Cloud Telemetry:** Devices send specific operational data to the cloud for historical tracking, including precise timestamps and the current speech/noise activation states.
- Implement a Dynamic Master/Slave election algorithm (detailed below).
- Develop a Web Application for remote control (e.g., muting individual followers) and statistical monitoring.

**Sprint 3: Advanced Audio & Hardware Finalization**
- Upgrade the Adaptive Masking Algorithm to dynamically select and play different types of masking sounds based on the specific acoustic environment.
- **Acoustic Profile Classification:** Implement Machine Learning models to recognize the specific audio signatures of power tools and working machinery, allowing the system to deploy targeted masking sounds for those specific disturbances.
- Design and 3D print custom enclosures for the hardware, ensuring proper physical acoustic isolation between the microphone and the transducer.

## Technical Stack & Algorithms

**Firmware & Protocols:**
- **OS:** FreeRTOS
- **Audio Processing:** Voice Activity Detection (VAD), Acoustic Echo Cancellation (AEC), Machine Learning Audio Classification
- **Mesh Communication:** ESP-NOW (for low-latency device-to-device communication)
- **Cloud Communication:** MQTT 

**Dynamic Master-Slave Election Algorithm:**
- The node detecting the strongest sound broadcasts a claim to become the Master.
- **Hysteresis & Thresholds:** To prevent network thrashing, the current Master retains its role until the ambient noise it detects drops below a predefined baseline threshold for a sustained period, OR until another node reports a noise level that is significantly higher (e.g., +20%).
- The algorithm evaluates the network state periodically.
- Message structures include: `Setup`, `Command`, and `ACK`.
- Payloads include Device IDs and role flags (Master/Slave).

## Hardware Components

- 2x ESP32-S3 Microcontrollers *(Recommended: Variants with at least 8MB PSRAM to handle audio buffering and ML models)*
- 2x I2S Microphones
- 2x Dayton Audio DAEX25 Sound Exciters (Transducers): [Link](https://amzn.eu/d/07JOvqCm)
- 2x MAX98357A I2S Class-D Amplifiers: [Link](https://amzn.eu/d/0508CoDR)
- 4x 18650 Batteries
- 1x 18650 Battery Shield V3: [Link](https://amzn.eu/d/0iAi3bPU)
- 2x Breadboards & Connectors
- 22 AWG Speaker Wire: [Link](https://amzn.eu/d/0dPKMZ0M)

## Inspiration & Related Work

### DNG-2300

The DNG-2300 generator is a commercial unit created to protect against listening devices that cannot be discovered by common methods. The unit protects a room by inducing non-filterable noise onto surfaces using TRN-2000 transducers and OMS-2000 speakers. 

<img src="assets/DNG_2300_illustration.png" alt="DNG-2300 Illustration" />