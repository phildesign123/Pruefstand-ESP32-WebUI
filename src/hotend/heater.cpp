#include "heater.h"
#include "../config.h"

static uint8_t s_current_pwm = 0;

void setup_heater() {
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(HEATER_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);
}

void set_heater_pwm(uint8_t value) {
    s_current_pwm = value;
    ledcWrite(PWM_CHANNEL, value);
}

uint8_t get_heater_pwm() { return s_current_pwm; }
