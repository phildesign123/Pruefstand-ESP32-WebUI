#include "hotend.h"
#include "sensor.h"
#include "heater.h"
#include "fan.h"
#include "pid_controller.h"
#include "safety.h"
#include "autotune.h"
#include "../config.h"
#include "../load_cell/load_cell.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

// =============================================================
// Hotend-Modul – FreeRTOS-Task, PID-Regelung, Safety
// =============================================================

static PIDController s_pid;
static volatile float s_target_temp  = 0.0f;
static volatile float s_current_temp = 0.0f;
static volatile float s_duty_frac    = 0.0f;
static volatile bool  s_fault        = false;
static volatile SafetyFault s_fault_code = SAFETY_OK;
static volatile bool  s_autotune_running = false;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void emergency_stop(SafetyFault fault) {
    set_heater_pwm(0);
    load_cell_set_compensation(0);
    ledcWrite(FAN_PIN, 255);   // Lüfter voll an zum Abkühlen
    s_pid.reset();
    portENTER_CRITICAL(&s_mux);
    s_target_temp = 0.0f;
    s_fault       = true;
    s_fault_code  = fault;
    portEXIT_CRITICAL(&s_mux);
    Serial.printf("[HOTEND] !!! NOTABSCHALTUNG !!! %s\n", safety_fault_string(fault));
    if (fault == FAULT_SENSOR) {
        Serial.printf("[HOTEND] MAX31865 fault=0x%02X raw=%u R=%.2f ohm T=%.2f C\n",
                      sensor_get_fault_code(),
                      (unsigned int)sensor_get_raw_rtd(),
                      sensor_get_resistance(),
                      s_current_temp);
    }
}

void hotend_task(void *arg) {
    unsigned long last_pid_ms  = 0;

    for (;;) {
        // Sensor-State-Machine bei JEDEM ms-Tick antreiben
        float temp = read_temperature();
        unsigned long now = millis();

        // PID-Zyklus alle ~164 ms
        if (now - last_pid_ms >= PID_INTERVAL_MS) {
            last_pid_ms = now;

            portENTER_CRITICAL(&s_mux);
            s_current_temp = temp;
            bool  fault_active = s_fault;
            float target       = s_target_temp;
            portEXIT_CRITICAL(&s_mux);

            if (sensor_has_fault()) {
                if (!fault_active) emergency_stop(FAULT_SENSOR);
            }

            if (!fault_active) {
                float output = s_pid.compute(target, temp);
                uint8_t pwm  = (uint8_t)((int)output >> 1);  // 0–255 → 0–127

                SafetyFault sf = safety_check(temp, target, pwm);
                if (sf != SAFETY_OK) {
                    emergency_stop(sf);
                } else {
                    set_heater_pwm(pwm);
                    float duty = (float)output / (float)PID_MAX;
                    portENTER_CRITICAL(&s_mux);
                    s_duty_frac = duty;
                    portEXIT_CRITICAL(&s_mux);
                    load_cell_set_compensation((int32_t)(duty * LOAD_CELL_HEATER_COMP));
                }

                update_fan(temp);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ── Öffentliche API ──────────────────────────────────────────

void hotend_init(SPIClass &spi, SemaphoreHandle_t spi_mutex) {
    setup_sensor(spi, spi_mutex);
    setup_heater();
    setup_fan();
    s_pid.set_tunings(DEFAULT_Kp, DEFAULT_Ki, DEFAULT_Kd);

    // WDT deaktiviert während Entwicklung

    xTaskCreatePinnedToCore(
        hotend_task, "hotend_pid", TASK_STACK_HOTEND,
        nullptr, TASK_PRIO_HOTEND, nullptr, CORE_REALTIME);

    Serial.println("[HOTEND] Initialisiert.");
}

bool hotend_set_target(float temp_c) {
    if (temp_c >= TEMP_MAX) return false;
    portENTER_CRITICAL(&s_mux);
    s_target_temp = temp_c;
    portEXIT_CRITICAL(&s_mux);
    if (temp_c == 0.0f) {
        s_pid.reset();
        set_heater_pwm(0);
        load_cell_set_compensation(0);
    }
    return true;
}

float   hotend_get_temperature()  { return s_current_temp; }
float   hotend_get_target()       { return s_target_temp; }
float   hotend_get_duty()         { return s_duty_frac; }
float   hotend_get_pid_p()        { return s_pid.get_work_p(); }
float   hotend_get_pid_i()        { return s_pid.get_work_i(); }
float   hotend_get_pid_d()        { return s_pid.get_work_d(); }
uint8_t hotend_get_fan_duty()     { return get_fan_duty(); }
bool    hotend_is_fan_auto()      { return is_fan_auto(); }
bool    hotend_has_fault()        { return s_fault; }
SafetyFault hotend_get_fault()    { return s_fault_code; }
const char* hotend_get_fault_string() { return safety_fault_string(s_fault_code); }

void hotend_set_pid(float kp, float ki, float kd) {
    s_pid.set_tunings(kp, ki, kd);
}

void hotend_set_fan(uint8_t duty) {
    set_fan_override(duty);
}

void hotend_fan_off_timed(uint32_t duration_ms) {
    set_fan_off_timed(duration_ms);
}

void hotend_clear_fault() {
    set_heater_pwm(0);
    s_pid.reset();
    safety_reset();
    sensor_clear_fault();
    portENTER_CRITICAL(&s_mux);
    s_fault       = false;
    s_fault_code  = SAFETY_OK;
    s_target_temp = 0.0f;
    portEXIT_CRITICAL(&s_mux);
    ledcWrite(FAN_PIN, 0);
    Serial.println("[HOTEND] Fault quittiert.");
}

AutotuneResult hotend_autotune(float target, int cycles) {
    s_autotune_running = true;
    // Autotune blockiert – Hotend-Task läuft parallel und
    // übernimmt nach Abschluss automatisch die neuen PID-Werte
    AutotuneResult r = autotune(target, cycles);
    if (r.success) s_pid.set_tunings(r.Kp, r.Ki, r.Kd);
    s_autotune_running = false;
    return r;
}
