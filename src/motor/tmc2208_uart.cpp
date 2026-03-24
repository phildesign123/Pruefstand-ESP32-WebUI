#include "tmc2208_uart.h"
#include "../config.h"
#include <TMCStepper.h>
#include "tmc2208_regs.h"

// =============================================================
// TMC2208/2209 UART-Treiber via TMCStepper-Library
// Nutzt Serial2 (HardwareSerial) wie funktionierende Referenz
// =============================================================

#define DRIVER_ADDRESS  0b00
#define R_SENSE         0.11f

static TMC2209Stepper *s_driver = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;

// ── Öffentliche API ──────────────────────────────────────────

bool tmc2208_init(int tx_pin, int rx_pin, SemaphoreHandle_t uart_mutex) {
    s_mutex = uart_mutex;

    Serial2.begin(MOTOR_UART_BAUD, SERIAL_8N1, rx_pin, tx_pin);
    delay(200);

    s_driver = new TMC2209Stepper(&Serial2, R_SENSE, DRIVER_ADDRESS);
    s_driver->begin();
    s_driver->toff(5);
    s_driver->pdn_disable(true);
    s_driver->mstep_reg_select(true);

    // Ping: IOIN lesen
    uint32_t ioin = s_driver->IOIN();
    if (ioin == 0 || ioin == 0xFFFFFFFF) {
        Serial.println("[TMC2208] FEHLER: Keine Kommunikation!");
        return false;
    }
    Serial.printf("[TMC2208] Verbindung OK (IOIN=0x%08X).\n", ioin);
    return true;
}

bool tmc2208_write_reg(uint8_t reg, uint32_t value) {
    // Generischer Register-Zugriff nicht verfügbar über TMCStepper
    // Nutze die spezifischen Funktionen (set_current, set_microstep, etc.)
    return s_driver != nullptr;
}

bool tmc2208_read_reg(uint8_t reg, uint32_t *value) {
    // Generischer Register-Zugriff nicht verfügbar über TMCStepper
    *value = 0;
    return s_driver != nullptr;
}

bool tmc2208_set_current(uint16_t run_ma, uint16_t hold_ma) {
    if (!s_driver) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    float hold_ratio = (hold_ma > 0) ? (float)hold_ma / (float)run_ma : 0.5f;
    s_driver->rms_current(run_ma, hold_ratio);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

bool tmc2208_set_microstep(uint8_t ms) {
    if (!s_driver) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_driver->microsteps(ms);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

bool tmc2208_set_stealthchop(bool enable) {
    if (!s_driver) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_driver->en_spreadCycle(!enable);
    if (enable) {
        s_driver->pwm_autoscale(true);
        s_driver->TPWMTHRS(0);
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

bool tmc2208_set_interpolation(bool enable) {
    if (!s_driver) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_driver->intpol(enable);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

bool tmc2208_read_config(TMC2208Config *cfg) {
    if (!s_driver) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    cfg->run_current_ma  = s_driver->rms_current();
    cfg->hold_current_ma = (uint16_t)(s_driver->rms_current() * s_driver->hold_multiplier());
    cfg->microsteps      = s_driver->microsteps();
    cfg->stealthchop     = !s_driver->en_spreadCycle();
    cfg->interpolation   = s_driver->intpol();
    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

bool tmc2208_read_status(TMC2208Status *status) {
    if (!s_driver) return false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t drv = s_driver->DRV_STATUS();
    if (s_mutex) xSemaphoreGive(s_mutex);

    if (drv == 0 || drv == 0xFFFFFFFF) return false;

    status->ot_warning         = drv & DRV_STATUS_OTPW;
    status->ot_shutdown        = drv & DRV_STATUS_OT;
    status->short_a            = drv & DRV_STATUS_S2GA;
    status->short_b            = drv & DRV_STATUS_S2GB;
    status->open_load_a        = drv & DRV_STATUS_OLA;
    status->open_load_b        = drv & DRV_STATUS_OLB;
    status->stealthchop_active = drv & DRV_STATUS_STEALTH;
    status->standstill         = drv & DRV_STATUS_STST;
    status->cs_actual          = (drv & DRV_STATUS_CS_ACTUAL_MASK) >> 16;
    return true;
}

bool tmc2208_ping() {
    if (!s_driver) return false;
    uint32_t ioin = s_driver->IOIN();
    return (ioin != 0 && ioin != 0xFFFFFFFF);
}
