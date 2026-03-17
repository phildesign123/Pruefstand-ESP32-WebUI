#include "safety.h"

// =============================================================
// Identisch mit Marlin-EPS32-Steuerung safety.cpp
// =============================================================

static float         s_prev_temp       = -1.0f;
static bool          s_watching        = false;
static unsigned long s_watch_start_ms  = 0;
static float         s_watch_start_temp = 0.0f;

SafetyFault safety_check(float current_temp, float target_temp, uint8_t heater_output) {
    if (current_temp >= TEMP_MAX) return FAULT_MAX_TEMP;

    if (s_prev_temp >= 0.0f) {
        if (fabsf(current_temp - s_prev_temp) > TEMP_MAX_JUMP) return FAULT_TEMP_JUMP;
    }
    s_prev_temp = current_temp;

    bool should_watch = (target_temp > 0.0f)
                     && ((target_temp - current_temp) > 5.0f)
                     && (heater_output >= THERMAL_RUNAWAY_MIN_OUTPUT);

    if (should_watch && !s_watching) {
        s_watching          = true;
        s_watch_start_ms    = millis();
        s_watch_start_temp  = current_temp;
    } else if (!should_watch) {
        s_watching = false;
    } else if (s_watching) {
        if (millis() - s_watch_start_ms >= THERMAL_RUNAWAY_PERIOD_MS) {
            if (current_temp - s_watch_start_temp < THERMAL_RUNAWAY_HYSTERESIS) {
                return FAULT_THERMAL_RUNAWAY;
            }
            s_watch_start_ms   = millis();
            s_watch_start_temp = current_temp;
        }
    }
    return SAFETY_OK;
}

const char* safety_fault_string(SafetyFault fault) {
    switch (fault) {
        case FAULT_MAX_TEMP:        return "MAXIMALTEMPERATUR UEBERSCHRITTEN";
        case FAULT_THERMAL_RUNAWAY: return "THERMAL RUNAWAY – Sensor oder Heizer defekt";
        case FAULT_TEMP_JUMP:       return "TEMPERATURSENSOR – unplausibler Sprung";
        case FAULT_SENSOR:          return "SENSOR-FAULT – MAX31865 Fehler";
        default:                    return "UNBEKANNTER FEHLER";
    }
}

void safety_reset() {
    s_prev_temp  = -1.0f;
    s_watching   = false;
}
