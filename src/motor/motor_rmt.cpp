#include "motor_rmt.h"
#include "../config.h"
#include "driver/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// =============================================================
// RMT-basierte Step-Pulserzeugung für TMC2208
// Clock: 80 MHz / 80 = 1 MHz → 1 µs Auflösung
// Pulse: HIGH 2 µs + LOW (Periode - 2) µs
//
// Strategie: Steps in Blöcken von 64 senden (RMT-Buffer-Größe).
// Nach jedem Block prüft der Task ob Stop angefordert wurde.
// Max Latenz bei 14.88 kHz: 64 / 14880 ≈ 4 ms – akzeptabel.
// =============================================================

#define RMT_CHANNEL     RMT_CHANNEL_0
#define RMT_CLK_DIV     80                // 1 MHz = 1 µs pro Tick
#define RMT_BLOCK_SIZE  64                // Puffer-Items pro Transfer

static volatile bool    s_running       = false;
static volatile bool    s_stop_req      = false;
static volatile int32_t s_steps_total   = 0;
static volatile int32_t s_steps_done    = 0;
static volatile uint32_t s_period_us    = 0;
static SemaphoreHandle_t s_done_sem     = nullptr;
static TaskHandle_t      s_rmt_task_hdl = nullptr;

// ── ISR-Callback: nach Abschluss eines Transfers ────────────

static void IRAM_ATTR rmt_done_callback(rmt_channel_t ch, void *arg) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// ── Interne Task: gibt Blöcke von Steps aus ─────────────────

static void rmt_step_task(void *arg) {
    rmt_item32_t items[RMT_BLOCK_SIZE];

    while (true) {
        // Warten auf Start-Signal (vom motor_rmt_start)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        s_steps_done = 0;
        s_running    = true;
        s_stop_req   = false;

        int32_t remaining = s_steps_total;

        while (remaining > 0 && !s_stop_req) {
            int32_t block = (remaining > RMT_BLOCK_SIZE) ? RMT_BLOCK_SIZE : remaining;

            // Block befüllen: alle Items identisch
            uint16_t high_us = 2;
            uint16_t low_us  = (uint16_t)(s_period_us - high_us);
            if (low_us < 2) low_us = 2;

            for (int i = 0; i < block; i++) {
                items[i].level0    = 1;
                items[i].duration0 = high_us;
                items[i].level1    = 0;
                items[i].duration1 = low_us;
            }

            // Transfer starten (nicht-blockierend)
            xSemaphoreTake(s_done_sem, 0);  // alten Wert leeren
            rmt_write_items(RMT_CHANNEL, items, block, false);

            // Warten bis Block gesendet
            if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(1000)) == pdFALSE) break;

            remaining    -= block;
            s_steps_done += block;
        }

        // Kanal leerlaufen lassen
        rmt_wait_tx_done(RMT_CHANNEL, pdMS_TO_TICKS(100));

        s_running = false;

        // Haupt-Semaphore freigeben (motor_rmt_wait unblockiert)
        xSemaphoreGive(s_done_sem);
    }
}

// ── Öffentliche API ──────────────────────────────────────────

bool motor_rmt_init(int step_pin) {
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) return false;

    rmt_config_t cfg = {};
    cfg.rmt_mode                = RMT_MODE_TX;
    cfg.channel                 = RMT_CHANNEL;
    cfg.gpio_num                = (gpio_num_t)step_pin;
    cfg.clk_div                 = RMT_CLK_DIV;
    cfg.mem_block_num           = 1;
    cfg.tx_config.loop_en       = false;
    cfg.tx_config.carrier_en    = false;
    cfg.tx_config.idle_level    = RMT_IDLE_LEVEL_LOW;
    cfg.tx_config.idle_output_en = true;

    if (rmt_config(&cfg) != ESP_OK)           return false;
    if (rmt_driver_install(RMT_CHANNEL, 0, 0) != ESP_OK) return false;
    rmt_register_tx_end_callback(rmt_done_callback, nullptr);

    // Interne Task erstellen
    xTaskCreatePinnedToCore(rmt_step_task, "rmt_step", 2048,
                            nullptr, TASK_PRIO_MOTOR + 1,
                            &s_rmt_task_hdl, CORE_REALTIME);
    Serial.println("[RMT] Initialisiert (1 MHz, CH0).");
    return true;
}

bool motor_rmt_start(uint32_t steps, uint32_t freq_hz) {
    if (s_running) return false;
    if (steps == 0 || freq_hz == 0) return false;

    s_steps_total = (int32_t)steps;
    s_period_us   = 1000000UL / freq_hz;
    if (s_period_us < 4) s_period_us = 4;  // Minimum: 4 µs

    // Task benachrichtigen
    xTaskNotifyGive(s_rmt_task_hdl);
    return true;
}

void motor_rmt_stop() {
    s_stop_req = true;
    rmt_tx_stop(RMT_CHANNEL);
}

bool motor_rmt_wait(uint32_t timeout_ms) {
    if (!s_running) return true;
    return xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool motor_rmt_is_running() { return s_running; }
