#include "heater.h"
#include "../config.h"

static uint8_t s_current_pwm = 0;

void setup_heater() {
    ledcAttachChannel(HEATER_PIN, PWM_FREQ, PWM_RESOLUTION, PWM_CHANNEL);  // Ch0, Timer 0
    ledcWrite(HEATER_PIN, 0);
}

void set_heater_pwm(uint8_t value) {
    s_current_pwm = value;
    ledcWrite(HEATER_PIN, value);
}

uint8_t get_heater_pwm() { return s_current_pwm; }
