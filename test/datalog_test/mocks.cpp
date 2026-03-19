// =============================================================
// Mock-Implementierungen für Datalog-Test
// Liefern simulierte Werte, damit datalog.cpp ohne Hotend/Motor/
// Wägezellen-Hardware kompiliert und läuft.
// =============================================================

#include <Arduino.h>
#include <math.h>

// ── Hotend-Mock ───────────────────────────────────────────────
// Linear steigende Temperatur: 25 °C → 200 °C über 60 s
float hotend_get_temperature() {
    static float s_temp = 25.0f;
    static unsigned long s_last = 0;
    unsigned long now = millis();
    if (now - s_last >= 1000) {
        s_last = now;
        s_temp += 0.5f;
        if (s_temp > 200.0f) s_temp = 25.0f;
    }
    return s_temp;
}

float hotend_get_target()  { return 0.0f; }
float hotend_get_duty()    { return 0.0f; }
bool  hotend_has_fault()   { return false; }

// ── Wägezellen-Mock ───────────────────────────────────────────
// Sinus-Gewicht: 0–500 g mit 10-s-Periode
float load_cell_get_weight_g() {
    float t = (float)(millis()) / 10000.0f;   // 10-s-Periode
    return 250.0f + 250.0f * sinf(2.0f * (float)M_PI * t);
}

// ── Motor-Mock ────────────────────────────────────────────────
// Schaltet alle 20 s zwischen Fahren (3 mm/s) und Stillstand
bool motor_is_moving() {
    return (millis() / 20000UL) % 2 == 1;
}

float motor_get_current_speed() {
    return motor_is_moving() ? 3.0f : 0.0f;
}
