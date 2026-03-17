#include "pid_controller.h"

// =============================================================
// PID-Algorithmus – basierend auf temperature.h Zeilen 201–228
// Identisch mit Marlin-EPS32-Steuerung, keine Änderungen
// =============================================================

void PIDController::set_tunings(float kp, float ki, float kd) {
    Kp         = kp;
    Ki_scaled  = ki * PID_dT;   // scalePID_i()
    Kd_scaled  = kd / PID_dT;   // scalePID_d()
}

float PIDController::compute(float target, float current) {
    const float pid_error = target - current;

    if (!target || pid_error < -(PID_FUNCTIONAL_RANGE)) {
        pid_reset = true;
        work_p = work_i = work_d = 0.0f;
        return 0.0f;
    }
    if (pid_error > PID_FUNCTIONAL_RANGE) {
        pid_reset = true;
        work_p = work_i = work_d = 0.0f;
        return PID_MAX;
    }

    if (pid_reset) {
        pid_reset = false;
        const float T_AMBIENT = 25.0f;
        float ratio = constrain((target - T_AMBIENT) / (200.0f - T_AMBIENT), 0.0f, 1.5f);
        float estimated_ss = 0.42f * (float)PID_MAX * ratio;

        temp_iState = (pid_error > 0.0f)
                      ? (estimated_ss * 0.4f) / Ki_scaled
                      : estimated_ss / Ki_scaled;
        work_d      = 0.0f;
        temp_dState = current;
    }

    const float max_i = (float)PID_MAX / Ki_scaled;
    float new_iState  = constrain(temp_iState + pid_error, 0.0f, max_i);

    work_p = Kp * pid_error;
    work_d = work_d + PID_K2 * (Kd_scaled * (temp_dState - current) - work_d);
    temp_dState = current;

    // Conditional Integration Anti-Windup
    float proposed_i  = Ki_scaled * new_iState;
    float output_raw  = work_p + proposed_i + work_d;
    bool  sat_low     = (output_raw < 0.0f)            && (pid_error < 0.0f);
    bool  sat_high    = (output_raw > (float)PID_MAX)  && (pid_error > 0.0f);

    if (!sat_low && !sat_high) temp_iState = new_iState;
    work_i = Ki_scaled * temp_iState;

    return constrain(work_p + work_i + work_d, 0.0f, (float)PID_MAX);
}

void PIDController::reset() { pid_reset = true; }
