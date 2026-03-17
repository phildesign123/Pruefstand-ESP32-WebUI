#pragma once
#include <Arduino.h>

void    setup_fan();
void    update_fan(float current_temp);   // Temperaturgesteuerter Automodus
void    set_fan_override(uint8_t duty);   // Manuell (0 = zurück auf Auto)
uint8_t get_fan_duty();
bool    is_fan_auto();
