#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "freertos/semphr.h"

// Sensor initialisieren (SPI + erste blockierende Messung)
void    setup_sensor(SPIClass &spi, SemaphoreHandle_t spi_mutex);

// Nicht-blockierende State-Machine – bei JEDEM Loop aufrufen
float   read_temperature();

// Fault-Status
bool    sensor_has_fault();
uint8_t sensor_get_fault_code();
void    sensor_clear_fault();

// Rohwerte aus letzter Messung (für Diagnose/Kalibrierung)
uint16_t sensor_get_raw_rtd();     // 15-Bit Rohwert aus MAX31865 RTD-Register
float    sensor_get_resistance();  // Berechneter RTD-Widerstand [Ω]
