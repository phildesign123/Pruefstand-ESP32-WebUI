#pragma once
#include <Arduino.h>

// Web-UI initialisieren (Wi-Fi AP, HTTP-Server, WebSocket, Tasks)
void webui_init();

// Wird von anderen Modulen aufgerufen um Events an WebSocket zu pushen
void webui_notify_clients(const char *json);
