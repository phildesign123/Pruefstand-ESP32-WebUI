#include "tmc2208_uart.h"
#include "tmc2208_regs.h"
#include "../config.h"

// =============================================================
// TMC2208 UART-Treiber
// Protokoll: 8N1, 115200 Baud, eigenes Trinamic-Frame-Format
// Frame (Write): SYNC(0x05) | ADDR(0x00) | REG|0x80 | DATA[3:0] | CRC
// Frame (Read):  SYNC(0x05) | ADDR(0x00) | REG        | CRC
//                → Antwort: SYNC | ADDR | REG | DATA[3:0] | CRC
// =============================================================

static HardwareSerial  *s_serial    = nullptr;
static SemaphoreHandle_t s_mutex    = nullptr;

// ── CRC8 nach Trinamic ───────────────────────────────────────

static uint8_t tmc_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int b = 0; b < 8; b++) {
            if ((crc >> 7) ^ (byte & 0x01)) crc = (crc << 1) ^ 0x07;
            else                             crc <<= 1;
            byte >>= 1;
        }
    }
    return crc;
}

// ── Low-Level Frame-Operationen ──────────────────────────────

static bool write_frame(uint8_t reg, uint32_t val) {
    uint8_t frame[8];
    frame[0] = TMC2208_SYNC;
    frame[1] = TMC2208_SLAVE_ADDR;
    frame[2] = reg | 0x80;
    frame[3] = (val >> 24) & 0xFF;
    frame[4] = (val >> 16) & 0xFF;
    frame[5] = (val >>  8) & 0xFF;
    frame[6] = (val >>  0) & 0xFF;
    frame[7] = tmc_crc8(frame, 7);

    s_serial->write(frame, 8);
    s_serial->flush();

    // Echo des eigenen Frames lesen und verwerfen (single-wire)
    unsigned long t = millis();
    while (s_serial->available() < 8) {
        if (millis() - t > 20) break;
        delay(1);
    }
    while (s_serial->available()) s_serial->read();
    return true;
}

static bool read_frame(uint8_t reg, uint32_t *val) {
    // Request senden
    uint8_t req[4];
    req[0] = TMC2208_SYNC;
    req[1] = TMC2208_SLAVE_ADDR;
    req[2] = reg & 0x7F;
    req[3] = tmc_crc8(req, 3);

    while (s_serial->available()) s_serial->read();  // RX-Buffer leeren
    s_serial->write(req, 4);
    s_serial->flush();

    // Echo verwerfen (4 Byte)
    unsigned long t = millis();
    while (s_serial->available() < 4) {
        if (millis() - t > 10) return false;
        delay(1);
    }
    for (int i = 0; i < 4; i++) s_serial->read();

    // Antwort lesen (8 Byte)
    t = millis();
    while (s_serial->available() < 8) {
        if (millis() - t > 20) return false;
        delay(1);
    }

    uint8_t resp[8];
    for (int i = 0; i < 8; i++) resp[i] = s_serial->read();

    // CRC prüfen
    uint8_t crc_calc = tmc_crc8(resp, 7);
    if (crc_calc != resp[7]) return false;

    *val = ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16) |
           ((uint32_t)resp[5] <<  8) | ((uint32_t)resp[6]);
    return true;
}

// ── Öffentliche API ──────────────────────────────────────────

bool tmc2208_init(int tx_pin, int rx_pin, SemaphoreHandle_t uart_mutex) {
    s_mutex = uart_mutex;

    // UART2 für TMC2208
    s_serial = new HardwareSerial(2);
    s_serial->begin(MOTOR_UART_BAUD, SERIAL_8N1, rx_pin, tx_pin);
    delay(100);

    // PDN_DISABLE setzen (UART-Modus) + MSTEP_REG_SELECT
    uint32_t gconf = GCONF_PDN_DISABLE | GCONF_MSTEP_REG_SELECT | GCONF_MULTISTEP_FILT;
    if (!tmc2208_write_reg(TMC2208_REG_GCONF, gconf)) return false;

    if (!tmc2208_ping()) {
        Serial.println("[TMC2208] FEHLER: Keine Kommunikation!");
        return false;
    }
    Serial.println("[TMC2208] Verbindung OK.");
    return true;
}

bool tmc2208_write_reg(uint8_t reg, uint32_t value) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = write_frame(reg, value);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ok;
}

bool tmc2208_read_reg(uint8_t reg, uint32_t *value) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = read_frame(reg, value);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ok;
}

bool tmc2208_set_current(uint16_t run_ma, uint16_t hold_ma) {
    // I_RMS = (IRUN + 1) / 32 × V_FS / R_SENSE / sqrt(2)
    // Für R_SENSE = 0.11 Ω, V_FS = 0.325 V:
    // IRUN = (run_ma * 32 * 1.414 * 0.11) / 325 - 1
    uint8_t irun  = (uint8_t)constrain((int)((run_ma  * 32.0f * 1.41421f * 0.11f) / 325.0f) - 1, 0, 31);
    uint8_t ihold = (uint8_t)constrain((int)((hold_ma * 32.0f * 1.41421f * 0.11f) / 325.0f) - 1, 0, 31);
    uint8_t iholddelay = 6;

    uint32_t reg = ((uint32_t)iholddelay << IHOLDDELAY_SHIFT) |
                   ((uint32_t)irun       << IRUN_SHIFT)       |
                   (ihold);
    return tmc2208_write_reg(TMC2208_REG_IHOLD_IRUN, reg);
}

bool tmc2208_set_microstep(uint8_t ms) {
    uint32_t chopconf = 0;
    if (!tmc2208_read_reg(TMC2208_REG_CHOPCONF, &chopconf)) return false;

    chopconf &= ~CHOPCONF_MRES_MASK;
    chopconf |= (uint32_t)microstep_to_mres(ms) << CHOPCONF_MRES_SHIFT;
    return tmc2208_write_reg(TMC2208_REG_CHOPCONF, chopconf);
}

bool tmc2208_set_stealthchop(bool enable) {
    uint32_t gconf = 0;
    if (!tmc2208_read_reg(TMC2208_REG_GCONF, &gconf)) return false;

    if (enable) gconf &= ~GCONF_EN_SPREADCYCLE;  // 0 = StealthChop
    else        gconf |=  GCONF_EN_SPREADCYCLE;  // 1 = SpreadCycle
    return tmc2208_write_reg(TMC2208_REG_GCONF, gconf);
}

bool tmc2208_set_interpolation(bool enable) {
    uint32_t chopconf = 0;
    if (!tmc2208_read_reg(TMC2208_REG_CHOPCONF, &chopconf)) return false;

    if (enable) chopconf |=  CHOPCONF_INTPOL;
    else        chopconf &= ~CHOPCONF_INTPOL;
    return tmc2208_write_reg(TMC2208_REG_CHOPCONF, chopconf);
}

bool tmc2208_read_status(TMC2208Status *status) {
    uint32_t drv = 0;
    if (!tmc2208_read_reg(TMC2208_REG_DRV_STATUS, &drv)) return false;

    status->ot_warning       = drv & DRV_STATUS_OTPW;
    status->ot_shutdown      = drv & DRV_STATUS_OT;
    status->short_a          = drv & DRV_STATUS_S2GA;
    status->short_b          = drv & DRV_STATUS_S2GB;
    status->open_load_a      = drv & DRV_STATUS_OLA;
    status->open_load_b      = drv & DRV_STATUS_OLB;
    status->stealthchop_active = drv & DRV_STATUS_STEALTH;
    status->standstill       = drv & DRV_STATUS_STST;
    status->cs_actual        = (drv & DRV_STATUS_CS_ACTUAL_MASK) >> 16;
    return true;
}

bool tmc2208_ping() {
    uint32_t val = 0;
    return tmc2208_read_reg(TMC2208_REG_IOIN, &val);
}
