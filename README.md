# PrivacyShield

## Description

The project consists of a device that can be attached to doors, windows, and other surfaces to mask the noise of a room. The primary focus is to cover conversations inside the room to prevent people outside from hearing them. This goal is accomplished by one or more devices that capture the sound, recognize if there is a conversation or other types of noise, and decide to cover it with the most appropriate sound emitted by a transducer attached to the door or window.

In a multi-device network, a Leader is elected based on which node detects the highest noise intensity. The Leader identifies the sound type, if it matches a specific target profile, it coordinates the Followers to emit a synchronized anti-noise signal to mask the disturbance.

The ecosystem includes a mobile application where it will be possible to set the type of noise to cover and view different statistics.

## Hardware list:
- ESP32-S3 Microcontroller (x2)
- Breadboard (x2)
- Connectors
- Battery (x4)
- 18650 Battery Shield V3: https://amzn.eu/d/0iAi3bPU
- I2S microphone (x2)
- Dayton Audio DAEX25: https://amzn.eu/d/07JOvqCm
- MAX98357A: https://amzn.eu/d/0508CoDR
- 22 AWG Speaker Wire: https://amzn.eu/d/0dPKMZ0M

## Other works

### DNG-2300

The DNG-2300 generator has been created to protect against listening devices
which cannot be discovered by common methods. The unit protects a room by
inducting non-filterable noise onto surfaces. This noise, also known as 'white'
noise, is transmitted onto surfaces with the help of the TRN-2000 transducers
and OMS-2000 speakers in the unit. 

<img src="assets/DNG_2300_illustration.png" />
