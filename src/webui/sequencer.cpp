#include "sequencer.h"
#include "../config.h"
#include "../hotend/hotend.h"
#include "../motor/motor.h"
#include "../datalog/datalog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// =============================================================
// Messreihen-Sequencer: IDLE → HEATING → RUNNING → NEXT → IDLE
// =============================================================

static Sequence      s_sequences[SEQ_MAX_SEQUENCES];
static int           s_seq_count     = 0;
static volatile int  s_active_idx    = -1;
static volatile SeqState s_state     = SEQ_IDLE;
static volatile bool s_stop_req      = false;
static portMUX_TYPE  s_mux           = portMUX_INITIALIZER_UNLOCKED;

static void sequencer_task(void *arg) {
    for (;;) {
        if (s_state == SEQ_IDLE || s_stop_req) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int idx;
        portENTER_CRITICAL(&s_mux);
        idx = s_active_idx;
        portEXIT_CRITICAL(&s_mux);

        if (idx < 0 || idx >= s_seq_count) {
            s_state     = SEQ_DONE;
            s_active_idx = -1;
            datalog_stop();
            hotend_set_target(0);
            Serial.println("[SEQ] Messreihe abgeschlossen.");
            s_state = SEQ_IDLE;
            continue;
        }

        Sequence &seq = s_sequences[idx];

        // ── HEATING ─────────────────────────────────────────
        s_state = SEQ_HEATING;
        hotend_set_target(seq.temperature_c);
        Serial.printf("[SEQ] Sequenz %d: Aufheizen auf %.1f °C...\n", idx + 1, seq.temperature_c);

        unsigned long heat_start = millis();
        unsigned long stable_since = 0;

        while (!s_stop_req) {
            float temp = hotend_get_temperature();
            float diff = fabsf(temp - seq.temperature_c);

            if (diff <= SEQ_TEMP_TOLERANCE) {
                if (stable_since == 0) stable_since = millis();
                if (millis() - stable_since >= SEQ_TEMP_STABLE_TIME_S * 1000UL) break;
            } else {
                stable_since = 0;
            }

            if (millis() - heat_start >= SEQ_HEATING_TIMEOUT_S * 1000UL) {
                Serial.printf("[SEQ] Aufheiz-Timeout bei Sequenz %d!\n", idx + 1);
                s_state     = SEQ_ERROR;
                s_stop_req  = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (s_stop_req) break;

        // ── RUNNING ─────────────────────────────────────────
        s_state = SEQ_RUNNING;
        Serial.printf("[SEQ] Sequenz %d: Extrudieren %.2f mm/s für %.1f s\n",
                      idx + 1, seq.speed_mm_s, seq.duration_s);

        motor_move(seq.speed_mm_s, seq.duration_s, MOTOR_DIR_FORWARD);

        // Warten bis motor_task den Befehl übernommen hat (s_moving wird gesetzt)
        {
            TickType_t t0 = xTaskGetTickCount();
            while (!motor_is_moving() && !s_stop_req) {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(2000)) break;
            }
        }
        // Warten bis Bewegung beendet ist
        {
            uint32_t timeout_ms = (uint32_t)(seq.duration_s * 1000.0f + 5000);
            TickType_t t0 = xTaskGetTickCount();
            while (motor_is_moving() && !s_stop_req) {
                vTaskDelay(pdMS_TO_TICKS(100));
                if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms)) break;
            }
        }

        if (s_stop_req) break;

        Serial.printf("[SEQ] Sequenz %d abgeschlossen.\n", idx + 1);

        // ── NEXT ─────────────────────────────────────────────
        s_state = SEQ_NEXT;
        portENTER_CRITICAL(&s_mux);
        s_active_idx = idx + 1;
        portEXIT_CRITICAL(&s_mux);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Aufräumen bei Stop
    motor_stop();
    datalog_stop();
    hotend_set_target(0);
    s_state     = SEQ_IDLE;
    s_active_idx = -1;
    Serial.println("[SEQ] Gestoppt.");
}

void sequencer_init() {
    xTaskCreatePinnedToCore(sequencer_task, "sequencer", TASK_STACK_SEQUENCER,
                            nullptr, TASK_PRIO_SEQUENCER, nullptr, CORE_REALTIME);
}

bool sequencer_add(float temp_c, float speed_mm_s, float duration_s) {
    if (s_seq_count >= SEQ_MAX_SEQUENCES) return false;
    s_sequences[s_seq_count++] = {temp_c, speed_mm_s, duration_s};
    return true;
}

bool sequencer_delete(int index) {
    if (index < 0 || index >= s_seq_count) return false;
    for (int i = index; i < s_seq_count - 1; i++) s_sequences[i] = s_sequences[i + 1];
    s_seq_count--;
    return true;
}

void sequencer_clear() { s_seq_count = 0; }

bool sequencer_reorder(const int *order, int count) {
    if (count != s_seq_count) return false;
    Sequence tmp[SEQ_MAX_SEQUENCES];
    for (int i = 0; i < count; i++) {
        if (order[i] < 0 || order[i] >= count) return false;
        tmp[i] = s_sequences[order[i]];
    }
    memcpy(s_sequences, tmp, count * sizeof(Sequence));
    return true;
}

int  sequencer_count()     { return s_seq_count; }
bool sequencer_get(int i, Sequence *s) {
    if (i < 0 || i >= s_seq_count) return false;
    *s = s_sequences[i]; return true;
}

bool sequencer_start() {
    if (s_state != SEQ_IDLE || s_seq_count == 0) return false;
    s_stop_req   = false;
    s_active_idx = 0;
    s_state      = SEQ_HEATING;
    datalog_start(SEQ_LOG_INTERVAL_MS);
    return true;
}

void sequencer_stop() {
    s_stop_req = true;
    motor_stop();
}

SeqState    sequencer_get_state()        { return s_state; }
int         sequencer_get_active_index() { return s_active_idx; }

const char* sequencer_state_string() {
    switch (s_state) {
        case SEQ_IDLE:    return "idle";
        case SEQ_HEATING: return "heating";
        case SEQ_RUNNING: return "running";
        case SEQ_NEXT:    return "next";
        case SEQ_DONE:    return "done";
        case SEQ_ERROR:   return "error";
        default:          return "unknown";
    }
}
