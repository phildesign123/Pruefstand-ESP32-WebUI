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
