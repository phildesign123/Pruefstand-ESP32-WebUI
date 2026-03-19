#pragma once

// =============================================================
// Zentrale Konfiguration – alle Hardware-Pins, PID, PWM, Limits
// =============================================================

// ── SPI VSPI (geteilt: MAX31865 + SD-Karte) ─────────────────
#define MAX_CS    5
#define MAX_MOSI  23
#define MAX_MISO  19
#define MAX_CLK   18
#define SD_CS     4

// ── Heizer ──────────────────────────────────────────────────
#define HEATER_PIN          25
#define PWM_CHANNEL         0
#define PWM_FREQ            8          // Hz (langsames Soft-PWM wie Marlin)
#define PWM_RESOLUTION      7          // 7-Bit → 0–127

// ── Lüfter ──────────────────────────────────────────────────
#define FAN_PIN             26
#define FAN_PWM_CHANNEL     2
#define FAN_PWM_FREQ        25000      // 25 kHz (lautlos)
#define FAN_PWM_RESOLUTION  8          // 8-Bit → 0–255
#define FAN_ON_TEMP         50.0f
#define FAN_FULL_TEMP       80.0f
#define FAN_MIN_DUTY        80

// ── PT100 / MAX31865 ─────────────────────────────────────────
#define SENSOR_OHMS         100.0f
#define CALIBRATION_OHMS    440.3f     // Individuell kalibrieren!
#define SENSOR_WIRES        3
#define SENSOR_OFFSET       0.1f

// ── PID ──────────────────────────────────────────────────────
#define PID_FUNCTIONAL_RANGE    10.0f
#define PID_MAX                 255
#define PID_K1                  0.95f
#define PID_K2                  (1.0f - PID_K1)

#define OVERSAMPLENR            16
#define ACTUAL_ADC_SAMPLES      10
#define TEMP_TIMER_FREQUENCY    977
#define PID_dT                  ((OVERSAMPLENR * float(ACTUAL_ADC_SAMPLES)) / TEMP_TIMER_FREQUENCY)
#define PID_INTERVAL_MS         ((unsigned long)(PID_dT * 1000.0f))  // ~164 ms

#define DEFAULT_Kp              19.58f
#define DEFAULT_Ki               0.97f
#define DEFAULT_Kd              98.62f

// ── Sicherheit ───────────────────────────────────────────────
#define TEMP_MAX                    300.0f
#define TEMP_MIN                    5.0f
#define THERMAL_RUNAWAY_PERIOD_MS   20000UL
#define THERMAL_RUNAWAY_HYSTERESIS  2.0f
#define THERMAL_RUNAWAY_MIN_OUTPUT  127
#define TEMP_MAX_JUMP               50.0f
// Thermal Runaway Haltephase (Ziel erreicht)
#define THERMAL_HOLD_WINDOW         5.0f     // Haltephase aktiv wenn < 5°C vom Ziel
#define THERMAL_HOLD_DROP           15.0f    // Max. erlaubter Abfall unter Ziel
#define THERMAL_HOLD_PERIOD_MS      30000UL  // 30 s Erholungszeit
// Hardware Watchdog
#define WDT_TIMEOUT_S               5        // 5 s Task-WDT

// ── Motor / TMC2208 ──────────────────────────────────────────
#define MOTOR_STEP_PIN      27
#define MOTOR_DIR_PIN       14
#define MOTOR_EN_PIN        13
#define MOTOR_UART_TX       33
#define MOTOR_UART_RX       32
#define MOTOR_UART_BAUD     115200

#define MOTOR_E_STEPS_PER_MM    93.0f
#define MOTOR_MICROSTEP         16
#define MOTOR_CURRENT_MA        800
#define MOTOR_HOLD_CURRENT_MA   400
#define MOTOR_STEALTHCHOP       true
#define MOTOR_IDLE_TIMEOUT_MS   2000UL   // Motor nach 2s stromlos

// ── Wägezelle / NAU7802 ──────────────────────────────────────
#define NAU7802_SDA             21
#define NAU7802_SCL             22
#define NAU7802_DRDY_PIN        34
#define NAU7802_I2C_ADDR        0x2A
#define NAU7802_I2C_FREQ        400000
#define LOAD_CELL_MEDIAN_SIZE   5
#define LOAD_CELL_AVG_SIZE      10
#define LOAD_CELL_TARE_SAMPLES  80
#define LOAD_CELL_CAL_SAMPLES   80

// ── Datenlogger / SD-Karte ───────────────────────────────────
#define DATALOG_INTERVAL_MS     1000
#define DATALOG_BUFFER_SIZE     128
#define DATALOG_FLUSH_INTERVAL_S 5
#define DATALOG_MAX_FILE_SIZE_MB 10
#define DATALOG_MAX_FILES        100

// ── Web-UI / Wi-Fi ───────────────────────────────────────────
#define WEBUI_PORT              80
#define WEBUI_WS_PATH           "/ws"
#define WEBUI_WS_INTERVAL_MS    100    // 10 Hz
#define WIFI_AP_SSID            "ESP32-Steuerung"
#define WIFI_AP_PASSWORD        "12345678"
#define WIFI_AP_IP              "192.168.4.1"

// ── Sequencer ────────────────────────────────────────────────
#define SEQ_TEMP_TOLERANCE      2.0f
#define SEQ_TEMP_STABLE_TIME_S  10
#define SEQ_HEATING_TIMEOUT_S   120
#define SEQ_LOG_INTERVAL_MS     100
#define SEQ_MAX_SEQUENCES       20

// ── Task-Konfiguration ────────────────────────────────────────
#define TASK_STACK_HOTEND       4096
#define TASK_STACK_LOADCELL     4096
#define TASK_STACK_MOTOR        4096
#define TASK_STACK_DATALOG_S    2048
#define TASK_STACK_DATALOG_W    4096
#define TASK_STACK_WS_PUSH      4096
#define TASK_STACK_SEQUENCER    4096

#define TASK_PRIO_LOADCELL      6
#define TASK_PRIO_HOTEND        5
#define TASK_PRIO_MOTOR         4
#define TASK_PRIO_SEQUENCER     4
#define TASK_PRIO_DATALOG_S     3
#define TASK_PRIO_WS_PUSH       3
#define TASK_PRIO_DATALOG_W     2
