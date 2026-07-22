# Battery Management — Implementation Guide

> **Component:** `battery`  
> **ESP-IDF:** v5.5.4  

## Overview

The `battery` component is responsible for monitoring the power state of the device. It interfaces with an MCP73833 charge controller to determine the charging status, reads the raw battery voltage using the ESP32's Analog-to-Digital Converter (ADC), and calculates a smoothed battery percentage.

---

## Hardware Interfacing

The subsystem interacts with two primary hardware elements:

1.  **Charge Controller (MCP73833):** Provides digital charging status via two pins (`STAT1` and `STAT2`).
2.  **Voltage Divider:** The battery voltage (up to 4.2V) exceeds the ESP32 ADC's maximum measurable voltage (~3.3V). A hardware voltage divider using two equal resistors (R1 = R2) halves the voltage before it reaches the ADC pin. 

---

## API Reference

### Initialization

`void battery_init(void);`

Initializes the GPIO and ADC peripherals.
* Configures `PIN_BATT_STAT1` and `PIN_BATT_STAT2` as inputs.
* Enables internal pull-up resistors on both STAT pins, which is crucial for reading the MCP73833 outputs correctly.
* Configures the ADC unit (`BATT_ADC_UNIT`) and channel (`BATT_ADC_CHAN`) with `ADC_ATTEN_DB_12` to allow readings up to 3.3V.

### Status Detection

`batt_status_t battery_get_status(void);`

Returns the current state of the battery based on the MCP73833 truth table and voltage thresholds. If the voltage drops below 2.5V, it returns `BATT_DISCONNECTED`. 

**MCP73833 Truth Table Mapping:**

| STAT1 | STAT2 | Returned Status | Condition |
|---|---|---|---|
| 0 | 1 | `BATT_CHARGING` | Actively charging |
| 1 | 0 | `BATT_FULL` | Charge complete |
| 0 | 0 | `BATT_CHARGING` | Handled as charging (unexpected hardware state) |
| 1 | 1 | `BATT_DISCHARGING` | USB unplugged |

### Voltage Calculation

`float battery_get_voltage(void);`

Reads the raw ADC value, converts it to pin voltage, and multiplies it to recover the true battery voltage. 

**Voltage Conversion Formula:**

The ADC reads the midpoint of the voltage divider. The calculation to recover the real voltage is:

$$
V_{real} = V_{pin} \times \frac{R_1 + R_2}{R_2}
$$

Because $R_1 = R_2$ in this hardware design, the multiplier is exactly 2.0:

$$
V_{real} = \left(\frac{ADC_{raw}}{4095.0}\right) \times 3.3 \times 2.0
$$

### Percentage Calculation

`int battery_get_percentage(void);`

Returns a smoothed battery percentage from 0% to 100%. 

**Linear Mapping:**
The system maps the voltage linearly between a minimum empty threshold (3.2V) and a maximum full threshold (4.2V). Values are hard-capped to not exceed 100% or drop below 0%.

$$
\text{Percentage} = \frac{V_{real} - 3.2}{4.2 - 3.2} \times 100
$$

**Smoothing Window:**
To prevent percentage jitter, the function maintains a rolling average using a window array (`percentage_window`) of 60 samples (`WINDOW_SIZE`). The function calculates and returns the mean of these 60 samples.

---

## FreeRTOS Tasks

### `battery_logger_task`

`void battery_logger_task(void *pvParameters);`

An infinite loop task meant for monitoring and debugging. 

* **Period:** Runs every 1000 milliseconds (1 second).
* **Behavior:** Calls `battery_get_status()`, `battery_get_voltage()`, and `battery_get_percentage()`.
* **Logging:** Outputs a formatted string to the console using `ESP_LOGI` under the `LOG_TAG_BATTERY` tag.

**Expected Serial Output:**
```text
I (3102) battery: Status: DISCHARGING 🔋 | Voltage: 3.85V | Level: 65%
I (4102) battery: Status: CHARGING ⚡ | Voltage: 4.10V | Level: 90%
I (5102) battery: Status: FULL ✅ | Voltage: 4.20V | Level: 100%