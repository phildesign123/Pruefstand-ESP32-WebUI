#include "fan.h"
#include "../config.h"

static uint8_t       s_duty             = 0;
static bool          s_auto_mode        = true;
static unsigned long s_override_until_ms = 0;   // 0 = kein zeitlicher Override

void setup_fan() {
    ledcAttachChannel(FAN_PIN, FAN_PWM_FREQ, FAN_PWM_RESOLUTION, FAN_PWM_CHANNEL);  // Ch2, Timer 1
    ledcWrite(FAN_PIN, 0);
}

void update_fan(float temp) {
    if (!s_auto_mode) {
        // Zeitlichen Override prüfen (rollover-sicher mit signed-Vergleich)
        if (s_override_until_ms != 0 &&
            (long)(millis() - s_override_until_ms) >= 0) {
            s_auto_mode         = true;
            s_override_until_ms = 0;
            // fällt in die Auto-Logik durch
        } else {
            return;
        }
    }

    uint8_t new_duty;
    if (temp < (FAN_ON_TEMP - 2.0f)) {
        // Hysterese: erst unter 48 °C ausschalten
        new_duty = 0;
    } else if (temp < FAN_ON_TEMP) {
        // Zwischen 48–50 °C: Zustand halten
        new_duty = s_duty;
    } else if (temp >= FAN_FULL_TEMP) {
        new_duty = 255;
    } else {
        float ratio = (temp - FAN_ON_TEMP) / (FAN_FULL_TEMP - FAN_ON_TEMP);
        new_duty = (uint8_t)(FAN_MIN_DUTY + ratio * (255 - FAN_MIN_DUTY));
    }

    if (new_duty != s_duty) {
        s_duty = new_duty;
        ledcWrite(FAN_PIN, s_duty);
    }
}

void set_fan_override(uint8_t duty) {
    s_override_until_ms = 0;   // Manueller Override hebt zeitlichen auf
    if (duty == 0) {
        s_auto_mode = true;
    } else {
        s_auto_mode = false;
        s_duty      = duty;
        ledcWrite(FAN_PIN, duty);
    }
}

void set_fan_off_timed(uint32_t duration_ms) {
    s_auto_mode         = false;
    s_duty              = 0;
    ledcWrite(FAN_PIN, 0);
    unsigned long end = millis() + duration_ms;
    if (end == 0) end = 1;     // 0 ist reserviert für "kein Override"
    s_override_until_ms = end;
}

uint8_t get_fan_duty() { return s_duty; }
bool    is_fan_auto()  { return s_auto_mode; }
