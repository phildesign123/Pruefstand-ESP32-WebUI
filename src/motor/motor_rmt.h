#pragma once
#include <Arduino.h>
#include "freertos/semphr.h"

// RMT initialisieren (STEP-Pin, 1 MHz Clock)
bool motor_rmt_init(int step_pin);

// Bewegung starten: N Steps bei freq_hz
// Nicht-blockierend. Gibt false wenn Kanal noch belegt.
bool motor_rmt_start(uint32_t steps, uint32_t freq_hz);

// Laufende Bewegung sofort abbrechen
void motor_rmt_stop();

// Warten bis Bewegung abgeschlossen (blockierend)
bool motor_rmt_wait(uint32_t timeout_ms);

// True wenn Bewegung läuft
bool motor_rmt_is_running();
