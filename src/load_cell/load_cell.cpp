#include "load_cell.h"
#include "nau7802.h"
#include "filter_median.h"
#include "filter_avg.h"
#include "../config.h"
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// =============================================================
// Wägezellen-Modul: NAU7802 + Median/AvgFilter + Tara/Kalibrierung
// DRDY-ISR gibt Semaphore frei → Task liest mit exakt 80 Hz
// =============================================================

static TwoWire          *s_wire        = nullptr;
static SemaphoreHandle_t s_i2c_mutex   = nullptr;
static TaskHandle_t      s_task_handle = nullptr;

static MedianFilter      s_median;
static AvgFilter         s_avg;

static volatile int32_t  s_filtered_raw = 0;
static volatile float    s_weight_g     = 0.0f;

static int32_t  s_tare_offset   = 0;
static float    s_cal_factor    = 1000.0f;  // Default (unkalibriert)
static bool     s_calibrated    = false;

static Preferences s_prefs;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// DRDY wird per Polling geprüft (stabiler als ISR bei hoher SPS-Rate)

// ── NVS ──────────────────────────────────────────────────────

static void nvs_load() {
    s_prefs.begin("loadcell", true);
    s_calibrated = (s_prefs.getUChar("cal_valid", 0) == 1);
    s_cal_factor  = s_prefs.getFloat("cal_factor", 1000.0f);
    s_prefs.end();
}

static void nvs_save() {
    s_prefs.begin("loadcell", false);
    s_prefs.putFloat("cal_factor", s_cal_factor);
    s_prefs.putUChar("cal_valid",  1);
    s_prefs.end();
}

// ── Sampling-Task ─────────────────────────────────────────────

static void load_cell_task(void *arg) {
    for (;;) {
        // Polling: DRDY-Register oder GPIO prüfen (80 SPS = 12.5 ms)
        if (!nau7802_is_ready(*s_wire, s_i2c_mutex)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        int32_t raw = nau7802_read_raw(*s_wire, s_i2c_mutex);

        s_median.push(raw);
        int32_t median_val = s_median.get();

        s_avg.push(median_val);
        int32_t avg_val = s_avg.get();

        float weight = (float)(avg_val - s_tare_offset) / s_cal_factor;

        portENTER_CRITICAL(&s_mux);
        s_filtered_raw = avg_val;
        s_weight_g     = weight;
        portEXIT_CRITICAL(&s_mux);
    }
}

// ── Öffentliche API ──────────────────────────────────────────

bool load_cell_init(TwoWire &wire, SemaphoreHandle_t i2c_mutex) {
    s_wire      = &wire;
    s_i2c_mutex = i2c_mutex;

    // NAU7802 initialisieren
    if (!nau7802_init(wire, i2c_mutex)) return false;

    // NVS laden
    nvs_load();
    Serial.printf("[LOADCELL] Kalibrierung: %s (Faktor=%.2f)\n",
                  s_calibrated ? "vorhanden" : "fehlt", s_cal_factor);

    // Auto-Tare VOR Task-Start (kein Mutex-Konflikt)
    Serial.println("[LOADCELL] Auto-Tare...");
    {
        vTaskDelay(pdMS_TO_TICKS(500));  // Sensor einschwingen lassen
        int64_t acc = 0;
        int count = 0;
        for (int i = 0; i < LOAD_CELL_TARE_SAMPLES; i++) {
            unsigned long t0 = millis();
            while (!nau7802_is_ready(*s_wire, s_i2c_mutex) && millis() - t0 < 50) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (!nau7802_is_ready(*s_wire, s_i2c_mutex)) continue;
            acc += nau7802_read_raw(*s_wire, s_i2c_mutex);
            count++;
        }
        if (count > 0) {
            s_tare_offset = (int32_t)(acc / count);
            Serial.printf("[LOADCELL] Tariert. Offset=%ld (%d Samples)\n", (long)s_tare_offset, count);
        }
    }

    xTaskCreatePinnedToCore(load_cell_task, "load_cell", TASK_STACK_LOADCELL,
                            nullptr, TASK_PRIO_LOADCELL, &s_task_handle, CORE_REALTIME);
    return true;
}

bool load_cell_tare() {
    if (s_task_handle) vTaskSuspend(s_task_handle);
    int64_t acc = 0;
    int     count = 0;
    for (int i = 0; i < LOAD_CELL_TARE_SAMPLES; i++) {
        unsigned long t0 = millis();
        while (!nau7802_is_ready(*s_wire, s_i2c_mutex) && millis() - t0 < 50) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!nau7802_is_ready(*s_wire, s_i2c_mutex)) continue;
        acc += nau7802_read_raw(*s_wire, s_i2c_mutex);
        count++;
    }
    if (s_task_handle) vTaskResume(s_task_handle);
    if (count == 0) return false;
    portENTER_CRITICAL(&s_mux);
    s_tare_offset = (int32_t)(acc / count);
    portEXIT_CRITICAL(&s_mux);
    Serial.printf("[LOADCELL] Tariert. Offset=%ld\n", (long)s_tare_offset);
    return true;
}

bool load_cell_calibrate(float known_weight_g) {
    if (known_weight_g <= 0.0f) return false;

    if (s_task_handle) vTaskSuspend(s_task_handle);
    int64_t acc = 0;
    int     count = 0;
    for (int i = 0; i < LOAD_CELL_CAL_SAMPLES; i++) {
        unsigned long t0 = millis();
        while (!nau7802_is_ready(*s_wire, s_i2c_mutex) && millis() - t0 < 50) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!nau7802_is_ready(*s_wire, s_i2c_mutex)) continue;
        acc += nau7802_read_raw(*s_wire, s_i2c_mutex);
        count++;
    }
    if (s_task_handle) vTaskResume(s_task_handle);
    if (count == 0) return false;
    int32_t avg = (int32_t)(acc / count);
    float factor = (float)(avg - s_tare_offset) / known_weight_g;

    if (fabsf(factor) < 1.0f) return false;  // Unrealistisch

    portENTER_CRITICAL(&s_mux);
    s_cal_factor  = factor;
    s_calibrated  = true;
    portEXIT_CRITICAL(&s_mux);

    nvs_save();
    Serial.printf("[LOADCELL] Kalibriert. Faktor=%.2f (Ref=%.1f g)\n", factor, known_weight_g);
    return true;
}

float   load_cell_get_weight_g()  { return s_weight_g; }
int32_t load_cell_get_raw()       { return s_filtered_raw; }
bool    load_cell_is_calibrated() { return s_calibrated; }

void load_cell_deinit() {
    if (s_task_handle) vTaskDelete(s_task_handle);
    s_task_handle = nullptr;
}
