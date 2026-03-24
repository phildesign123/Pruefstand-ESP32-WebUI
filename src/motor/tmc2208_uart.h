#pragma once
#include <Arduino.h>
#include "freertos/semphr.h"

struct TMC2208Status {
    bool  ot_warning;
    bool  ot_shutdown;
    bool  short_a;
    bool  short_b;
    bool  open_load_a;
    bool  open_load_b;
    bool  stealthchop_active;
    bool  standstill;
    uint8_t cs_actual;
};

// Treiber initialisieren
bool tmc2208_init(int tx_pin, int rx_pin, SemaphoreHandle_t uart_mutex);

// Register lesen / schreiben
bool tmc2208_write_reg(uint8_t reg, uint32_t value);
bool tmc2208_read_reg(uint8_t reg, uint32_t *value);

// Konfiguration (verifiziert nach Schreiben)
bool tmc2208_set_current(uint16_t run_ma, uint16_t hold_ma);
bool tmc2208_set_microstep(uint8_t ms);
bool tmc2208_set_stealthchop(bool enable);
bool tmc2208_set_interpolation(bool enable);
bool tmc2208_read_status(TMC2208Status *status);

struct TMC2208Config {
    uint16_t run_current_ma;
    uint16_t hold_current_ma;
    uint16_t microsteps;
    bool     stealthchop;
    bool     interpolation;
};
bool tmc2208_read_config(TMC2208Config *cfg);

// Kommunikationstest
bool tmc2208_ping();
