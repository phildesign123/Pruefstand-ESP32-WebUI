#pragma once
#include <Arduino.h>

struct AutotuneResult {
    float Kp, Ki, Kd;
    bool  success;
};

AutotuneResult autotune(float target, int cycles = 5);
