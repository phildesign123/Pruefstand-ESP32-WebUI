#include "nau7802.h"
#include "../config.h"

static uint8_t i2c_read_reg(TwoWire &wire, SemaphoreHandle_t mx, uint8_t reg) {
    if (mx) xSemaphoreTake(mx, portMAX_DELAY);
    wire.beginTransmission(NAU7802_I2C_ADDR);
    wire.write(reg);
    wire.endTransmission(false);
    wire.requestFrom((uint8_t)NAU7802_I2C_ADDR, (uint8_t)1);
    uint8_t val = wire.available() ? wire.read() : 0;
    if (mx) xSemaphoreGive(mx);
    return val;
}

static void i2c_write_reg(TwoWire &wire, SemaphoreHandle_t mx, uint8_t reg, uint8_t val) {
    if (mx) xSemaphoreTake(mx, portMAX_DELAY);
    wire.beginTransmission(NAU7802_I2C_ADDR);
    wire.write(reg);
    wire.write(val);
    wire.endTransmission();
    if (mx) xSemaphoreGive(mx);
}

bool nau7802_init(TwoWire &wire, SemaphoreHandle_t i2c_mutex) {
    // Reset
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_PU_CTRL, PU_CTRL_RR);
    delay(10);

    // Power-Up: Digital + Analog + interner AVDD-LDO (AVDDS=1)
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_PU_CTRL,
                  PU_CTRL_PUD | PU_CTRL_PUA | PU_CTRL_AVDDS);
    delay(10);

    // Warten bis PU_CTRL_PUR gesetzt
    unsigned long t = millis();
    while (!(i2c_read_reg(wire, i2c_mutex, NAU7802_REG_PU_CTRL) & PU_CTRL_PUR)) {
        if (millis() - t > 500) {
            Serial.println("[NAU7802] Power-Up Timeout!");
            return false;
        }
        delay(10);
    }

    // CTRL1: VLDO=3.3V (bits[5:3]=0b100) + Gain=128 (bits[2:0]=0b111) = 0x27
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_CTRL1, (0b100 << 3) | GAIN_128);
    // CTRL2: 80 SPS
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_CTRL2, CRS_80SPS);
    // Chopper aktiv
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_ADC, 0x30);

    // Interne Offset-Kalibrierung (vor Cycle Start)
    uint8_t ctrl2 = i2c_read_reg(wire, i2c_mutex, NAU7802_REG_CTRL2);
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_CTRL2, ctrl2 | CALS_BIT);

    t = millis();
    while (i2c_read_reg(wire, i2c_mutex, NAU7802_REG_CTRL2) & CALS_BIT) {
        if (millis() - t > 500) {
            Serial.println("[NAU7802] Kalibrierung Timeout!");
            return false;
        }
        delay(10);
    }

    if (i2c_read_reg(wire, i2c_mutex, NAU7802_REG_CTRL2) & CAL_ERR_BIT) {
        Serial.println("[NAU7802] Kalibrierfehler!");
        return false;
    }

    // Cycle Start nach Kalibrierung
    uint8_t pu = i2c_read_reg(wire, i2c_mutex, NAU7802_REG_PU_CTRL);
    i2c_write_reg(wire, i2c_mutex, NAU7802_REG_PU_CTRL, pu | PU_CTRL_CS);

    Serial.println("[NAU7802] Initialisiert (AVDDS=1, VLDO=3.3V, Gain=128, 80 SPS).");
    return true;
}

int32_t nau7802_read_raw(TwoWire &wire, SemaphoreHandle_t mx) {
    if (mx) xSemaphoreTake(mx, portMAX_DELAY);
    wire.beginTransmission(NAU7802_I2C_ADDR);
    wire.write(NAU7802_REG_ADC_DATA);
    wire.endTransmission(false);
    wire.requestFrom((uint8_t)NAU7802_I2C_ADDR, (uint8_t)3);

    int32_t raw = 0;
    if (wire.available() >= 3) {
        raw  = (int32_t)wire.read() << 16;
        raw |= (int32_t)wire.read() <<  8;
        raw |=          wire.read();
    }
    if (mx) xSemaphoreGive(mx);

    // Vorzeichen-Erweiterung (24-Bit signed → 32-Bit)
    if (raw & 0x800000) raw |= 0xFF000000;
    return raw;
}

bool nau7802_is_ready(TwoWire &wire, SemaphoreHandle_t mx) {
    return (i2c_read_reg(wire, mx, NAU7802_REG_PU_CTRL) & PU_CTRL_CR) != 0;
}
