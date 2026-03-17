#pragma once
#include <Arduino.h>
#include "../config.h"

struct PIDController {
    float Kp        = DEFAULT_Kp;
    float Ki_scaled = DEFAULT_Ki * PID_dT;
    float Kd_scaled = DEFAULT_Kd / PID_dT;

    float temp_iState = 0.0f;
    float temp_dState = 0.0f;
    float work_p      = 0.0f;
    float work_i      = 0.0f;
    float work_d      = 0.0f;
    bool  pid_reset   = true;

    void  set_tunings(float kp, float ki, float kd);
    float compute(float target, float current);
    void  reset();

    float get_work_p() const { return work_p; }
    float get_work_i() const { return work_i; }
    float get_work_d() const { return work_d; }
};
