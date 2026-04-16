#pragma once
#include <Arduino.h>

void    setup_fan();
void    update_fan(float current_temp);   // Temperaturgesteuerter Automodus
void    set_fan_override(uint8_t duty);   // Manuell (0 = zurück auf Auto)
void    set_fan_off_timed(uint32_t duration_ms);  // Lüfter für X ms aus, danach Auto
uint8_t get_fan_duty();
bool    is_fan_auto();
