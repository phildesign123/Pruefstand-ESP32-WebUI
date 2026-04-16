#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "safety.h"
#include "autotune.h"

// Hotend-Modul initialisieren und Task starten
void hotend_init(SPIClass &spi, SemaphoreHandle_t spi_mutex);

// Zieltemperatur setzen (0 = aus; >= TEMP_MAX wird abgelehnt)
bool hotend_set_target(float temp_c);

// Lesezugriffe (threadsicher, nicht-blockierend)
float   hotend_get_temperature();
float   hotend_get_target();
float   hotend_get_duty();          // 0.0–1.0
float   hotend_get_pid_p();
float   hotend_get_pid_i();
float   hotend_get_pid_d();
uint8_t hotend_get_fan_duty();
bool    hotend_is_fan_auto();

// Konfiguration
void hotend_set_pid(float kp, float ki, float kd);
void hotend_set_fan(uint8_t duty);  // 0 = Auto
void hotend_fan_off_timed(uint32_t duration_ms);  // Lüfter für X ms aus, danach Auto

// Fault-Management
bool        hotend_has_fault();
SafetyFault hotend_get_fault();
const char* hotend_get_fault_string();
void        hotend_clear_fault();

// Autotune (blockiert den Aufruf-Task!)
AutotuneResult hotend_autotune(float target, int cycles = 5);

// FreeRTOS-Task (intern, nicht direkt aufrufen)
void hotend_task(void *arg);
