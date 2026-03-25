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
static volatile float s_remaining_s  = 0.0f;
static volatile bool s_move_queued   = false;  // Move bereits in Motor-Queue
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
            vTaskDelay(pdMS_TO_TICKS(2000));  // 2s Nachlauf für Messdaten
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

        // Aufheizen überspringen wenn Temperatur bereits im Zielbereich
        float cur_temp = hotend_get_temperature();
        if (fabsf(cur_temp - seq.temperature_c) <= SEQ_TEMP_TOLERANCE) {
            Serial.printf("[SEQ] Sequenz %d: Temperatur %.1f °C bereits erreicht.\n", idx + 1, seq.temperature_c);
        } else {
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
        }
        if (s_stop_req) {
            motor_stop(); datalog_stop(); hotend_set_target(0);
            s_state = SEQ_IDLE; s_active_idx = -1; s_stop_req = false;
            Serial.println("[SEQ] Gestoppt.");
            continue;
        }

        // ── RUNNING ─────────────────────────────────────────
        s_state = SEQ_RUNNING;
        Serial.printf("[SEQ] Sequenz %d: Extrudieren %.2f mm/s für %.1f s\n",
                      idx + 1, seq.speed_mm_s, seq.duration_s);

        s_remaining_s = seq.duration_s;
        if (!s_move_queued) {
            motor_move(seq.speed_mm_s, seq.duration_s, MOTOR_DIR_FORWARD);
        }
        s_move_queued = false;

        // Warten bis motor_task den Befehl übernommen hat (s_moving wird gesetzt)
        {
            TickType_t t0 = xTaskGetTickCount();
            while (!motor_is_moving() && !s_stop_req) {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(2000)) break;
            }
        }
        // Warten bis Bewegung beendet ist, Restzeit aktualisieren
        {
            unsigned long run_start = millis();
            uint32_t timeout_ms = (uint32_t)(seq.duration_s * 1000.0f + 5000);
            TickType_t t0 = xTaskGetTickCount();
            while (motor_is_moving() && !s_stop_req) {
                float elapsed = (millis() - run_start) / 1000.0f;
                s_remaining_s = seq.duration_s - elapsed;
                if (s_remaining_s < 0) s_remaining_s = 0;
                vTaskDelay(pdMS_TO_TICKS(100));
                if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms)) break;
            }
            s_remaining_s = 0;
        }

        if (s_stop_req) {
            motor_stop(); datalog_stop(); hotend_set_target(0);
            s_state = SEQ_IDLE; s_active_idx = -1; s_stop_req = false;
            Serial.println("[SEQ] Gestoppt.");
            continue;
        }

        Serial.printf("[SEQ] Sequenz %d abgeschlossen.\n", idx + 1);

        // ── NEXT ─────────────────────────────────────────────
        s_state = SEQ_NEXT;
        portENTER_CRITICAL(&s_mux);
        s_active_idx = idx + 1;
        portEXIT_CRITICAL(&s_mux);

        // Nächste Sequenz: Move sofort in Queue schieben wenn gleiche Temp
        int next = idx + 1;
        if (next < s_seq_count) {
            float next_temp = s_sequences[next].temperature_c;
            float cur = hotend_get_temperature();
            if (fabsf(cur - next_temp) <= SEQ_TEMP_TOLERANCE) {
                // Gleiche Temperatur → Move vorab in Queue, minimiert Pause
                motor_move(s_sequences[next].speed_mm_s, s_sequences[next].duration_s, MOTOR_DIR_FORWARD);
                s_move_queued = true;
            }
        }
    }  // for(;;) – Task kehrt nie zurück
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

bool sequencer_start(const char *filename) {
    if (s_state != SEQ_IDLE || s_seq_count == 0) return false;
    s_stop_req    = false;
    s_move_queued = false;
    s_active_idx  = 0;
    s_state       = SEQ_HEATING;

    // Sequenz-Tabelle als Preamble in CSV schreiben
    char preamble[512];
    int pos = 0;
    pos += snprintf(preamble + pos, sizeof(preamble) - pos,
                    "# Messreihen\n# Nr,Temp_C,Speed_mm_s,Dauer_s\n");
    for (int i = 0; i < s_seq_count && pos < (int)sizeof(preamble) - 40; i++) {
        pos += snprintf(preamble + pos, sizeof(preamble) - pos,
                        "# %d,%.1f,%.2f,%.1f\n",
                        i + 1, s_sequences[i].temperature_c,
                        s_sequences[i].speed_mm_s, s_sequences[i].duration_s);
    }
    pos += snprintf(preamble + pos, sizeof(preamble) - pos, "#\n");
    datalog_set_preamble(preamble);

    datalog_start(SEQ_LOG_INTERVAL_MS, filename);
    return true;
}

void sequencer_stop() {
    s_stop_req = true;
    motor_stop();
}

SeqState    sequencer_get_state()        { return s_state; }
int         sequencer_get_active_index() { return s_active_idx; }
float       sequencer_get_remaining_s()  { return s_remaining_s; }

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
