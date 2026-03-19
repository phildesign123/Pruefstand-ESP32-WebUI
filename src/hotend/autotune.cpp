#include "autotune.h"
#include "sensor.h"
#include "heater.h"
#include "safety.h"
#include "../config.h"
#include "esp_task_wdt.h"

// =============================================================
// Relay-Autotune nach Ziegler-Nichols (wie Marlin Z.711–777)
// Identisch mit Referenzcode, ohne Änderungen
// =============================================================

#define AUTOTUNE_TIMEOUT_MS  (10UL * 60UL * 1000UL)

AutotuneResult autotune(float target, int cycles) {
    AutotuneResult result = {0, 0, 0, false};

    if (target >= TEMP_MAX) {
        Serial.printf("[AUTOTUNE] Fehler: Zieltemperatur %.1f °C zu hoch (Max: %.0f °C)\n",
                      target, TEMP_MAX);
        return result;
    }

    long bias = PID_MAX / 2, d = bias;
    long t_high = 0, t_low = 0;
    float maxT = 0.0f, minT = 10000.0f;
    bool heating = true;
    unsigned long t1 = millis(), t2 = t1;
    int cycle = 0;

    set_heater_pwm((uint8_t)(bias / 2));  // Start bei 50 % (7-Bit: /2)
    Serial.printf("[AUTOTUNE] Starte bei %.1f °C, %d Zyklen...\n", target, cycles);
    unsigned long start_ms = millis();

    while (cycle <= cycles) {
        if (millis() - start_ms >= AUTOTUNE_TIMEOUT_MS) {
            set_heater_pwm(0);
            Serial.println("[AUTOTUNE] TIMEOUT – abgebrochen.");
            return result;
        }

        float current = read_temperature();

        if (current >= TEMP_MAX) {
            set_heater_pwm(0);
            Serial.println("[AUTOTUNE] Maximaltemperatur erreicht – abgebrochen.");
            return result;
        }

        if (current > maxT) maxT = current;
        if (current < minT) minT = current;

        if (heating && current > target) {
            heating = false;
            set_heater_pwm((uint8_t)((bias - d) / 2));
            t1     = millis();
            t_high = t1 - t2;
            maxT   = target;
        }

        if (!heating && current < target) {
            heating = true;
            t2      = millis();
            t_low   = t2 - t1;

            if (cycle > 0) {
                bias += (d * (t_high - t_low)) / (t_low + t_high);
                bias = constrain(bias, 20L, (long)PID_MAX - 20);
                d    = (bias > PID_MAX / 2) ? (long)PID_MAX - 1 - bias : bias;

                if (cycle > 2) {
                    float Ku = (4.0f * d) / (M_PI * (maxT - minT) * 0.5f);
                    float Tu = (t_low + t_high) * 0.001f;

                    result.Kp = 0.6f * Ku;
                    result.Ki = result.Kp * 2.0f / Tu;
                    result.Kd = result.Kp * Tu / 8.0f;

                    Serial.printf("[AUTOTUNE] Zyklus %d: Kp=%.2f Ki=%.4f Kd=%.2f\n",
                                  cycle, result.Kp, result.Ki, result.Kd);
                }
            }
            set_heater_pwm((uint8_t)((bias + d) / 2));
            cycle++;
            minT = target;
        }
        esp_task_wdt_reset();
        delay(1);
    }

    set_heater_pwm(0);
    result.success = (result.Kp > 0.0f);
    Serial.printf("[AUTOTUNE] Fertig: Kp=%.2f Ki=%.4f Kd=%.2f\n",
                  result.Kp, result.Ki, result.Kd);
    return result;
}
