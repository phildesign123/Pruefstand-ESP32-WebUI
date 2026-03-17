#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_sntp.h"

#include "config.h"
#include "hotend/hotend.h"
#include "motor/motor.h"
#include "load_cell/load_cell.h"
#include "datalog/datalog.h"
#include "webui/webui.h"
#include "webui/sequencer.h"

// =============================================================
// Prüfstand ESP32 – Hauptprogramm
// Initialisiert alle Module, startet FreeRTOS-Tasks
// =============================================================

// ── Shared Mutexes ───────────────────────────────────────────
static SemaphoreHandle_t spi_mutex   = nullptr;
static SemaphoreHandle_t i2c_mutex   = nullptr;
static SemaphoreHandle_t uart_mutex  = nullptr;

// ── Shared SPI-Bus ────────────────────────────────────────────
static SPIClass vspi(VSPI);

// ── Shared I2C-Bus ────────────────────────────────────────────
// Wire (I2C0) wird für NAU7802 verwendet

// ── NTP-Synchronisation ───────────────────────────────────────
static void sntp_init_callback(struct timeval *tv) {
    Serial.println("[NTP] Zeit synchronisiert.");
    struct tm ti;
    getLocalTime(&ti);
    Serial.printf("[NTP] Zeit: %04d-%02d-%02d %02d:%02d:%02d\n",
                  ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                  ti.tm_hour, ti.tm_min, ti.tm_sec);
}

// ── Setup ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Prüfstand ESP32 WebUI ===");
    Serial.printf("Free Heap: %lu Bytes\n", (unsigned long)ESP.getFreeHeap());

    // ── Mutexes erstellen ────────────────────────────────────
    spi_mutex  = xSemaphoreCreateMutex();
    i2c_mutex  = xSemaphoreCreateMutex();
    uart_mutex = xSemaphoreCreateMutex();

    // ── SPI-Bus initialisieren (VSPI: MAX31865 + SD) ─────────
    vspi.begin(MAX_CLK, MAX_MISO, MAX_MOSI, MAX_CS);

    // ── I2C-Bus initialisieren (NAU7802) ─────────────────────
    Wire.begin(NAU7802_SDA, NAU7802_SCL, NAU7802_I2C_FREQ);

    // ── Hotend (Sensor + PID + Lüfter) ───────────────────────
    hotend_init(vspi, spi_mutex);

    // ── Motor (TMC2208 + RMT) ─────────────────────────────────
    motor_init(uart_mutex);

    // ── Wägezelle (NAU7802) ───────────────────────────────────
    load_cell_init(Wire, i2c_mutex);

    // ── Datenlogger (SD-Karte) ────────────────────────────────
    datalog_init(vspi, spi_mutex);

    // ── Web-UI (WiFi AP + HTTP + WebSocket) ──────────────────
    webui_init();

    // ── Sequencer-Task starten ────────────────────────────────
    sequencer_init();

    // ── SNTP für Zeitstempel (läuft nach WiFi-Verbindung) ────
    sntp_set_time_sync_notification_cb(sntp_init_callback);
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");

    Serial.printf("[MAIN] Bereit. Free Heap: %lu Bytes\n",
                  (unsigned long)ESP.getFreeHeap());
    Serial.printf("[MAIN] Verbinde mit WLAN: http://%s\n", WIFI_AP_IP);
}

// ── Loop ─────────────────────────────────────────────────────
// Alle zeitkritische Arbeit läuft in FreeRTOS-Tasks.
// Loop nur für serielle Kommandos.

void loop() {
    if (!Serial.available()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    // ── Hotend-Kommandos ──────────────────────────────────────
    if (cmd.startsWith("S") || cmd.startsWith("s")) {
        float t = cmd.substring(1).toFloat();
        if (!hotend_set_target(t))
            Serial.printf("[CMD] Fehler: %.1f °C abgelehnt (Max: %.0f °C)\n", t, TEMP_MAX);
        else
            Serial.printf("[CMD] Zieltemperatur: %.1f °C\n", t);

    } else if (cmd.startsWith("A") || cmd.startsWith("a")) {
        float t = (cmd.length() > 1) ? cmd.substring(1).toFloat() : 200.0f;
        Serial.printf("[CMD] Autotune bei %.1f °C...\n", t);
        AutotuneResult r = hotend_autotune(t);
        if (r.success)
            Serial.printf("[CMD] PID: Kp=%.2f Ki=%.4f Kd=%.2f\n", r.Kp, r.Ki, r.Kd);

    } else if (cmd.startsWith("P") || cmd.startsWith("p")) {
        int i1 = cmd.indexOf(','), i2 = cmd.indexOf(',', i1 + 1);
        if (i1 > 0 && i2 > 0) {
            float kp = cmd.substring(1, i1).toFloat();
            float ki = cmd.substring(i1 + 1, i2).toFloat();
            float kd = cmd.substring(i2 + 1).toFloat();
            hotend_set_pid(kp, ki, kd);
            Serial.printf("[CMD] PID: Kp=%.2f Ki=%.4f Kd=%.2f\n", kp, ki, kd);
        }

    } else if (cmd.startsWith("F") || cmd.startsWith("f")) {
        hotend_set_fan((uint8_t)cmd.substring(1).toInt());

    } else if (cmd.equalsIgnoreCase("R") || cmd.equalsIgnoreCase("reset")) {
        hotend_clear_fault();
        Serial.println("[CMD] Reset.");

    // ── Motor-Kommandos ───────────────────────────────────────
    } else if (cmd.startsWith("move ")) {
        // move <speed> <duration> [fwd|rev]
        float speed = 0, dur = 0; char dir[4] = "fwd";
        sscanf(cmd.c_str() + 5, "%f %f %3s", &speed, &dur, dir);
        motor_move(speed, dur, strcmp(dir, "rev") == 0 ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD);

    } else if (cmd.equalsIgnoreCase("stop")) {
        motor_stop();

    // ── Wägezelle ─────────────────────────────────────────────
    } else if (cmd.equalsIgnoreCase("tare")) {
        load_cell_tare();
        Serial.println("[CMD] Tariert.");

    } else if (cmd.startsWith("cal ")) {
        float w = cmd.substring(4).toFloat();
        load_cell_calibrate(w);

    // ── Datenlogger ───────────────────────────────────────────
    } else if (cmd.equalsIgnoreCase("log start")) {
        datalog_start();
    } else if (cmd.equalsIgnoreCase("log stop")) {
        datalog_stop();

    // ── Status ────────────────────────────────────────────────
    } else if (cmd.equalsIgnoreCase("status")) {
        Serial.printf("T: %.1f / %.1f °C  |  W: %.2f g  |  Speed: %.2f mm/s  |  Motor: %s\n",
                      hotend_get_temperature(), hotend_get_target(),
                      load_cell_get_weight_g(), motor_get_current_speed(),
                      motor_is_moving() ? "AN" : "AUS");
        Serial.printf("Heap: %lu B  |  Uptime: %lus\n",
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)(millis() / 1000));

    } else if (cmd.equalsIgnoreCase("help")) {
        Serial.println("Kommandos: S<temp>  A[temp]  P<kp,ki,kd>  F<duty>  R  reset");
        Serial.println("           move <speed> <dur> [fwd|rev]  stop");
        Serial.println("           tare  cal <gramm>");
        Serial.println("           log start  log stop  status  help");
    }
}
