#pragma once
#include <Arduino.h>
#include "freertos/semphr.h"
#include "tmc2208_uart.h"

enum MotorDir : uint8_t {
    MOTOR_DIR_FORWARD = 0,
    MOTOR_DIR_REVERSE = 1,
};

// Modul initialisieren (RMT, GPIO, UART, NVS, Task)
bool motor_init(SemaphoreHandle_t uart_mutex = nullptr);

// ── Bewegung ─────────────────────────────────────────────────
bool motor_move(float speed_mm_s, float duration_s, MotorDir dir = MOTOR_DIR_FORWARD);
bool motor_move_distance(float speed_mm_s, float distance_mm, MotorDir dir = MOTOR_DIR_FORWARD);
void motor_stop();
bool motor_wait_done(uint32_t timeout_ms = portMAX_DELAY);
bool motor_is_moving();

// ── Status ───────────────────────────────────────────────────
float motor_get_current_speed();   // mm/s (0 wenn nicht aktiv)

// ── TMC2208 Konfiguration ────────────────────────────────────
bool motor_set_current(uint16_t run_ma, uint16_t hold_ma = 0);
bool motor_set_microstep(uint8_t microstep);
bool motor_set_stealthchop(bool enable);
bool motor_set_interpolation(bool enable);
bool motor_get_tmc_status(TMC2208Status *status);
bool motor_get_tmc_config(TMC2208Config *cfg);
bool motor_get_dir_invert();
void motor_set_dir_invert(bool invert);

// ── E-Steps Kalibrierung ─────────────────────────────────────
bool  motor_calibrate_start(float distance_mm, float speed_mm_s);
bool  motor_calibrate_apply(float remaining_mm);
float motor_get_esteps();
bool  motor_set_esteps(float steps_per_mm);
bool  motor_esteps_is_calibrated();
