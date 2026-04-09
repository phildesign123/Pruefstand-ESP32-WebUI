# Hotend-Modul Spezifikation

## Wichtiger Hinweis

**Der neue Code soll exakt auf Basis des bestehenden Codes programmiert werden.**
Den vorhandenen Code aus diesem Projekt kopieren und dann anpassen – keine Neuentwicklung von Grund auf.
C:\Users\phili\OneDrive - TH Köln\Desktop\Marlin-EPS32-Steuerung\src
---

## Projektstruktur

```
src/
├── config.h            – Alle Konstanten (Pins, PID, Sensor, PWM, Lüfter, WebUI)
├── sensor.h / .cpp     – PT100/MAX31865 Temperaturmessung
├── heater.h / .cpp     – Heizer PWM-Ansteuerung
├── fan.h / .cpp         – Lüftersteuerung (temperaturgesteuert)
├── pid_controller.h / .cpp  – PID-Algorithmus
├── autotune.h / .cpp   – Relay-Autotune (Ziegler-Nichols)
├── safety.h / .cpp     – Sicherheitsfunktionen
├── webui.h / .cpp       – WebUI (AsyncWebServer + WebSocket)
├── test_hotend.h / .cpp – Hardware-Testfunktionen
└── main.cpp            – Setup, Loop, serielle Kommandos
```

---

## Konfiguration (`config.h`)

### SPI-Pins (ESP32 VSPI)

```cpp
#define MAX_CS    5
#define MAX_MOSI  23
#define MAX_MISO  19
#define MAX_CLK   18
```

### Heizer

```cpp
#define HEATER_PIN    25       // GPIO für MOSFET
```

### Lüfter

```cpp
#define FAN_PIN           26       // GPIO für MOSFET 2
#define FAN_PWM_CHANNEL   1        // LEDC Kanal (Kanal 0 = Heizer)
#define FAN_PWM_FREQ      25000    // 25 kHz (leise)
#define FAN_PWM_RESOLUTION 8       // 8-Bit = 0–255
#define FAN_ON_TEMP       50.0f    // Einschalttemperatur [°C]
#define FAN_FULL_TEMP     80.0f    // Volle Drehzahl ab [°C]
#define FAN_MIN_DUTY      80       // Mindest-PWM damit Lüfter sicher anläuft
```

### WebUI

```cpp
#define WIFI_SSID         "SSID"
#define WIFI_PASSWORD     "PASSWORD"
#define WEBUI_PORT        80
#define WEBSOCKET_PATH    "/ws"
#define WEBSOCKET_INTERVAL_MS  1000   // Status-Push alle 1 s
```

### PT100 / MAX31865 Kalibrierung

```cpp
#define SENSOR_OHMS      100.0   // PT100 = 100 Ohm bei 0 °C
#define CALIBRATION_OHMS 440.3   // Gemessener Referenzwiderstand (individuell vermessen!)
#define SENSOR_WIRES     3       // 3-Draht PT100
#define SENSOR_OFFSET    0.1     // Temperatur-Offset [°C]
```

> `CALIBRATION_OHMS` ist der wichtigste Kalibrierungswert. Typisch wäre 430 Ω,
> aber der tatsächlich gemessene Widerstand des RREF auf dem Board muss eingetragen werden.

### PID

```cpp
#define PID_FUNCTIONAL_RANGE 10.0
#define PID_MAX              255
#define PID_K1               0.95   // IIR-Glättungsfaktor D-Anteil
#define PID_K2               (1.0f - float(PID_K1))  // = 0.05
```

### Abtastzeit (von Marlin AVR übernommen)

```cpp
#define OVERSAMPLENR         16
#define ACTUAL_ADC_SAMPLES   10
#define TEMP_TIMER_FREQUENCY 977    // Hz
#define PID_dT               ((OVERSAMPLENR * float(ACTUAL_ADC_SAMPLES)) / TEMP_TIMER_FREQUENCY)
#define PID_INTERVAL_MS      (unsigned long)(PID_dT * 1000)  // ~164 ms
```

### Heizer PWM

```cpp
#define PWM_CHANNEL    0
#define PWM_FREQ       1000     // Hz (schnell → EMI oberhalb NAU7802-Nyquist)
#define PWM_RESOLUTION 7        // 7-Bit = 0–127
```

> **Hinweis:** Die PWM-Frequenz wurde von 8 Hz auf 1000 Hz erhöht. Bei 8 Hz lag die
> Heater-Schaltfrequenz unterhalb der Nyquist-Frequenz des NAU7802-ADC (40 Hz) und
> erzeugte einen systematischen Kraftoffset. Zusätzlich kompensiert das Wägezellen-Modul
> den verbleibenden DC-Offset (Ground-Shift durch Heizstrom) per Software-Kompensation
> proportional zum Heater-Duty (`LOAD_CELL_HEATER_COMP` in `config.h`).

### Standard PID-Werte

```cpp
#define DEFAULT_Kp  19.58
#define DEFAULT_Ki   0.97
#define DEFAULT_Kd  98.62
```

---

## Temperatursensor (`sensor.h` / `sensor.cpp`)

**Vorlage:** `src/sensor.cpp` kopieren und anpassen.

### Funktionen

```cpp
void    setup_sensor();           // SPI initialisieren, erste Messung blockierend
float   read_temperature();       // Nicht-blockierend, gibt letzten Wert zurück
bool    sensor_has_fault();       // MAX31865 Hardware-Fehler
uint8_t sensor_get_fault_code();  // Fault-Register 0x07
```

### Messprinzip

Nicht-blockierende One-Shot State-Machine (wie Marlin `MAX31865.cpp` Z.378–408):

```
SETUP_BIAS_VOLTAGE (2 ms) → SETUP_1_SHOT_MODE (63 ms) → READ_RTD_REG → (von vorne)
```

`read_temperature()` muss **bei jedem Loop-Durchlauf** aufgerufen werden, nicht nur im PID-Zyklus.

### Temperaturberechnung

Callendar-Van Dusen Koeffizienten (wie Marlin):
```cpp
#define RTD_A  3.9083e-3
#define RTD_B -5.775e-7
```

Formel:
```
rtd_resistance = (rtd_raw / 32768.0) * CALIBRATION_OHMS
temp = (sqrt(RTD_A² - 4*RTD_B*(1 - rtd_resistance/SENSOR_OHMS)) - RTD_A) / (2*RTD_B)
```

---

## Heizer (`heater.h` / `heater.cpp`)

**Vorlage:** `src/heater.cpp` kopieren und anpassen.

```cpp
void setup_heater();              // LEDC PWM initialisieren
void set_heater_pwm(uint8_t v);  // Wert 0–127 (7-Bit Soft-PWM wie Marlin)
```

ESP32 LEDC ersetzt Marlin Soft-PWM:
```cpp
ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
ledcAttachPin(HEATER_PIN, PWM_CHANNEL);
```

---

## Lüftersteuerung (`fan.h` / `fan.cpp`)

### Funktionen

```cpp
void    setup_fan();                     // LEDC Kanal 1 initialisieren
void    update_fan(float current_temp);  // Temperaturabhängig regeln
void    set_fan_override(uint8_t duty);  // Manueller Override (0 = auto)
uint8_t get_fan_duty();                  // Aktuellen Duty-Wert zurückgeben
bool    is_fan_auto();                   // true = automatischer Modus
```

### Verhalten

Der Lüfter (MOSFET 2, GPIO 26) wird temperaturgesteuert angesteuert:

| Temperatur         | Lüfter-PWM                                |
| ------------------ | ----------------------------------------- |
| < 50 °C            | AUS (Duty = 0)                            |
| 50 °C              | EIN mit `FAN_MIN_DUTY` (Anlaufschutz)     |
| 50 – 80 °C         | Linear skaliert `FAN_MIN_DUTY` → 255      |
| ≥ 80 °C            | Volle Drehzahl (Duty = 255)               |

### Berechnung

```cpp
void update_fan(float current_temp) {
  if (!fan_auto_mode) return;   // Override aktiv

  if (current_temp < FAN_ON_TEMP) {
    duty = 0;
  } else if (current_temp >= FAN_FULL_TEMP) {
    duty = 255;
  } else {
    float ratio = (current_temp - FAN_ON_TEMP) / (FAN_FULL_TEMP - FAN_ON_TEMP);
    duty = FAN_MIN_DUTY + (uint8_t)(ratio * (255 - FAN_MIN_DUTY));
  }
  ledcWrite(FAN_PWM_CHANNEL, duty);
}
```

### Hysterese

Zum Vermeiden von Ein/Aus-Flattern um die 50 °C-Schwelle:
- **Einschalten** bei ≥ `FAN_ON_TEMP` (50 °C)
- **Ausschalten** bei < `FAN_ON_TEMP - 2.0` (48 °C)

### Aufruf

`update_fan(current_temp)` wird im PID-Zyklus in `main.cpp` aufgerufen, direkt nach `safety_check`.

### Notabschaltung

Bei `emergency_stop()` wird der Lüfter **NICHT** abgeschaltet, sondern auf volle Drehzahl gesetzt,
um das Hotend kontrolliert abzukühlen:
```cpp
void emergency_stop(SafetyFault fault) {
  set_heater_pwm(0);
  ledcWrite(FAN_PWM_CHANNEL, 255);   // Lüfter voll an
  pid.reset();
  target_temp = 0;
  system_fault = true;
}
```

---

## PID-Regler (`pid_controller.h` / `pid_controller.cpp`)

**Vorlage:** `src/pid_controller.cpp` kopieren und anpassen.

```cpp
void  set_tunings(float kp, float ki, float kd);
float compute(float target, float current);   // gibt 0–255 zurück
void  reset();
```

### Besonderheiten

- Ki und Kd werden intern mit `PID_dT` skaliert (wie Marlin `scalePID_i/d`)
- Bang-Bang außerhalb `PID_FUNCTIONAL_RANGE`
- **I-Term Pre-Seeding** beim Reset: verhindert langes Anlaufen
- **Conditional Integration Anti-Windup**: I-Term einfrieren bei Sättigung

### PWM-Umrechnung in main.cpp

```cpp
float output = pid.compute(target_temp, current_temp);  // 0–255
uint8_t pwm  = (uint8_t)((int)output >> 1);             // → 0–127 (7-Bit)
set_heater_pwm(pwm);
```

---

## Sicherheitsfunktionen (`safety.h` / `safety.cpp`)

**Vorlage:** `src/safety.cpp` kopieren und anpassen.

### Grenzwerte

```cpp
#define TEMP_MAX                   300.0f   // Absolute Abschaltschwelle [°C]
#define THERMAL_RUNAWAY_PERIOD_MS  20000UL  // Beobachtungsfenster [ms]
#define THERMAL_RUNAWAY_HYSTERESIS 2.0f     // Mindest-Temperaturanstieg [°C]
#define THERMAL_RUNAWAY_MIN_OUTPUT 200      // Heizer gilt als "an" ab diesem PWM
#define TEMP_MAX_JUMP              50.0f    // Max. Sprung zwischen zwei Messungen [°C]
```

### Fehlertypen

```cpp
SAFETY_OK             = 0
FAULT_MAX_TEMP        = 1   // TEMP_MAX überschritten
FAULT_THERMAL_RUNAWAY = 2   // Heizer an, aber keine Erwärmung
FAULT_TEMP_JUMP       = 3   // Sensor-Ausreißer
FAULT_SENSOR          = 4   // MAX31865 Hardware-Fehler
```

### Aufruf pro PID-Zyklus

```cpp
SafetyFault fault = safety_check(current_temp, target_temp, pwm);
if (fault != SAFETY_OK) emergency_stop(fault);
```

### Zieltemperatur-Validierung (Eingabe)

Vor dem Setzen von `target_temp` immer prüfen:
```cpp
if (requested_temp >= TEMP_MAX) {
  // Fehler ausgeben, target_temp NICHT setzen
}
```

---

## Autotune (`autotune.h` / `autotune.cpp`)

**Vorlage:** `src/autotune.cpp` kopieren und anpassen.

```cpp
AutotuneResult autotune(float target, int cycles);
// Rückgabe: { float Kp, float Ki, float Kd }
```

Relay-Autotune nach Ziegler-Nichols (wie Marlin Z.711–777):
- Typischer Aufruf: `autotune(200.0, 5)`
- Timeout: 10 Minuten
- Bricht ab wenn `target >= TEMP_MAX`

---

## WebUI (`webui.h` / `webui.cpp`)

### Abhängigkeiten (PlatformIO `lib_deps`)

```ini
ESP Async WebServer
AsyncTCP
ArduinoJson
```

### Funktionen

```cpp
void setup_webui();                // WiFi verbinden, Server starten
void loop_webui();                 // WebSocket-Events verarbeiten
void webui_send_status();          // JSON-Status an alle Clients pushen
```

### Architektur

- **ESPAsyncWebServer** auf Port 80 liefert die HTML/JS/CSS-Seite aus
- **AsyncWebSocket** (`/ws`) für bidirektionale Echtzeit-Kommunikation
- HTML wird als `PROGMEM`-String direkt im Code eingebettet (kein SPIFFS/LittleFS nötig)

### HTTP-Endpunkte

| Route       | Methode | Funktion                         |
| ----------- | ------- | -------------------------------- |
| `/`         | GET     | HTML-Seite (Single Page App)     |
| `/ws`       | WS      | WebSocket-Verbindung             |

### WebSocket-Protokoll

Alle Nachrichten sind JSON.

#### Server → Client (Status-Push, jede Sekunde)

```json
{
  "type": "status",
  "temp": 195.3,
  "target": 200.0,
  "pwm": 87,
  "fan": 180,
  "fan_auto": true,
  "pid": { "p": 12.3, "i": 45.6, "d": 7.8 },
  "fault": 0,
  "uptime": 12345
}
```

#### Client → Server (Befehle)

| Befehl-JSON                                  | Funktion                              |
| -------------------------------------------- | ------------------------------------- |
| `{"cmd":"set_temp","value":200}`             | Zieltemperatur setzen                 |
| `{"cmd":"set_temp","value":0}`               | Heizer aus                            |
| `{"cmd":"autotune","value":200}`             | Autotune starten                      |
| `{"cmd":"set_pid","kp":20,"ki":1,"kd":100}` | PID-Werte manuell setzen              |
| `{"cmd":"set_fan","value":150}`              | Lüfter manuell setzen (0 = auto)      |
| `{"cmd":"reset"}`                            | Fault-Reset                           |
| `{"cmd":"test","name":"sensor"}`             | Hardware-Test auslösen                |

### WebUI-Oberfläche (HTML/JS in PROGMEM)

Die Oberfläche enthält folgende Elemente:

1. **Temperatur-Anzeige** — Ist-Temperatur groß, Soll-Temperatur daneben
2. **Temperatur-Graph** — Live-Chart der letzten 60 Sekunden (Canvas/JS)
3. **Soll-Temperatur Eingabe** — Eingabefeld + Setzen-Button
4. **PID-Anteile** — P, I, D live angezeigt
5. **PID-Tuning** — Eingabefelder für Kp, Ki, Kd + Setzen-Button
6. **Autotune** — Button mit Temperatur-Eingabe
7. **Lüfter-Status** — Duty-Anzeige, Auto/Manual Toggle, manueller Slider
8. **Fault-Anzeige** — Rot hervorgehoben bei Fehler, Reset-Button
9. **System-Info** — Uptime, WiFi-RSSI, Heap frei

### Integration in main.cpp

```cpp
void setup() {
  setup_sensor();
  setup_heater();
  setup_fan();
  setup_webui();         // WiFi + WebServer starten
  pid.set_tunings(DEFAULT_Kp, DEFAULT_Ki, DEFAULT_Kd);
  target_temp = 0;
}

void loop() {
  float temp = read_temperature();
  loop_webui();           // WebSocket-Events verarbeiten

  if (now - last_pid >= PID_INTERVAL_MS) {
    current_temp = temp;
    // PID + Safety + Heater (wie bisher)
    update_fan(current_temp);
  }

  if (now - last_ws >= WEBSOCKET_INTERVAL_MS) {
    webui_send_status();  // Status an WebSocket-Clients
  }
}
```

### Sicherheit WebUI

- Zieltemperatur-Validierung (`>= TEMP_MAX` ablehnen) gilt auch für WebSocket-Befehle
- Während Autotune läuft werden `set_temp`-Befehle ignoriert
- Fault-Zustand wird im UI klar angezeigt; nur `reset`-Befehl hebt die Sperre auf

---

## Hardware-Tests (`test_hotend.h` / `test_hotend.cpp`)

### Funktionen

```cpp
struct TestResult {
  const char* name;
  bool        passed;
  char        message[128];
};

TestResult test_sensor();       // MAX31865 Kommunikation + plausible Temperatur
TestResult test_heater();       // Kurzer PWM-Puls, Temperaturanstieg prüfen
TestResult test_fan();          // Lüfter kurz anlaufen lassen
TestResult test_pid_response(); // Mini-Heizvorgang auf 50 °C, PID-Verhalten prüfen
void       run_all_tests();     // Alle Tests sequenziell, Ergebnis auf Serial + WebSocket
```

### Test 1: Sensor (`test_sensor`)

Prüft ob der MAX31865 korrekt kommuniziert und eine plausible Temperatur liefert.

```
1. setup_sensor() aufrufen
2. 500 ms warten, read_temperature() mehrfach aufrufen
3. Prüfen: sensor_has_fault() == false
4. Prüfen: Temperatur im Bereich -10 °C … 50 °C (bei kaltem Hotend)
5. Prüfen: Zwei aufeinanderfolgende Messungen weichen < 2 °C ab
```

**Pass:** Kein Fault, Temperatur plausibel.  
**Fail:** Fault-Code ausgeben, oder Temperatur außerhalb Bereich (Sensor nicht angeschlossen / defekt).

### Test 2: Heizer (`test_heater`)

Prüft ob der MOSFET + Heizkörper funktionsfähig sind.

```
1. Starttemperatur messen
2. Heizer mit PWM 127 (voll) für 3 Sekunden einschalten
3. Heizer aus
4. Temperatur messen
5. Prüfen: Temperaturanstieg ≥ 2 °C
```

**Pass:** Messbarer Temperaturanstieg.  
**Fail:** Kein Anstieg → MOSFET defekt, Heizpatrone nicht angeschlossen, oder Kabel lose.

> **Achtung:** Nur bei kaltem Hotend (< 50 °C) durchführen, sonst überspringen.

### Test 3: Lüfter (`test_fan`)

Prüft ob der Lüfter-MOSFET angesteuert werden kann.

```
1. Lüfter auf PWM 255 setzen
2. 2 Sekunden laufen lassen
3. Lüfter aus
```

**Pass:** Visuell/akustisch bestätigt (kein automatischer Feedback-Sensor vorhanden).  
**Hinweis:** Ergebnis ist immer "PASS (manuell prüfen)" — da kein Tachosignal ausgewertet wird.

### Test 4: PID-Response (`test_pid_response`)

Prüft ob der PID-Regler das Hotend kontrolliert aufheizen kann.

```
1. Zieltemperatur = 50 °C setzen
2. PID-Loop für max. 60 Sekunden laufen lassen
3. Prüfen: Temperatur erreicht 48–52 °C innerhalb der Zeit
4. Prüfen: Kein Safety-Fault ausgelöst
5. Heizer aus, Zieltemperatur = 0
```

**Pass:** Zieltemperatur innerhalb ±2 °C erreicht, kein Fault.  
**Fail:** Timeout oder Safety-Fault → PID-Werte prüfen, Hardware-Verkabelung prüfen.

### Aufruf

Tests können auf drei Wegen gestartet werden:

1. **Seriell:** Kommando `T` → `run_all_tests()`
2. **WebUI:** Button „Hardware-Test" → sendet `{"cmd":"test","name":"all"}`
3. **Beim Boot:** Optional per Compile-Flag `#define RUN_TESTS_ON_BOOT`

### Ausgabeformat (Serial)

```
=== HARDWARE TESTS ===
[PASS] sensor       — 23.4 °C, kein Fault
[PASS] heater       — +3.1 °C Anstieg in 3 s
[PASS] fan          — Lüfter aktiviert (manuell prüfen)
[PASS] pid_response — 50.2 °C erreicht in 34 s
=== 4/4 TESTS BESTANDEN ===
```

### Ausgabeformat (WebSocket)

```json
{
  "type": "test_result",
  "results": [
    { "name": "sensor",       "passed": true,  "message": "23.4 °C, kein Fault" },
    { "name": "heater",       "passed": true,  "message": "+3.1 °C Anstieg in 3 s" },
    { "name": "fan",          "passed": true,  "message": "Lüfter aktiviert (manuell prüfen)" },
    { "name": "pid_response", "passed": true,  "message": "50.2 °C erreicht in 34 s" }
  ]
}
```

---

## Hauptprogramm (`main.cpp`)

**Vorlage:** `src/main.cpp` kopieren und anpassen.

### setup()

```cpp
setup_sensor();
setup_heater();
setup_fan();
setup_webui();
pid.set_tunings(DEFAULT_Kp, DEFAULT_Ki, DEFAULT_Kd);
target_temp = 0;

#ifdef RUN_TESTS_ON_BOOT
  run_all_tests();
#endif
```

### loop()

```cpp
float temp = read_temperature();   // IMMER, nicht nur im PID-Takt
loop_webui();                       // WebSocket-Events verarbeiten

if (now - last_pid >= PID_INTERVAL_MS) {
  current_temp = temp;
  // Sensor-Fault prüfen
  // PID berechnen
  // safety_check
  // set_heater_pwm
  update_fan(current_temp);
}

if (now - last_ws >= WEBSOCKET_INTERVAL_MS) {
  webui_send_status();
}
```

### Serielle Kommandos

| Befehl | Funktion |
|---|---|
| `S<temp>` | Zieltemperatur setzen (≥ TEMP_MAX wird abgelehnt) |
| `S0` | Heizer aus |
| `A` oder `A<temp>` | Autotune starten (Standard: 200 °C, 5 Zyklen) |
| `P<Kp>,<Ki>,<Kd>` | PID-Werte manuell setzen |
| `F<duty>` | Lüfter manuell setzen (F0 = zurück auf Auto) |
| `T` | Hardware-Tests ausführen |
| `R` | Reset (auch Fault-Zustand löschen) |

### Statusausgabe (jede Sekunde)

```
T:<ist> /<soll> P:<p-anteil> I:<i-anteil> D:<d-anteil> F:<fan-duty>
```

---

## Notabschaltung

```cpp
void emergency_stop(SafetyFault fault) {
  set_heater_pwm(0);
  ledcWrite(FAN_PWM_CHANNEL, 255);   // Lüfter voll an zum Abkühlen
  pid.reset();
  target_temp = 0;
  system_fault = true;
  // Ursache auf Serial + WebSocket ausgeben
}
```

Nach Notabschaltung ist das System gesperrt bis `R`-Kommando gesendet wird (seriell oder WebUI).
