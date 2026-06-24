#ifndef BATTERY_H
#define BATTERY_H

#define BATT_ADC_UNIT   ADC_UNIT_1
#define BATT_ADC_CHAN   ADC_CHANNEL_0

// Possible battery states
typedef enum {
    BATT_CHARGING,    // Charging
    BATT_FULL,        // Charge complete
    BATT_DISCHARGING, // Running on battery
    BATT_ERROR        // Overheating or battery failure
} batt_status_t;

// Initializes GPIO pins and ADC unit
void battery_init(void);

// Returns the current charging status
batt_status_t battery_get_status(void);

// Returns the estimated battery percentage
int battery_get_percentage(void);

// Returns the real battery voltage
float battery_get_voltage(void);

void battery_logger_task(void *pvParameters);

#endif