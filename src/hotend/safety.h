#pragma once
#include <Arduino.h>
#include "../config.h"

enum SafetyFault : uint8_t {
    SAFETY_OK             = 0,
    FAULT_MAX_TEMP        = 1,
    FAULT_THERMAL_RUNAWAY = 2,
    FAULT_TEMP_JUMP       = 3,
    FAULT_SENSOR          = 4,
    FAULT_MIN_TEMP        = 5,
};

SafetyFault      safety_check(float current_temp, float target_temp, uint8_t heater_output);
const char*      safety_fault_string(SafetyFault fault);
void             safety_reset();
