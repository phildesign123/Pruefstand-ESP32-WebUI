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
static bool             s_dir_invert     = false;
static float            s_cal_commanded  = 0.0f;   // Phase 1: gespeicherte Sollstrecke

static Preferences      s_prefs;

// ── NVS laden/speichern ──────────────────────────────────────

static void nvs_load() {
    s_prefs.begin("motor", true);
    s_esteps_valid = (s_prefs.getUChar("esteps_ok", 0) == 1);
    s_esteps       = s_prefs.getFloat("esteps", MOTOR_E_STEPS_PER_MM);
    s_dir_invert   = (s_prefs.getUChar("dir_inv", 0) == 1);

    // TMC2208 Einstellungen aus NVS wiederherstellen
    uint16_t run_ma  = s_prefs.getUShort("tmc_run",  0);
    uint16_t hold_ma = s_prefs.getUShort("tmc_hold", 0);
    uint16_t ms      = s_prefs.getUShort("tmc_ms",   0);
    uint8_t  sc      = s_prefs.getUChar("tmc_sc",   0xFF);
    uint8_t  intpol  = s_prefs.getUChar("tmc_intpol", 0xFF);
    s_prefs.end();

    if (run_ma > 0)    tmc2208_set_current(run_ma, hold_ma);
    if (ms > 0)        tmc2208_set_microstep((uint8_t)ms);
    if (sc != 0xFF)    tmc2208_set_stealthchop(sc == 1);
    if (intpol != 0xFF) tmc2208_set_interpolation(intpol == 1);
    Serial.printf("[MOTOR] E-Steps: %.2f Steps/mm (%s)\n",
                  s_esteps, s_esteps_valid ? "kalibriert" : "DEFAULT");
}

static void nvs_save_tmc_val(const char *key_u16, uint16_t val16,
                              const char *key_u8, uint8_t val8) {
    s_prefs.begin("motor", false);
    if (key_u16) s_prefs.putUShort(key_u16, val16);
    if (key_u8)  s_prefs.putUChar(key_u8, val8);
    s_prefs.end();
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
    bool fwd = (dir == MOTOR_DIR_FORWARD) ^ s_dir_invert;
    digitalWrite(MOTOR_DIR_PIN, fwd ? HIGH : LOW);
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
}

// ── Motor-Manager-Task ────────────────────────────────────────

static void motor_task(void *arg) {
    MotorCmd cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(MOTOR_IDLE_TIMEOUT_MS)) != pdTRUE) {
            // Timeout: kein neuer Befehl → Motor stromlos
            if (!s_moving) digitalWrite(MOTOR_EN_PIN, HIGH);
            // Nächsten Befehl unbegrenzt abwarten
            if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        }

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
                tmc2208_set_current(cmd.current.run_ma, cmd.current.hold_ma);
                break;

            case CMD_SET_MICROSTEP:
                tmc2208_set_microstep(cmd.ms.microstep);
                break;

            case CMD_SET_STEALTHCHOP:
                tmc2208_set_stealthchop(cmd.flag.enable);
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
        // Standardwerte setzen
        tmc2208_set_current(MOTOR_CURRENT_MA, MOTOR_HOLD_CURRENT_MA);
        tmc2208_set_microstep(MOTOR_MICROSTEP);
        tmc2208_set_stealthchop(MOTOR_STEALTHCHOP);
        tmc2208_set_interpolation(true);
    }

    // NVS laden (überschreibt TMC-Standardwerte falls gespeichert)
    nvs_load();

    // Queue & Task
    s_cmd_queue = xQueueCreate(8, sizeof(MotorCmd));
    xTaskCreatePinnedToCore(motor_task, "motor_mgr", TASK_STACK_MOTOR,
                            nullptr, TASK_PRIO_MOTOR, nullptr, CORE_REALTIME);
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
    // RMT sofort stoppen – befreit do_move() aus motor_rmt_wait()
    motor_rmt_stop();
    // CMD_STOP in Queue damit motor_task die Flags aufräumt
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
    bool ok = xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
    if (ok) {
        s_prefs.begin("motor", false);
        s_prefs.putUShort("tmc_run", run_ma);
        s_prefs.putUShort("tmc_hold", cmd.current.hold_ma);
        s_prefs.end();
    }
    return ok;
}

bool motor_set_microstep(uint8_t microstep) {
    // Aktuelle Mikroschritte lesen für E-Steps-Anpassung
    TMC2208Config cfg;
    uint16_t old_ms = MOTOR_MICROSTEP;
    if (tmc2208_read_config(&cfg)) old_ms = cfg.microsteps;

    MotorCmd cmd; cmd.type = CMD_SET_MICROSTEP; cmd.ms.microstep = microstep;
    bool ok = xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
    if (ok) {
        nvs_save_tmc_val("tmc_ms", microstep, nullptr, 0);
        // E-Steps proportional anpassen damit Geschwindigkeit gleich bleibt
        if (old_ms > 0 && old_ms != microstep) {
            float new_esteps = s_esteps * (float)microstep / (float)old_ms;
            s_esteps = new_esteps;
            s_esteps_valid = true;
            nvs_save_esteps(s_esteps);
            Serial.printf("[MOTOR] Mikroschritte %u→%u, E-Steps angepasst: %.2f\n",
                          old_ms, microstep, s_esteps);
        }
    }
    return ok;
}

bool motor_set_stealthchop(bool enable) {
    MotorCmd cmd; cmd.type = CMD_SET_STEALTHCHOP; cmd.flag.enable = enable;
    bool ok = xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
    if (ok) nvs_save_tmc_val(nullptr, 0, "tmc_sc", enable ? 1 : 0);
    return ok;
}

bool motor_set_interpolation(bool enable) {
    bool ok = tmc2208_set_interpolation(enable);
    if (ok) nvs_save_tmc_val(nullptr, 0, "tmc_intpol", enable ? 1 : 0);
    return ok;
}

bool motor_get_tmc_status(TMC2208Status *status) {
    return tmc2208_read_status(status);
}

bool motor_get_tmc_config(TMC2208Config *cfg) {
    return tmc2208_read_config(cfg);
}

bool motor_get_dir_invert() { return s_dir_invert; }

void motor_set_dir_invert(bool invert) {
    s_dir_invert = invert;
    s_prefs.begin("motor", false);
    s_prefs.putUChar("dir_inv", invert ? 1 : 0);
    s_prefs.end();
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
