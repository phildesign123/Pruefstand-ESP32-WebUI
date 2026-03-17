#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "freertos/semphr.h"

// NAU7802 Register-Adressen
#define NAU7802_REG_PU_CTRL  0x00
#define NAU7802_REG_CTRL1    0x01
#define NAU7802_REG_CTRL2    0x02
#define NAU7802_REG_ADC      0x15
#define NAU7802_REG_ADC_DATA 0x12   // 3 Byte: MSB bei 0x12

// PU_CTRL Bits
#define PU_CTRL_RR    (1 << 0)   // Register Reset
#define PU_CTRL_PUD   (1 << 1)   // Power-Up Digital
#define PU_CTRL_PUA   (1 << 2)   // Power-Up Analog
#define PU_CTRL_PUR   (1 << 3)   // Power-Up Ready (read-only)
#define PU_CTRL_CS    (1 << 4)   // Cycle Start
#define PU_CTRL_CR    (1 << 5)   // Conversion Ready (read-only)
#define PU_CTRL_OSCS  (1 << 6)   // Oscillator Select
#define PU_CTRL_AVDDS (1 << 7)   // AVDD Source Select

// CTRL2 Bits: Conversion Rate [6:4]
#define CRS_10SPS   (0b000 << 4)
#define CRS_20SPS   (0b001 << 4)
#define CRS_40SPS   (0b010 << 4)
#define CRS_80SPS   (0b011 << 4)
#define CRS_320SPS  (0b111 << 4)
#define CALS_BIT    (1 << 2)     // Kalibrierung starten
#define CAL_ERR_BIT (1 << 3)     // Kalibrierfehler

// CTRL1 Gains
#define GAIN_128 (0b111)
#define GAIN_64  (0b110)
#define GAIN_32  (0b101)
#define GAIN_16  (0b100)
#define GAIN_8   (0b011)
#define GAIN_4   (0b010)
#define GAIN_2   (0b001)
#define GAIN_1   (0b000)

bool    nau7802_init(TwoWire &wire, SemaphoreHandle_t i2c_mutex);
int32_t nau7802_read_raw(TwoWire &wire, SemaphoreHandle_t i2c_mutex);
bool    nau7802_is_ready(TwoWire &wire, SemaphoreHandle_t i2c_mutex);
