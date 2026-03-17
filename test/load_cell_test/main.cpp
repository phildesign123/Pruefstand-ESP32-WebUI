// =============================================================
// Wägezellen-Test: NAU7802, Filter, Tara, Kalibrierung
// Serielle Befehle (115200 Baud):
//   status                   – NAU7802-Status ausgeben
//   raw                      – Einmaligen Rohwert ausgeben
//   stream [on|off]          – Kontinuierliche Ausgabe (80 Hz CSV)
//   stream slow [on|off]     – Langsame Ausgabe (1 Hz)
//   tare                     – Tarierung durchführen
//   calibrate <gewicht_g>    – Kalibrierung mit Referenzgewicht
//   calinfo                  – Kalibrierfaktor/Tara anzeigen
//   calreset                 – Kalibrierung zurücksetzen (nicht unterstützt)
//   filter median <n>        – Median-Fenstergröße (Compile-Zeit, Info only)
//   filter avg <n>           – Avg-Fenstergröße (Compile-Zeit, Info only)
//   help                     – Befehlsübersicht
// =============================================================

#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "load_cell/load_cell.h"

static SemaphoreHandle_t s_i2c_mutex;

// ── Stream-Modus ──────────────────────────────────────────────
enum StreamMode { STREAM_OFF = 0, STREAM_FAST, STREAM_SLOW };
static volatile StreamMode s_stream = STREAM_OFF;

// Stabilitätsverfolgung (±0.5 g über 500 ms)
static float   s_stab_min   = 0.0f;
static float   s_stab_max   = 0.0f;
static unsigned long s_stab_t = 0;

static void stab_update(float w) {
    unsigned long now = millis();
    if (now - s_stab_t > 500) {
        s_stab_min = s_stab_max = w;
        s_stab_t = now;
    } else {
        if (w < s_stab_min) s_stab_min = w;
        if (w > s_stab_max) s_stab_max = w;
    }
}
static bool stab_is_stable() {
    return (s_stab_max - s_stab_min) < 0.5f;
}

// ── Befehl: status ────────────────────────────────────────────
static void cmd_status() {
    float   w   = load_cell_get_weight_g();
    int32_t raw = load_cell_get_raw();
    bool    cal = load_cell_is_calibrated();
    Serial.printf("[LOADCELL] Status: %s | Gain: 128 | SPS: 80 | Cal: %s\n",
                  "OK",
                  cal ? "valid" : "NOT calibrated");
    Serial.printf("[LOADCELL] Raw: %ld | Weight: %.2f g\n", (long)raw, w);
}

// ── Befehl: raw ───────────────────────────────────────────────
static void cmd_raw() {
    int32_t raw = load_cell_get_raw();
    float   w   = load_cell_get_weight_g();
    Serial.printf("[LOADCELL] Raw: %ld | Filtered: %ld | Weight: %.2f g\n",
                  (long)raw, (long)raw, w);
}

// ── Befehl: tare ─────────────────────────────────────────────
static void cmd_tare() {
    Serial.println("[LOADCELL] Tarierung läuft (ca. 1 s)...");
    if (load_cell_tare())
        Serial.println("[LOADCELL] Tare done. Gewicht = 0 g");
    else
        Serial.println("[ERR] Tarierung fehlgeschlagen.");
}

// ── Befehl: calibrate <g> ────────────────────────────────────
static void cmd_calibrate(const String &args) {
    float g = args.toFloat();
    if (g <= 0) {
        Serial.println("[ERR] Syntax: calibrate <gewicht_g>  (z.B. calibrate 500)");
        return;
    }
    Serial.printf("[LOADCELL] Kalibrierung mit %.1f g läuft (ca. 1 s)...\n", g);
    if (load_cell_calibrate(g))
        Serial.printf("[LOADCELL] Cal done. Gewicht ≈ %.1f g\n", g);
    else
        Serial.println("[ERR] Kalibrierung fehlgeschlagen.");
}

// ── Befehl: calinfo ───────────────────────────────────────────
static void cmd_calinfo() {
    bool cal = load_cell_is_calibrated();
    Serial.printf("[LOADCELL] Kalibrierung: %s\n", cal ? "gültig" : "NICHT kalibriert");
    Serial.printf("[LOADCELL] Median-Fenstergröße: %d (Compile-Zeit)\n", LOAD_CELL_MEDIAN_SIZE);
    Serial.printf("[LOADCELL] AvgFilter-Fenstergröße: %d (Compile-Zeit)\n", LOAD_CELL_AVG_SIZE);
}

// ── CSV-Header für Stream-Modus ───────────────────────────────
static void print_csv_header() {
    Serial.println("# time_ms, raw, weight_g");
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Wägezellen-Test (Prüfstand ESP32) ===");

    s_i2c_mutex = xSemaphoreCreateMutex();
    Wire.begin(NAU7802_SDA, NAU7802_SCL, NAU7802_I2C_FREQ);

    if (!load_cell_init(Wire, s_i2c_mutex)) {
        Serial.println("[ERR] NAU7802 Init fehlgeschlagen! Hardware prüfen.");
    } else {
        Serial.println("[OK] NAU7802 bereit.");
    }

    delay(200);

    Serial.println("Befehle:");
    Serial.println("  status                   – NAU7802-Status");
    Serial.println("  raw                      – Einmaliger Rohwert");
    Serial.println("  stream [on|off]          – 80-Hz CSV-Stream");
    Serial.println("  stream slow [on|off]     – 1-Hz Stream");
    Serial.println("  tare                     – Tarierung");
    Serial.println("  calibrate <gewicht_g>    – Kalibrierung");
    Serial.println("  calinfo                  – Kalibrierinfo");
    Serial.println("  help                     – Diese Übersicht");
}

// ── Loop ──────────────────────────────────────────────────────
static unsigned long s_last_fast  = 0;
static unsigned long s_last_slow  = 0;

void loop() {
    unsigned long now = millis();

    // ── Streaming ─────────────────────────────────────────────
    if (s_stream == STREAM_FAST && now - s_last_fast >= 12) {
        s_last_fast = now;
        int32_t raw = load_cell_get_raw();
        float   w   = load_cell_get_weight_g();
        stab_update(w);
        Serial.printf("%lu, %ld, %.2f\n", now, (long)raw, w);
    }

    if (s_stream == STREAM_SLOW && now - s_last_slow >= 1000) {
        s_last_slow = now;
        int32_t raw = load_cell_get_raw();
        float   w   = load_cell_get_weight_g();
        stab_update(w);
        Serial.printf("[LOADCELL] t=%lums | Raw=%ld | %.2f g | %s\n",
                      now, (long)raw, w,
                      stab_is_stable() ? "stable" : "unstable");
    }

    if (!Serial.available()) {
        // Kein Delay wenn schnelles Streaming läuft
        if (s_stream != STREAM_FAST)
            vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("> %s\n", cmd.c_str());

    String cmd_lower = cmd;
    cmd_lower.toLowerCase();

    int sp    = cmd_lower.indexOf(' ');
    String verb = (sp < 0) ? cmd_lower : cmd_lower.substring(0, sp);
    String args = (sp < 0) ? String("") : cmd.substring(sp + 1);
    args.trim();

    if (verb == "status") {
        cmd_status();

    } else if (verb == "raw") {
        cmd_raw();

    } else if (verb == "stream") {
        // "stream on|off" oder "stream slow on|off"
        String a = args;
        a.toLowerCase();
        if (a == "slow on" || a == "slow") {
            if (s_stream == STREAM_FAST) Serial.println("[INFO] Schneller Stream gestoppt.");
            s_stream = STREAM_SLOW;
            s_last_slow = 0;
            Serial.println("[LOADCELL] Langsamer Stream gestartet (1 Hz).");
        } else if (a == "slow off") {
            s_stream = STREAM_OFF;
            Serial.println("[LOADCELL] Stream gestoppt.");
        } else if (a == "on" || a.length() == 0) {
            if (s_stream == STREAM_SLOW) Serial.println("[INFO] Langsamer Stream gestoppt.");
            s_stream = STREAM_FAST;
            s_last_fast = 0;
            print_csv_header();
            Serial.println("[LOADCELL] Schneller Stream gestartet (80 Hz CSV). 'stream off' zum Stoppen.");
        } else if (a == "off") {
            s_stream = STREAM_OFF;
            Serial.println("[LOADCELL] Stream gestoppt.");
        } else {
            Serial.println("[ERR] Syntax: stream [on|off]  |  stream slow [on|off]");
        }

    } else if (verb == "tare") {
        StreamMode prev = s_stream;
        s_stream = STREAM_OFF;
        cmd_tare();
        s_stream = prev;

    } else if (verb == "calibrate") {
        StreamMode prev = s_stream;
        s_stream = STREAM_OFF;
        cmd_calibrate(args);
        s_stream = prev;

    } else if (verb == "calinfo") {
        cmd_calinfo();

    } else if (verb == "calreset") {
        Serial.println("[INFO] calreset: Kein öffentliches API vorhanden.");
        Serial.println("       Kalibrierung über 'calibrate <g>' neu durchführen.");

    } else if (verb == "filter") {
        // "filter median <n>" oder "filter avg <n>"
        String a = args;
        a.toLowerCase();
        Serial.println("[INFO] Filtergrößen sind zur Compile-Zeit fixiert:");
        Serial.printf("       Median: %d  |  Avg: %d\n",
                      LOAD_CELL_MEDIAN_SIZE, LOAD_CELL_AVG_SIZE);
        Serial.println("       Zum Ändern: LOAD_CELL_MEDIAN_SIZE / LOAD_CELL_AVG_SIZE in config.h anpassen.");

    } else if (verb == "help") {
        Serial.println("status | raw | stream [on|off] | stream slow [on|off]");
        Serial.println("tare | calibrate <g> | calinfo | filter median <n> | filter avg <n> | help");

    } else {
        Serial.println("Unbekannt. 'help' für Übersicht.");
    }
}
