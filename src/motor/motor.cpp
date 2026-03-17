#include "motor.h"
#include "motor_rmt.h"
#include "tmc2208_uart.h"
#include "../config.h"
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp32-hal.h"

// =============================================================
// Motor-Modul – TMC2208 via RMT + UART
// FreeRTOS-Queue serialisiert alle Befehle
// =============================================================

enum MotorCmdType : uint8_t {
    CMD_MOVE, CMD_MOVE_DIST, CMD_STOP,
    CMD_SET_CURRENT, CMD_SET_MICROSTEP, CMD_SET_STEALTHCHOP,
    CMD_CAL_START, CMD_CAL_APPLY, CMD_SET_ESTEPS,
};

struct MotorCmd {
    MotorCmdType type;
    union {
        struct { float speed; float duration; MotorDir dir; } move;
        struct { float speed; float distance; MotorDir dir; } move_dist;
        struct { uint16_t run_ma; uint16_t hold_ma; } current;
        struct { uint8_t microstep; } ms;
        struct { bool enable; } flag;
        struct { float distance; float speed; } cal_start;
        struct { float remaining; } cal_apply;
        struct { float steps_per_mm; } esteps;
    };
};

static QueueHandle_t    s_cmd_queue      = nullptr;
static SemaphoreHandle_t s_uart_mutex    = nullptr;
static volatile float   s_current_speed  = 0.0f;
static volatile bool    s_moving         = false;
static float            s_esteps         = MOTOR_E_STEPS_PER_MM;
static bool             s_esteps_valid   = false;
static float            s_cal_commanded  = 0.0f;   // Phase 1: gespeicherte Sollstrecke

static Preferences      s_prefs;

// ── NVS laden/speichern ──────────────────────────────────────

static void nvs_load() {
    s_prefs.begin("motor", true);
    s_esteps_valid = (s_prefs.getUChar("esteps_ok", 0) == 1);
    s_esteps       = s_prefs.getFloat("esteps", MOTOR_E_STEPS_PER_MM);
    s_prefs.end();
    Serial.printf("[MOTOR] E-Steps: %.2f Steps/mm (%s)\n",
                  s_esteps, s_esteps_valid ? "kalibriert" : "DEFAULT");
}

static void nvs_save_esteps(float val) {
    s_prefs.begin("motor", false);
    s_prefs.putFloat("esteps", val);
    s_prefs.putUChar("esteps_ok", 1);
    s_prefs.end();
}

// ── Bewegung ausführen ────────────────────────────────────────

static void do_move(float speed_mm_s, float steps_total, MotorDir dir) {
    uint32_t freq_hz = (uint32_t)(speed_mm_s * s_esteps);
    if (freq_hz < 1)    freq_hz = 1;
    if (freq_hz > 50000) freq_hz = 50000;

    // DIR setzen und Setup-Zeit abwarten
    digitalWrite(MOTOR_DIR_PIN, dir == MOTOR_DIR_FORWARD ? HIGH : LOW);
    ets_delay_us(20);

    // EN LOW (Motor aktiv)
    digitalWrite(MOTOR_EN_PIN, LOW);
    ets_delay_us(20);

    s_current_speed = speed_mm_s;
    s_moving        = true;

    motor_rmt_start((uint32_t)steps_total, freq_hz);
    motor_rmt_wait(portMAX_DELAY);

    s_moving        = false;
    s_current_speed = 0.0f;

    // Idle-Timeout: Motor nach MOTOR_IDLE_TIMEOUT_MS stromlos
    vTaskDelay(pdMS_TO_TICKS(MOTOR_IDLE_TIMEOUT_MS));
    if (!s_moving) digitalWrite(MOTOR_EN_PIN, HIGH);
}

// ── Motor-Manager-Task ────────────────────────────────────────

static void motor_task(void *arg) {
    MotorCmd cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        switch (cmd.type) {
            case CMD_MOVE: {
                float dist   = cmd.move.speed * cmd.move.duration;
                float steps  = dist * s_esteps;
                Serial.printf("[MOTOR] Move: %.2f mm/s × %.2f s = %.1f mm (%u steps @ %u Hz) %s\n",
                    cmd.move.speed, cmd.move.duration, dist, (unsigned)steps,
                    (unsigned)(cmd.move.speed * s_esteps),
                    cmd.move.dir == MOTOR_DIR_FORWARD ? "FWD" : "REV");
                do_move(cmd.move.speed, steps, cmd.move.dir);
                break;
            }
            case CMD_MOVE_DIST: {
                float steps = cmd.move_dist.distance * s_esteps;
                Serial.printf("[MOTOR] Move dist: %.2f mm @ %.2f mm/s (%u steps)\n",
                    cmd.move_dist.distance, cmd.move_dist.speed, (unsigned)steps);
                do_move(cmd.move_dist.speed, steps, cmd.move_dist.dir);
                break;
            }
            case CMD_STOP:
                motor_rmt_stop();
                s_moving = false;
                s_current_speed = 0.0f;
                break;

            case CMD_SET_CURRENT:
                if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
                tmc2208_set_current(cmd.current.run_ma, cmd.current.hold_ma);
                if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
                break;

            case CMD_SET_MICROSTEP:
                if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
                tmc2208_set_microstep(cmd.ms.microstep);
                if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
                break;

            case CMD_SET_STEALTHCHOP:
                if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
                tmc2208_set_stealthchop(cmd.flag.enable);
                if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
                break;

            case CMD_CAL_START: {
                s_cal_commanded = cmd.cal_start.distance;
                float steps = s_cal_commanded * s_esteps;
                Serial.printf("[MOTOR] E-Steps Kalibrierung Phase 1: %.1f mm extrudieren...\n",
                              s_cal_commanded);
                do_move(cmd.cal_start.speed, steps, MOTOR_DIR_FORWARD);
                Serial.println("[MOTOR] Fertig. Verbleibende Strecke messen und eingeben.");
                break;
            }
            case CMD_CAL_APPLY: {
                float actual = s_cal_commanded - cmd.cal_apply.remaining;
                if (actual <= 0.0f) {
                    Serial.println("[MOTOR] Kalibrierung: ungültige Restlänge!");
                    break;
                }
                float new_esteps = s_esteps * (s_cal_commanded / actual);
                Serial.printf("[MOTOR] E-Steps: alt=%.2f neu=%.2f (tatsächlich %.1f mm)\n",
                              s_esteps, new_esteps, actual);
                s_esteps       = new_esteps;
                s_esteps_valid = true;
                nvs_save_esteps(new_esteps);
                break;
            }
            case CMD_SET_ESTEPS:
                s_esteps       = cmd.esteps.steps_per_mm;
                s_esteps_valid = true;
                nvs_save_esteps(s_esteps);
                Serial.printf("[MOTOR] E-Steps manuell: %.2f Steps/mm\n", s_esteps);
                break;
        }
    }
}

// ── Öffentliche API ──────────────────────────────────────────

bool motor_init(SemaphoreHandle_t uart_mutex) {
    s_uart_mutex = uart_mutex;

    // GPIO konfigurieren
    pinMode(MOTOR_DIR_PIN, OUTPUT);
    pinMode(MOTOR_EN_PIN,  OUTPUT);
    digitalWrite(MOTOR_EN_PIN, HIGH);  // Motor anfangs stromlos

    // RMT initialisieren
    if (!motor_rmt_init(MOTOR_STEP_PIN)) return false;

    // TMC2208 UART
    SemaphoreHandle_t uart_mx = uart_mutex ? uart_mutex : xSemaphoreCreateMutex();
    if (!tmc2208_init(MOTOR_UART_TX, MOTOR_UART_RX, uart_mx)) {
        Serial.println("[MOTOR] TMC2208 nicht erreichbar – weiter ohne UART.");
    } else {
        tmc2208_set_current(MOTOR_CURRENT_MA, MOTOR_HOLD_CURRENT_MA);
        tmc2208_set_microstep(MOTOR_MICROSTEP);
        tmc2208_set_stealthchop(MOTOR_STEALTHCHOP);
        tmc2208_set_interpolation(true);
    }

    // NVS laden
    nvs_load();

    // Queue & Task
    s_cmd_queue = xQueueCreate(8, sizeof(MotorCmd));
    xTaskCreatePinnedToCore(motor_task, "motor_mgr", TASK_STACK_MOTOR,
                            nullptr, TASK_PRIO_MOTOR, nullptr, tskNO_AFFINITY);
    Serial.println("[MOTOR] Initialisiert.");
    return true;
}

bool motor_move(float speed_mm_s, float duration_s, MotorDir dir) {
    if (speed_mm_s <= 0 || duration_s <= 0) return false;
    MotorCmd cmd;
    cmd.type          = CMD_MOVE;
    cmd.move.speed    = speed_mm_s;
    cmd.move.duration = duration_s;
    cmd.move.dir      = dir;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool motor_move_distance(float speed_mm_s, float distance_mm, MotorDir dir) {
    if (speed_mm_s <= 0 || distance_mm <= 0) return false;
    MotorCmd cmd;
    cmd.type               = CMD_MOVE_DIST;
    cmd.move_dist.speed    = speed_mm_s;
    cmd.move_dist.distance = distance_mm;
    cmd.move_dist.dir      = dir;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

void motor_stop() {
    MotorCmd cmd; cmd.type = CMD_STOP;
    xQueueSendToFront(s_cmd_queue, &cmd, pdMS_TO_TICKS(10));
}

bool motor_wait_done(uint32_t timeout_ms) {
    return motor_rmt_wait(timeout_ms);
}

bool  motor_is_moving()          { return s_moving; }
float motor_get_current_speed()  { return s_current_speed; }

bool motor_set_current(uint16_t run_ma, uint16_t hold_ma) {
    MotorCmd cmd;
    cmd.type           = CMD_SET_CURRENT;
    cmd.current.run_ma  = run_ma;
    cmd.current.hold_ma = (hold_ma == 0) ? run_ma / 2 : hold_ma;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool motor_set_microstep(uint8_t microstep) {
    MotorCmd cmd; cmd.type = CMD_SET_MICROSTEP; cmd.ms.microstep = microstep;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool motor_set_stealthchop(bool enable) {
    MotorCmd cmd; cmd.type = CMD_SET_STEALTHCHOP; cmd.flag.enable = enable;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool motor_set_interpolation(bool enable) {
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    bool ok = tmc2208_set_interpolation(enable);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
    return ok;
}

bool motor_get_tmc_status(TMC2208Status *status) {
    if (s_uart_mutex) xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    bool ok = tmc2208_read_status(status);
    if (s_uart_mutex) xSemaphoreGive(s_uart_mutex);
    return ok;
}

bool motor_calibrate_start(float distance_mm, float speed_mm_s) {
    MotorCmd cmd;
    cmd.type             = CMD_CAL_START;
    cmd.cal_start.distance = distance_mm;
    cmd.cal_start.speed    = speed_mm_s;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool motor_calibrate_apply(float remaining_mm) {
    MotorCmd cmd;
    cmd.type              = CMD_CAL_APPLY;
    cmd.cal_apply.remaining = remaining_mm;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

float motor_get_esteps()             { return s_esteps; }
bool  motor_esteps_is_calibrated()   { return s_esteps_valid; }

bool motor_set_esteps(float steps_per_mm) {
    MotorCmd cmd;
    cmd.type             = CMD_SET_ESTEPS;
    cmd.esteps.steps_per_mm = steps_per_mm;
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}
