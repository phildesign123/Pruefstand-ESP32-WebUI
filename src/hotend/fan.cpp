#include "fan.h"
#include "../config.h"

static uint8_t s_duty      = 0;
static bool    s_auto_mode = true;

void setup_fan() {
    ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    ledcAttachPin(FAN_PIN, FAN_PWM_CHANNEL);
    ledcWrite(FAN_PWM_CHANNEL, 0);
}

void update_fan(float temp) {
    if (!s_auto_mode) return;

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
        ledcWrite(FAN_PWM_CHANNEL, s_duty);
    }
}

void set_fan_override(uint8_t duty) {
    if (duty == 0) {
        s_auto_mode = true;
    } else {
        s_auto_mode = false;
        s_duty      = duty;
        ledcWrite(FAN_PWM_CHANNEL, duty);
    }
}

uint8_t get_fan_duty() { return s_duty; }
bool    is_fan_auto()  { return s_auto_mode; }
