# Key Performance Indicators

This is the page for Key Performance Indicators, that will be evaluated 

## Sound Intake based Indicators
Under this header are the Key Performance Indicators for taking in sound (Microphone).

### Signal-to-Noise Ratio (SNR)

Measures how dominant speech is relative to masking noise.

$$\Large
\text{SNR}_{dB}=10\log_{10}\left(\frac{P_{speech}}{P_{noise}}\right)
$$

or using amplitudes:

$$\Large
\text{SNR}_{dB}=20\log_{10}\left(\frac{A_{speech}}{A_{noise}}\right)
$$

Where:
- $P_{speech}$ = speech power
- $P_{noise}$ = masking noise power
- $A_{speech}$ = speech amplitude
- $A_{noise}$ = masking noise amplitude

Lower SNR implies better masking performance.

### Speech Privacy Index (SPI)

Simplified approximation:

$$\Large
\text{SPI}=1-\text{Speech Intelligibility}
$$

Using Speech Intelligibility Index (SII):

$$\Large
\text{SPI}=1-\sum_{i=1}^{n} B_iA_i
$$

Where:
- $B_i$ = importance of frequency band
- $A_i$ = audibility in frequency band

Higher SPI indicates greater speech privacy.

### Sound Pressure Level (SPL)

Measures sound loudness in decibels.

$$\Large
L_p=20\log_{10}\left(\frac{p}{p_0}\right)
$$

Where:
- $p$ = measured sound pressure
- $p_0=20\mu Pa$ = reference pressure

### Root Mean Square (RMS) Amplitude
Measures microphone signal intensity.

$$\Large
A_{RMS}=\sqrt{\frac{1}{N}\sum_{i=1}^{N}x_i^2}
$$

Where:
- $x_i$ = audio sample
- $N$ = number of samples

Used for adaptive gain and amplitude tracking.

## Sound Emmiting Indicators
Under this header are the Key Performance Indicators for emmiting sound (Speaker - Transducer).

### Masking Efficiency

Measures effectiveness of intelligibility reduction.

$$\Large
\text{Masking Efficiency}=
\frac{I_{before}-I_{after}}{I_{before}}
$$

Where:
- $I_{before}$ = speech intelligibility before masking
- $I_{after}$ = speech intelligibility after masking

Higher values indicate more effective sound masking.

### White Noise Spectrum
To measure White Noise Quality  
#### Spectral Flatness Measure (SFM)  
  
A simple KPI for evaluating white noise is the Spectral Flatness Measure.  
  
$$\Large  
SFM=  
\frac{  
\left(  
\prod_{i=1}^{N} P_i  
\right)^{1/N}  
}{  
\frac{1}{N}\sum_{i=1}^{N}P_i  
}  
$$  
  
Where:  
- $P_i$ = power at frequency bin $i$  
- $N$ = number of frequency bins  
  
---  
  
#### Interpretation  
  
| SFM Value   | Meaning                          |
| ----------- | -------------------------------- |
| $\approx 1$ | Good white noise (flat spectrum) |
| $\approx 0$ | Uneven or tonal signal           |
  
Higher SFM values indicate:  
- more uniform frequency distribution,  
- better white noise quality,  
- stronger spectral flatness.

### Pink Noise Spectrum

Ideal pink noise distribution:

$$\Large
P(f)\propto\frac{1}{f}
$$

Equal energy exists per octave.

### Brown Noise Spectrum

Ideal brown noise distribution:

$$\Large
P(f)\propto\frac{1}{f^2}
$$

Emphasizes lower frequencies more strongly.

### Mean Square Error
Compares generated spectrum to ideal spectrum.

$$\Large
MSE=\frac{1}{N}\sum_{i=1}^{N}(y_i-\hat{y}_i)^2
$$

Where:
- $y_i$ = ideal spectrum
- $\hat{y}_i$ = generated spectrum

Lower MSE indicates more accurate noise generation.

### Power Spectral Density (PSD)

Measures frequency-domain energy distribution.

$$\Large
P(f)=\frac{|X(f)|^2}{N}
$$

Where:
- $X(f)$ = Fourier transform of signal
- $N$ = signal length

Used to validate generated noise spectra.

## General Indicators
Under this header are general Edge Computing and Networking Indicators.

### System Latency

Measures total input-to-output delay.

$$\Large
\text{Latency}=t_{output}-t_{input}
$$

Where:
- $t_{input}$ = microphone capture time
- $t_{output}$ = speaker playback time

Target latency is typically below $20\,ms$.

### Adaptive Response Time

Measures how quickly the system reacts to sound changes.

$$\Large
T_{response}=t_{stable}-t_{change}
$$

Where:
- $t_{change}$ = time environmental change occurs
- $t_{stable}$ = time masking stabilizes

### Packet Loss Rate

Applicable for distributed IoT systems.

$$\Large
\text{Packet Loss Rate}=
\frac{Packets_{lost}}{Packets_{sent}}\times100
$$

### CPU Utilization

Measures edge device processing load.

$$\Large
CPU\ Usage=
\frac{T_{active}}{T_{total}}\times100
$$

Where:
- $T_{active}$ = processor active time
- $T_{total}$ = total observation time

### Energy Consumption

Measures system energy usage.

$$\Large
E=P\times t
$$

Where:
- $P$ = power consumption
- $t$ = operating time

### Edge Device Uptime

#### Equation

$$\Large
\text{Uptime (\%)}=
\frac{T_{active}}{T_{total}}\times100
$$

Where:
- $T_{active}$ = operational time
- $T_{total}$ = total monitoring period

### Wireless Transmission Reliability

#### Packet Delivery Ratio (PDR)

$$\Large
PDR=
\frac{Packets_{received}}{Packets_{sent}}\times100
$$

Where:
- $Packets_{received}$ = successfully received packets
- $Packets_{sent}$ = total transmitted packets

Higher PDR indicates more reliable communication.

---

#### Packet Loss Rate

$$\Large
\text{Packet Loss (\%)}=
\frac{Packets_{lost}}{Packets_{sent}}\times100
$$

Where:

$$\Large
Packets_{lost}=Packets_{sent}-Packets_{received}
$$

---
