#pragma once
#include <Arduino.h>

void    setup_heater();
void    set_heater_pwm(uint8_t value);  // 0–127 (7-Bit)
uint8_t get_heater_pwm();
