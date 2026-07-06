#pragma once

/* -------------------------------------------------------------------------- */
/*  I2S Microphone (MEMS) pins                                              */
/* -------------------------------------------------------------------------- */

#define PIN_I2S_MIC_LRCLK   GPIO_NUM_15    /* L/R clock (word select) / LRCL  / WS */
#define PIN_I2S_MIC_DIN     GPIO_NUM_16    /* Data in (from mic) / DIN / SD */
#define PIN_I2S_MIC_BCLK    GPIO_NUM_17    /* Bit clock / BCLK / SCK */

/* -------------------------------------------------------------------------- */
/*  I2S Amplifier (MAX98357A) pins                                          */
/* -------------------------------------------------------------------------- */
#define PIN_AMP_LRCLK       GPIO_NUM_4   /* Left-right clock (word select) */
#define PIN_AMP_BCLK        GPIO_NUM_5   /* Bit clock */
#define PIN_AMP_DOUT        GPIO_NUM_6   /* Data out (to amp) */
#define PIN_AMP_SD          GPIO_NUM_18   /* Shutdown pin (active low — pull high to enable) */

/* -------------------------------------------------------------------------- */
/*  Status LED                                                               */
/* -------------------------------------------------------------------------- */
#define PIN_LED             GPIO_NUM_48   /* Built-in RGB LED (or GPIO for external) */

/* -------------------------------------------------------------------------- */
/*  Node defaults                                                            */
/* -------------------------------------------------------------------------- */
// Remove this
//#define DEFAULT_NODE_ID     1    /* Change per device (1–254). 0 = hub. */

/* -------------------------------------------------------------------------- */
/*  Battery pins                                                              */
/* -------------------------------------------------------------------------- */

#define PIN_BATT_STAT1  GPIO_NUM_42
#define PIN_BATT_STAT2  GPIO_NUM_41 
#define PIN_BATT_ADC    GPIO_NUM_1
