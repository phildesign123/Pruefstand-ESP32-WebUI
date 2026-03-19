#include "safety.h"

// =============================================================
// Safety: MAXTEMP, MINTEMP, Thermal Runaway (Aufheiz + Halte),
//         Temperatursprung, Sensor-Fault
// =============================================================

static float         s_prev_temp       = -1.0f;

// Thermal Runaway – Aufheizphase
static bool          s_watching        = false;
static unsigned long s_watch_start_ms  = 0;
static float         s_watch_start_temp = 0.0f;

// Thermal Runaway – Haltephase
static bool          s_hold_active     = false;
static bool          s_hold_fault_armed = false;
static unsigned long s_hold_drop_ms    = 0;

SafetyFault safety_check(float current_temp, float target_temp, uint8_t heater_output) {
    // ── 1. MAXTEMP ────────────────────────────────────────────
    if (current_temp >= TEMP_MAX) return FAULT_MAX_TEMP;

    // ── 2. MINTEMP (Sensor abgerissen → meldet 0°C) ──────────
    if (target_temp > 0.0f && current_temp < TEMP_MIN) return FAULT_MIN_TEMP;

    // ── 3. Temperatursprung ───────────────────────────────────
    if (s_prev_temp >= 0.0f) {
        if (fabsf(current_temp - s_prev_temp) > TEMP_MAX_JUMP) return FAULT_TEMP_JUMP;
    }
    s_prev_temp = current_temp;

    // ── 4. Thermal Runaway – Aufheizphase ─────────────────────
    // Aktiv wenn Ziel > 0 und Temperatur noch >5°C unter Ziel
    bool heating_phase = (target_temp > 0.0f)
                      && ((target_temp - current_temp) > THERMAL_HOLD_WINDOW)
                      && (heater_output >= THERMAL_RUNAWAY_MIN_OUTPUT);

    if (heating_phase && !s_watching) {
        s_watching          = true;
        s_watch_start_ms    = millis();
        s_watch_start_temp  = current_temp;
    } else if (!heating_phase) {
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

    // ── 5. Thermal Runaway – Haltephase ───────────────────────
    // Aktiv wenn Zieltemperatur erreicht (innerhalb THERMAL_HOLD_WINDOW)
    bool hold_phase = (target_temp > 0.0f)
                   && ((target_temp - current_temp) <= THERMAL_HOLD_WINDOW);

    if (hold_phase && !s_hold_active) {
        // Haltephase beginnt
        s_hold_active      = true;
        s_hold_fault_armed = false;
    } else if (!hold_phase && target_temp > 0.0f) {
        // Noch nicht am Ziel → Haltephase aus
        s_hold_active      = false;
        s_hold_fault_armed = false;
    }

    if (s_hold_active) {
        float drop = target_temp - current_temp;
        if (drop > THERMAL_HOLD_DROP) {
            // Temperatur zu weit abgefallen
            if (!s_hold_fault_armed) {
                s_hold_fault_armed = true;
                s_hold_drop_ms     = millis();
            } else if (millis() - s_hold_drop_ms >= THERMAL_HOLD_PERIOD_MS) {
                // 30 s ohne Erholung → Fault
                return FAULT_THERMAL_RUNAWAY;
            }
        } else {
            // Temperatur hat sich erholt
            s_hold_fault_armed = false;
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
        case FAULT_MIN_TEMP:        return "MINTEMP – Sensor defekt oder abgerissen";
        default:                    return "UNBEKANNTER FEHLER";
    }
}

void safety_reset() {
    s_prev_temp        = -1.0f;
    s_watching         = false;
    s_hold_active      = false;
    s_hold_fault_armed = false;
}
