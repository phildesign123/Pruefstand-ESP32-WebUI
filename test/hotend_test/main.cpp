// =============================================================
// Hotend-Test: MAX31865, Heizer, Lüfter, PID-Response (200 °C)
// Serielle Befehle (115200 Baud):
//   T       – Alle 4 Tests ausführen
//   T1…T4   – Einzelnen Test ausführen
//   S<temp> – Zieltemperatur setzen (S0 = AUS)
//   F<duty> – Lüfter manuell (0–255, F0 = Auto)
//   R       – Fault reset
//   status  – Aktuellen Status ausgeben
//   help    – Befehlsübersicht
// =============================================================

#include <Arduino.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "hotend/hotend.h"
#include "hotend/heater.h"
#include "hotend/fan.h"
#include "hotend/safety.h"

static SPIClass vspi(VSPI);
static SemaphoreHandle_t spi_mutex;

// ── Test-Ergebnis ─────────────────────────────────────────────
struct TestResult {
    const char *name;
    bool        passed;
    char        message[128];
};

// ── Test 1: Sensor ────────────────────────────────────────────
// Prüft MAX31865-Kommunikation und plausible Raumtemperatur
static TestResult test_sensor() {
    TestResult r = {"sensor", false, ""};

    // State-Machine 500 ms laufen lassen, zwei Messwerte sammeln
    float first = -999.0f, second = -999.0f;
    unsigned long t0 = millis();
    while (millis() - t0 < 600) {
        float t = hotend_get_temperature();
        if (first == -999.0f && t != 0.0f) first = t;
        else if (first != -999.0f && second == -999.0f) second = t;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (hotend_has_fault()) {
        snprintf(r.message, sizeof(r.message),
                 "Sensor-Fault: %s", hotend_get_fault_string());
        return r;
    }

    float temp = hotend_get_temperature();
    if (temp < -10.0f || temp > 50.0f) {
        snprintf(r.message, sizeof(r.message),
                 "Temperatur außerhalb Bereich: %.1f °C", temp);
        return r;
    }

    if (first != -999.0f && second != -999.0f && fabsf(first - second) >= 2.0f) {
        snprintf(r.message, sizeof(r.message),
                 "Unplausible Schwankung: %.1f → %.1f °C", first, second);
        return r;
    }

    r.passed = true;
    snprintf(r.message, sizeof(r.message), "%.1f °C, kein Fault", temp);
    return r;
}

// ── Test 2: Heizer ────────────────────────────────────────────
// Prüft Temperaturrrise durch kurzes Anheizen (max 3 s)
static TestResult test_heater() {
    TestResult r = {"heater", false, ""};

    float start_temp = hotend_get_temperature();
    if (start_temp >= 50.0f) {
        r.passed = true;
        snprintf(r.message, sizeof(r.message),
                 "Übersprungen – Hotend bereits warm (%.1f °C)", start_temp);
        return r;
    }

    // Ziel deutlich über Raumtemperatur setzen → PID heizt maximal
    float target = start_temp + 40.0f;
    if (target >= TEMP_MAX) target = TEMP_MAX - 1.0f;
    hotend_set_target(target);

    unsigned long t0 = millis();
    float peak = start_temp;
    while (millis() - t0 < 15000) {
        float t = hotend_get_temperature();
        if (t > peak) peak = t;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    hotend_set_target(0.0f);   // Heizer aus

    float rise = peak - start_temp;
    if (rise < 1.0f) {
        snprintf(r.message, sizeof(r.message),
                 "Kein Anstieg: Δ=%.1f °C (%.1f→%.1f) – MOSFET/Kabel prüfen",
                 rise, start_temp, peak);
        return r;
    }

    r.passed = true;
    snprintf(r.message, sizeof(r.message),
             "+%.1f °C Anstieg in 3 s (%.1f → %.1f °C)", rise, start_temp, peak);
    return r;
}

// ── Test 3: Lüfter ────────────────────────────────────────────
// Rein visuell/akustisch – kein Tacho vorhanden
static TestResult test_fan() {
    TestResult r = {"fan", true, ""};
    set_fan_override(255);
    vTaskDelay(pdMS_TO_TICKS(10000));
    set_fan_override(0);   // Zurück auf Auto-Modus
    snprintf(r.message, sizeof(r.message),
             "Lüfter 10 s auf 100 %% – bitte visuell/akustisch prüfen");
    return r;
}

// ── Test 4: PID-Response ──────────────────────────────────────
// Prüft, ob PID-Regler Hotend auf 200 °C aufheizen kann (max 600 s)
static TestResult test_pid_response() {
    TestResult r = {"pid_response", false, ""};

    hotend_set_target(200.0f);

    unsigned long t_start   = millis();
    unsigned long t_stable  = 0;
    bool          reached   = false;

    while (millis() - t_start < 600000UL) {
        if (hotend_has_fault()) {
            snprintf(r.message, sizeof(r.message),
                     "Safety-Fault: %s", hotend_get_fault_string());
            hotend_set_target(0.0f);
            return r;
        }

        float temp = hotend_get_temperature();
        bool in_range = fabsf(temp - 200.0f) <= 2.0f;

        if (in_range) {
            if (!reached) { reached = true; t_stable = millis(); }
            if (millis() - t_stable >= 3000UL) break;   // 3 s stabil
        } else {
            reached = false;
        }

        // Fortschritt jede Sekunde ausgeben
        unsigned long elapsed = millis() - t_start;
        if (elapsed % 1000 < 200) {
            Serial.printf("[TEST] pid_response: %.1f / 200.0 °C  (%lu s)\n",
                          temp, elapsed / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    float final_temp = hotend_get_temperature();
    hotend_set_target(0.0f);

    if (!reached) {
        snprintf(r.message, sizeof(r.message),
                 "Timeout – %.1f °C (Soll 200.0 °C) – PID/Verkabelung prüfen",
                 final_temp);
        return r;
    }

    r.passed = true;
    snprintf(r.message, sizeof(r.message),
             "200.0 °C erreicht in %.0f s, stabil (%.1f °C)",
             (float)(t_stable - t_start) / 1000.0f, final_temp);
    return r;
}

// ── Alle Tests sequenziell ausführen ─────────────────────────
static void run_all_tests() {
    Serial.println("\n=== HARDWARE TESTS ===");

    TestResult results[4];
    results[0] = test_sensor();
    results[1] = test_heater();
    results[2] = test_fan();
    results[3] = test_pid_response();

    int passed = 0;
    for (int i = 0; i < 4; i++) {
        Serial.printf("[%s] %-16s — %s\n",
                      results[i].passed ? "PASS" : "FAIL",
                      results[i].name,
                      results[i].message);
        if (results[i].passed) passed++;
    }
    Serial.printf("=== %d/4 TESTS BESTANDEN ===\n\n", passed);
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Hotend-Test (Prüfstand ESP32) ===");

    spi_mutex = xSemaphoreCreateMutex();
    vspi.begin(MAX_CLK, MAX_MISO, MAX_MOSI, MAX_CS);

    hotend_init(vspi, spi_mutex);
    delay(300);

    Serial.println("Bereit. Befehle:");
    Serial.println("  T       – Alle 4 Tests ausführen");
    Serial.println("  T1…T4   – Einzelnen Test (1=Sensor 2=Heizer 3=Lüfter 4=PID)");
    Serial.println("  S<temp> – Zieltemperatur setzen (S0 = AUS)");
    Serial.println("  F<duty> – Lüfter manuell 0–255 (F0 = Auto)");
    Serial.println("  R       – Fault reset");
    Serial.println("  status  – Aktueller Zustand");
    Serial.println("  help    – Diese Übersicht");
}

// ── Loop ──────────────────────────────────────────────────────
static unsigned long s_last_status = 0;

void loop() {
    // Jede Sekunde automatisch Status auf Serial ausgeben
    if (millis() - s_last_status >= 1000) {
        s_last_status = millis();
        Serial.printf("[HOTEND] T=%.1f / %.1f °C  Duty=%.0f%%  Fan=%d",
                      hotend_get_temperature(), hotend_get_target(),
                      hotend_get_duty() * 100.0f, hotend_get_fan_duty());
        if (hotend_has_fault())
            Serial.printf("  !! FAULT: %s !!", hotend_get_fault_string());
        Serial.println();
    }

    if (!Serial.available()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("> %s\n", cmd.c_str());

    if (cmd.equalsIgnoreCase("T") || cmd.equalsIgnoreCase("T0")) {
        run_all_tests();
    } else if (cmd.equalsIgnoreCase("T1")) {
        TestResult r = test_sensor();
        Serial.printf("[%s] sensor — %s\n", r.passed ? "PASS" : "FAIL", r.message);
    } else if (cmd.equalsIgnoreCase("T2")) {
        TestResult r = test_heater();
        Serial.printf("[%s] heater — %s\n", r.passed ? "PASS" : "FAIL", r.message);
    } else if (cmd.equalsIgnoreCase("T3")) {
        TestResult r = test_fan();
        Serial.printf("[%s] fan — %s\n", r.passed ? "PASS" : "FAIL", r.message);
    } else if (cmd.equalsIgnoreCase("T4")) {
        TestResult r = test_pid_response();
        Serial.printf("[%s] pid_response — %s\n", r.passed ? "PASS" : "FAIL", r.message);
    } else if (cmd.startsWith("S") || cmd.startsWith("s")) {
        float t = cmd.substring(1).toFloat();
        if (hotend_set_target(t))
            Serial.printf("[CMD] Zieltemperatur: %.1f °C\n", t);
        else
            Serial.printf("[CMD] Abgelehnt: %.1f °C >= %.0f °C (TEMP_MAX)\n", t, TEMP_MAX);
    } else if (cmd.startsWith("F") || cmd.startsWith("f")) {
        uint8_t duty = (uint8_t)cmd.substring(1).toInt();
        hotend_set_fan(duty);
        Serial.printf("[CMD] Lüfter: %d %s\n", duty, duty == 0 ? "(Auto)" : "");
    } else if (cmd.equalsIgnoreCase("R") || cmd.equalsIgnoreCase("reset")) {
        hotend_clear_fault();
        Serial.println("[CMD] Reset.");
    } else if (cmd.equalsIgnoreCase("status")) {
        Serial.printf("[HOTEND] Ist=%.2f °C  Soll=%.1f °C  Duty=%.0f%%\n",
                      hotend_get_temperature(), hotend_get_target(),
                      hotend_get_duty() * 100.0f);
        Serial.printf("         Fan=%d  AutoFan=%s  Fault=%s (%s)\n",
                      hotend_get_fan_duty(),
                      hotend_is_fan_auto() ? "ja" : "nein",
                      hotend_has_fault() ? "JA" : "nein",
                      hotend_get_fault_string());
    } else if (cmd.equalsIgnoreCase("help")) {
        Serial.println("T/T1..T4  S<temp>  F<duty>  R  status  help");
    } else {
        Serial.println("Unbekannt. 'help' für Übersicht.");
    }
}
