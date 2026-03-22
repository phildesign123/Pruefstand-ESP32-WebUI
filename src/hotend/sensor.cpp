#include "sensor.h"
#include "../config.h"

// =============================================================
// MAX31865 nicht-blockierende One-Shot State-Machine
// Adaptiert von Marlin MAX31865.cpp Z.378–408
// Angepasst: SPI-Mutex für Bus-Sharing mit SD-Karte
// =============================================================

// MAX31865 Register
#define MAX31865_CONFIG_REG    0x00
#define MAX31865_CONFIG_WRITE  0x80
#define MAX31865_RTDMSB_REG    0x01
#define MAX31865_FAULT_REG     0x07

// Config-Bits
#define MAX31865_CONFIG_BIAS   0x80
#define MAX31865_CONFIG_AUTO   0x40
#define MAX31865_CONFIG_1SHOT  0x20
#define MAX31865_CONFIG_3WIRE  0x10
#define MAX31865_CONFIG_FILT50 0x01

// Callendar-Van Dusen
#define RTD_A  3.9083e-3f
#define RTD_B -5.775e-7f

enum SensorState : uint8_t {
    SETUP_BIAS_VOLTAGE,
    SETUP_1_SHOT_MODE,
    READ_RTD_REG
};

static SPIClass        *s_spi          = nullptr;
static SemaphoreHandle_t s_spi_mutex   = nullptr;
static SensorState      s_state        = SETUP_BIAS_VOLTAGE;
static unsigned long    s_next_ms      = 0;
static float            s_last_temp    = 0.0f;
static bool             s_fault        = false;
static uint8_t          s_fault_code   = 0;
static const uint8_t    STD_FLAGS      = MAX31865_CONFIG_3WIRE | MAX31865_CONFIG_FILT50;

// ── SPI-Hilfsfunktionen (mit Mutex) ─────────────────────────

static const SPISettings MAX_SPI_SETTINGS(1000000, MSBFIRST, SPI_MODE1);

static void spi_write_reg(uint8_t addr, uint8_t val) {
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    s_spi->beginTransaction(MAX_SPI_SETTINGS);
    digitalWrite(MAX_CS, LOW);
    s_spi->transfer(addr | 0x80);
    s_spi->transfer(val);
    digitalWrite(MAX_CS, HIGH);
    s_spi->endTransaction();
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
}

static uint8_t spi_read_reg(uint8_t addr) {
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    s_spi->beginTransaction(MAX_SPI_SETTINGS);
    digitalWrite(MAX_CS, LOW);
    s_spi->transfer(addr & 0x7F);
    uint8_t val = s_spi->transfer(0xFF);
    digitalWrite(MAX_CS, HIGH);
    s_spi->endTransaction();
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    return val;
}

static uint16_t spi_read_reg16(uint8_t addr) {
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    s_spi->beginTransaction(MAX_SPI_SETTINGS);
    digitalWrite(MAX_CS, LOW);
    s_spi->transfer(addr & 0x7F);
    uint8_t msb = s_spi->transfer(0xFF);
    uint8_t lsb = s_spi->transfer(0xFF);
    digitalWrite(MAX_CS, HIGH);
    s_spi->endTransaction();
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    return ((uint16_t)msb << 8) | lsb;
}

// ── Callendar-Van Dusen Temperaturberechnung ─────────────────
// Identisch mit Marlin MAX31865.cpp Z.449–484

static float rtd_to_temp(float resistance) {
    float Z1 = -RTD_A;
    float Z2 = RTD_A * RTD_A - (4.0f * RTD_B);
    float Z3 = (4.0f * RTD_B) / SENSOR_OHMS;
    float Z4 = 2.0f * RTD_B;

    float temp = Z2 + (Z3 * resistance);
    temp = (sqrtf(temp) + Z1) / Z4;

    if (temp < 0.0f) {
        // Unterhalb 0 °C: Polynomkorrektur
        float r = (resistance / SENSOR_OHMS) * 100.0f;
        float rpoly = r;
        temp = -242.02f + 2.2228f * rpoly;
        rpoly *= r; temp +=  2.5859e-3f  * rpoly;
        rpoly *= r; temp -= 4.8260e-6f   * rpoly;
        rpoly *= r; temp -= 2.8183e-8f   * rpoly;
        rpoly *= r; temp += 1.5243e-10f  * rpoly;
    }
    return temp;
}

static float read_rtd_temp() {
    uint16_t raw = spi_read_reg16(MAX31865_RTDMSB_REG);
    if (raw & 0x0001) {
        s_fault = true;
        s_fault_code = spi_read_reg(MAX31865_FAULT_REG);
    }
    raw >>= 1;
    float resistance = ((float)raw / 32768.0f) * CALIBRATION_OHMS;
    return rtd_to_temp(resistance) + SENSOR_OFFSET;
}

// ── Öffentliche API ──────────────────────────────────────────

void setup_sensor(SPIClass &spi, SemaphoreHandle_t spi_mutex) {
    s_spi       = &spi;
    s_spi_mutex = spi_mutex;

    pinMode(MAX_CS, OUTPUT);
    digitalWrite(MAX_CS, HIGH);

    spi.begin(MAX_CLK, MAX_MISO, MAX_MOSI, MAX_CS);

    // Erst-Konfiguration: Bias aus, Auto aus, 3-Wire, 50Hz
    spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS);

    // Fault-Status löschen (Bit 1 = Fault Clear)
    spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS | 0x02);
    delay(1);

    // Blockierende Erstmessung für initialen Wert
    spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS | MAX31865_CONFIG_BIAS);
    delay(10);
    spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS | MAX31865_CONFIG_BIAS | MAX31865_CONFIG_1SHOT);
    delay(65);
    s_last_temp = read_rtd_temp();
    spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS);
    // Software-Fault nach Init zurücksetzen
    s_fault = false;
    s_fault_code = 0;

    s_state   = SETUP_BIAS_VOLTAGE;
    s_next_ms = millis();
    Serial.printf("[SENSOR] Initialisiert. Starttemperatur: %.1f °C\n", s_last_temp);
}

float read_temperature() {
    unsigned long ms = millis();
    if ((long)(ms - s_next_ms) < 0) return s_last_temp;

    switch (s_state) {
        case SETUP_BIAS_VOLTAGE:
            spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS | MAX31865_CONFIG_BIAS);
            s_next_ms = ms + 2;
            s_state   = SETUP_1_SHOT_MODE;
            break;

        case SETUP_1_SHOT_MODE:
            spi_write_reg(MAX31865_CONFIG_WRITE,
                          STD_FLAGS | MAX31865_CONFIG_BIAS | MAX31865_CONFIG_1SHOT);
            s_next_ms = ms + 63;
            s_state   = READ_RTD_REG;
            break;

        case READ_RTD_REG:
            s_last_temp = read_rtd_temp();
            spi_write_reg(MAX31865_CONFIG_WRITE, STD_FLAGS);
            s_state   = SETUP_BIAS_VOLTAGE;
            s_next_ms = ms;
            break;
    }
    return s_last_temp;
}

bool    sensor_has_fault()       { return s_fault; }
uint8_t sensor_get_fault_code()  { return s_fault_code; }
void    sensor_clear_fault()     { s_fault = false; s_fault_code = 0; }
