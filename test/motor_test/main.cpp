// =============================================================
// Motor-Test: TMC2208, RMT-Bewegung, E-Steps-Kalibrierung
// Serielle Befehle (115200 Baud):
//   status                        – TMC2208-Status ausgeben
//   move <speed> <dur> [fwd|rev]  – Zeitbasierte Bewegung
//   move_dist <speed> <mm> [fwd|rev] – Distanzbasierte Bewegung
//   stop                          – Sofortstopp
//   current <run_ma> [hold_ma]    – Motorstrom setzen
//   microstep <n>                 – Mikroschritt (1..256)
//   stealthchop <on|off>          – StealthChop umschalten
//   esteps                        – E-Steps anzeigen
//   esteps set <wert>             – E-Steps manuell setzen
//   cal start [dist] [speed]      – Kalibrierung Phase 1
//   cal apply <remaining_mm>      – Kalibrierung Phase 2
//   enable                        – Motor aktivieren (EN LOW)
//   disable                       – Motor deaktivieren (EN HIGH)
//   help                          – Befehlsübersicht
// =============================================================

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "motor/motor.h"
#include "motor/tmc2208_uart.h"

static SemaphoreHandle_t s_uart_mutex;

// ── Hilfsfunktionen ───────────────────────────────────────────

static MotorDir parse_dir(const String &token) {
    return (token.equalsIgnoreCase("rev")) ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
}

// Fortschrittsbalken während Bewegung ausgeben
static void wait_for_move(float duration_s) {
    // Auf Bewegungsstart warten (max 2 s)
    TickType_t t0 = xTaskGetTickCount();
    while (!motor_is_moving()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(2000)) {
            Serial.println("[MOTOR] Bewegung nicht gestartet (Timeout).");
            return;
        }
    }

    uint32_t total_ms = (uint32_t)(duration_s * 1000.0f + 500);
    uint32_t start_ms = millis();
    int last_pct = -1;

    Serial.print("[MOTOR] Moving... [");
    // Platz für den Balken reservieren – wird überschrieben
    Serial.print("                    ] 0%");

    while (motor_is_moving()) {
        uint32_t elapsed = millis() - start_ms;
        int pct = (total_ms > 0) ? (int)(elapsed * 100UL / total_ms) : 0;
        if (pct > 100) pct = 100;

        if (pct != last_pct) {
            last_pct = pct;
            int filled = pct / 5;   // 20 Zeichen Balken
            // Zeile neu ausgeben
            Serial.printf("\r[MOTOR] Moving... [");
            for (int i = 0; i < 20; i++) Serial.print(i < filled ? '#' : ' ');
            Serial.printf("] %3d%%", pct);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Abschluss auf 100 %
    Serial.printf("\r[MOTOR] Moving... [####################] 100%%\n");
}

// ── Befehl: status ────────────────────────────────────────────
static void cmd_status() {
    TMC2208Status st;
    bool ok = motor_get_tmc_status(&st);

    Serial.println("[MOTOR] TMC2208 Status:");
    if (!ok) {
        Serial.println("  !! Kommunikation fehlgeschlagen !!");
        return;
    }

    Serial.printf("  Run Current:   %d mA\n",   MOTOR_CURRENT_MA);
    Serial.printf("  Hold Current:  %d mA\n",   MOTOR_HOLD_CURRENT_MA);
    Serial.printf("  Microstep:     1/%d\n",    MOTOR_MICROSTEP);
    Serial.printf("  StealthChop:   %s\n",      st.stealthchop_active ? "ON" : "OFF");
    Serial.printf("  Interpolation: ON (256 µStep)\n");
    Serial.printf("  Driver Temp:   %s\n",      st.ot_shutdown ? "FAULT" : (st.ot_warning ? "WARN" : "OK"));
    Serial.printf("  OT Warning:    %s\n",      st.ot_warning  ? "YES" : "NO");
    Serial.printf("  Short A/B:     %s / %s\n", st.short_a ? "YES" : "NO", st.short_b ? "YES" : "NO");
    Serial.printf("  Open Load A/B: %s / %s\n", st.open_load_a ? "YES" : "NO", st.open_load_b ? "YES" : "NO");
    Serial.printf("  Standstill:    %s\n",      st.standstill ? "YES" : "NO");

    float esteps = motor_get_esteps();
    bool  cal    = motor_esteps_is_calibrated();
    Serial.printf("  E-Steps:       %.2f Steps/mm (%s)\n",
                  esteps, cal ? "kalibriert" : "DEFAULT — nicht kalibriert");
}

// ── Befehl: move <speed> <dur> [fwd|rev] ─────────────────────
static void cmd_move(const String &args) {
    // args: "speed dur [fwd|rev]"
    int s1 = args.indexOf(' ');
    if (s1 < 0) { Serial.println("[ERR] Syntax: move <speed> <dur> [fwd|rev]"); return; }
    float speed = args.substring(0, s1).toFloat();

    int s2 = args.indexOf(' ', s1 + 1);
    float dur;
    MotorDir dir = MOTOR_DIR_FORWARD;
    if (s2 < 0) {
        dur = args.substring(s1 + 1).toFloat();
    } else {
        dur = args.substring(s1 + 1, s2).toFloat();
        dir = parse_dir(args.substring(s2 + 1));
    }

    if (speed <= 0 || dur <= 0) {
        Serial.println("[ERR] Ungültige Werte. Speed und Dauer müssen > 0 sein.");
        return;
    }

    float dist  = speed * dur;
    float freq  = speed * motor_get_esteps();
    float steps = dist  * motor_get_esteps();

    Serial.printf("[MOTOR] Move: %.2f mm/s × %.2f s = %.2f mm (%.0f steps @ %.0f Hz) %s\n",
                  speed, dur, dist, steps, freq,
                  dir == MOTOR_DIR_FORWARD ? "FWD" : "REV");

    if (!motor_move(speed, dur, dir)) {
        Serial.println("[ERR] Befehl abgelehnt (Motor beschäftigt?).");
        return;
    }

    wait_for_move(dur);
    Serial.printf("[MOTOR] Move done. Steps: %.0f\n", steps);
}

// ── Befehl: move_dist <speed> <mm> [fwd|rev] ─────────────────
static void cmd_move_dist(const String &args) {
    int s1 = args.indexOf(' ');
    if (s1 < 0) { Serial.println("[ERR] Syntax: move_dist <speed> <mm> [fwd|rev]"); return; }
    float speed = args.substring(0, s1).toFloat();

    int s2 = args.indexOf(' ', s1 + 1);
    float dist;
    MotorDir dir = MOTOR_DIR_FORWARD;
    if (s2 < 0) {
        dist = args.substring(s1 + 1).toFloat();
    } else {
        dist = args.substring(s1 + 1, s2).toFloat();
        dir  = parse_dir(args.substring(s2 + 1));
    }

    if (speed <= 0 || dist <= 0) {
        Serial.println("[ERR] Ungültige Werte. Speed und Distanz müssen > 0 sein.");
        return;
    }

    float dur   = dist / speed;
    float freq  = speed * motor_get_esteps();
    float steps = dist  * motor_get_esteps();

    Serial.printf("[MOTOR] Move dist: %.2f mm/s × %.2f mm = %.2f s (%.0f steps @ %.0f Hz) %s\n",
                  speed, dist, dur, steps, freq,
                  dir == MOTOR_DIR_FORWARD ? "FWD" : "REV");

    if (!motor_move_distance(speed, dist, dir)) {
        Serial.println("[ERR] Befehl abgelehnt (Motor beschäftigt?).");
        return;
    }

    wait_for_move(dur);
    Serial.printf("[MOTOR] Move done. Steps: %.0f\n", steps);
}

// ── Befehl: current <run_ma> [hold_ma] ───────────────────────
static void cmd_current(const String &args) {
    int sp = args.indexOf(' ');
    uint16_t run_ma, hold_ma;
    if (sp < 0) {
        run_ma  = (uint16_t)args.toInt();
        hold_ma = 0;
    } else {
        run_ma  = (uint16_t)args.substring(0, sp).toInt();
        hold_ma = (uint16_t)args.substring(sp + 1).toInt();
    }
    if (run_ma == 0) { Serial.println("[ERR] Syntax: current <run_ma> [hold_ma]"); return; }
    if (!motor_set_current(run_ma, hold_ma))
        Serial.println("[ERR] Strom-Änderung fehlgeschlagen.");
    else
        Serial.printf("[CMD] Strom: Run=%u mA  Hold=%u mA\n",
                      run_ma, hold_ma ? hold_ma : run_ma / 2);
}

// ── Befehl: microstep <n> ─────────────────────────────────────
static void cmd_microstep(const String &args) {
    uint8_t ms = (uint8_t)args.toInt();
    if (ms == 0) { Serial.println("[ERR] Syntax: microstep <1|2|4|8|16|32|64|128|256>"); return; }
    if (!motor_set_microstep(ms))
        Serial.println("[ERR] Mikroschritt-Änderung fehlgeschlagen.");
    else
        Serial.printf("[CMD] Mikroschritt: 1/%u\n", ms);
}

// ── Befehl: stealthchop <on|off> ─────────────────────────────
static void cmd_stealthchop(const String &args) {
    bool en;
    if (args.equalsIgnoreCase("on"))       en = true;
    else if (args.equalsIgnoreCase("off")) en = false;
    else { Serial.println("[ERR] Syntax: stealthchop <on|off>"); return; }
    if (!motor_set_stealthchop(en))
        Serial.println("[ERR] StealthChop-Änderung fehlgeschlagen.");
    else
        Serial.printf("[CMD] StealthChop: %s\n", en ? "ON" : "OFF");
}

// ── Befehl: esteps / esteps set <val> ────────────────────────
static void cmd_esteps(const String &args) {
    if (args.length() == 0) {
        float e = motor_get_esteps();
        bool  c = motor_esteps_is_calibrated();
        Serial.printf("[MOTOR] E-Steps: %.4f Steps/mm (%s)\n",
                      e, c ? "kalibriert" : "DEFAULT — nicht kalibriert");
        return;
    }
    // "set <wert>"
    String a = args;
    a.trim();
    if (a.startsWith("set ") || a.startsWith("set\t")) {
        float val = a.substring(4).toFloat();
        if (val <= 0) { Serial.println("[ERR] Ungültiger Wert."); return; }
        if (!motor_set_esteps(val))
            Serial.println("[ERR] Setzen fehlgeschlagen.");
        else
            Serial.printf("[CMD] E-Steps gesetzt: %.4f Steps/mm\n", val);
    } else {
        Serial.println("[ERR] Syntax: esteps  |  esteps set <wert>");
    }
}

// ── Befehl: cal start [dist] [speed] ─────────────────────────
static void cmd_cal_start(const String &args) {
    float dist  = 100.0f;
    float speed = 3.0f;

    if (args.length() > 0) {
        int sp = args.indexOf(' ');
        if (sp < 0) {
            dist = args.toFloat();
        } else {
            dist  = args.substring(0, sp).toFloat();
            speed = args.substring(sp + 1).toFloat();
        }
    }
    if (dist <= 0 || speed <= 0) {
        Serial.println("[ERR] Ungültige Werte."); return;
    }

    float steps = dist * motor_get_esteps();
    Serial.println("[MOTOR] E-Steps Calibration Phase 1:");
    Serial.printf("  Extruding %.2f mm at %.2f mm/s (%.0f steps)...\n",
                  dist, speed, steps);

    if (!motor_calibrate_start(dist, speed)) {
        Serial.println("[ERR] Kalibrierung fehlgeschlagen (Motor beschäftigt?).");
        return;
    }

    float dur = dist / speed;
    wait_for_move(dur);

    Serial.println("[MOTOR] Fertig. Verbleibende Strecke zur Markierung messen und eingeben:");
    Serial.println("  > cal apply <remaining_mm>");
}

// ── Befehl: cal apply <remaining_mm> ─────────────────────────
static void cmd_cal_apply(const String &args) {
    float remaining = args.toFloat();
    if (remaining < 0.0f || args.length() == 0) {
        Serial.println("[ERR] Syntax: cal apply <remaining_mm>"); return;
    }

    float old_esteps = motor_get_esteps();
    if (!motor_calibrate_apply(remaining)) {
        Serial.println("[ERR] apply fehlgeschlagen (Phase 1 nicht abgeschlossen?).");
        return;
    }
    float new_esteps = motor_get_esteps();

    Serial.println("[MOTOR] E-Steps Calibration Phase 2:");
    Serial.printf("  Commanded: 100.00 mm | Actual: %.2f mm\n",
                  100.0f - remaining);
    Serial.printf("  Old E-Steps: %.2f | New E-Steps: %.2f\n",
                  old_esteps, new_esteps);
    Serial.println("  Saved to NVS.");
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Motor-Test (Prüfstand ESP32) ===");

    s_uart_mutex = xSemaphoreCreateMutex();
    motor_init(s_uart_mutex);
    delay(300);

    Serial.println("Bereit. Befehle:");
    Serial.println("  status                          – TMC2208-Status");
    Serial.println("  move <speed> <dur> [fwd|rev]    – Zeitbasierte Bewegung");
    Serial.println("  move_dist <speed> <mm> [fwd|rev]– Distanzbasierte Bewegung");
    Serial.println("  stop                            – Sofortstopp");
    Serial.println("  current <run_ma> [hold_ma]      – Motorstrom");
    Serial.println("  microstep <n>                   – Mikroschritt");
    Serial.println("  stealthchop <on|off>             – StealthChop");
    Serial.println("  esteps                          – E-Steps anzeigen");
    Serial.println("  esteps set <wert>               – E-Steps setzen");
    Serial.println("  cal start [dist] [speed]        – Kalibrierung Phase 1");
    Serial.println("  cal apply <remaining_mm>        – Kalibrierung Phase 2");
    Serial.println("  enable / disable                – Motor EN-Pin");
    Serial.println("  help                            – Diese Übersicht");
}

// ── Loop ──────────────────────────────────────────────────────
static unsigned long s_last_status = 0;

void loop() {
    // Alle 5 s automatisch Kurzstatus ausgeben
    if (millis() - s_last_status >= 5000) {
        s_last_status = millis();
        float speed = motor_get_current_speed();
        Serial.printf("[MOTOR] %s  speed=%.2f mm/s  esteps=%.2f\n",
                      motor_is_moving() ? "MOVING" : "IDLE  ",
                      speed, motor_get_esteps());
    }

    if (!Serial.available()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("> %s\n", cmd.c_str());

    // Befehl + Argumente trennen
    int sp = cmd.indexOf(' ');
    String verb = (sp < 0) ? cmd : cmd.substring(0, sp);
    String args = (sp < 0) ? String("") : cmd.substring(sp + 1);
    args.trim();
    verb.toLowerCase();

    // move_dist muss vor move geprüft werden (Präfix-Überschneidung)
    String cmd_lower = cmd;
    cmd_lower.toLowerCase();

    if (verb == "status") {
        cmd_status();

    } else if (cmd_lower.startsWith("move_dist")) {
        String a2 = cmd.substring(9);
        a2.trim();
        cmd_move_dist(a2);

    } else if (verb == "move") {
        cmd_move(args);

    } else if (verb == "stop") {
        motor_stop();
        Serial.println("[CMD] Stop.");

    } else if (verb == "current") {
        cmd_current(args);

    } else if (verb == "microstep") {
        cmd_microstep(args);

    } else if (verb == "stealthchop") {
        cmd_stealthchop(args);

    } else if (verb == "esteps") {
        cmd_esteps(args);

    } else if (verb == "cal") {
        // "cal start ..." oder "cal apply ..."
        int sp2 = args.indexOf(' ');
        String sub  = (sp2 < 0) ? args : args.substring(0, sp2);
        String rest = (sp2 < 0) ? String("") : args.substring(sp2 + 1);
        rest.trim();
        sub.toLowerCase();
        if (sub == "start")       cmd_cal_start(rest);
        else if (sub == "apply")  cmd_cal_apply(rest);
        else Serial.println("[ERR] Syntax: cal start [dist] [speed]  |  cal apply <remaining_mm>");

    } else if (verb == "enable") {
        digitalWrite(MOTOR_EN_PIN, LOW);
        Serial.println("[CMD] Motor aktiviert (EN LOW).");

    } else if (verb == "disable") {
        digitalWrite(MOTOR_EN_PIN, HIGH);
        Serial.println("[CMD] Motor deaktiviert (EN HIGH).");

    } else if (verb == "help") {
        Serial.println("status | move | move_dist | stop | current | microstep");
        Serial.println("stealthchop | esteps | cal start | cal apply | enable | disable | help");

    } else {
        Serial.println("Unbekannt. 'help' für Übersicht.");
    }
}
