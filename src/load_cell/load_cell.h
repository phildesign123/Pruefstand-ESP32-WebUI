#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "freertos/semphr.h"

// Modul initialisieren (I2C, DRDY-ISR, Task)
bool load_cell_init(TwoWire &wire, SemaphoreHandle_t i2c_mutex);

// Tarierung (blockiert ~1 s)
bool load_cell_tare();

// Kalibrierung mit bekanntem Referenzgewicht (blockiert ~1 s)
bool load_cell_calibrate(float known_weight_g);

// Aktuelles Gewicht (gefiltert, tariert, kalibriert) in Gramm
float   load_cell_get_weight_g();

// Gefilterter Rohwert (ohne Tarierung)
int32_t load_cell_get_raw();

// Kalibrierung vorhanden?
bool load_cell_is_calibrated();

// Modul stoppen
void load_cell_deinit();
