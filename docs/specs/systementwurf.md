# Systementwurf – ESP32 Extruder-Prüfstand

## 1. Systemübersicht

Der Extruder-Prüfstand ist ein eingebettetes Echtzeit-Messsystem auf Basis eines ESP32-WROVER. Es steuert ein Hotend (Heizelement + Lüfter), einen Schrittmotor (Extruder), eine Wägezelle und zeichnet Messdaten auf SD-Karte auf. Die gesamte Bedienung erfolgt über ein Web-UI (Single-Page-App) per WLAN.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Web-Browser                              │
│   Dashboard · Einstellungen · Sequenzer · Dateiverwaltung       │
└──────────────────────────┬──────────────────────────────────────┘
                           │ WebSocket (JSON, 10 Hz)
                           │ HTTP REST API
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                     ESP32-WROVER (Dual-Core)                    │
│                                                                 │
│  ┌──────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────────┐  │
│  │  Hotend   │ │  Motor   │ │ Wägezelle │ │   Datenlogger    │  │
│  │  Modul    │ │  Modul   │ │  Modul    │ │     Modul        │  │
│  └────┬─────┘ └────┬─────┘ └─────┬─────┘ └───────┬──────────┘  │
│       │             │             │                │             │
│  ┌────┴─────────────┴─────────────┴────────────────┴──────────┐ │
│  │              Sequenzer (Messablauf-Steuerung)               │ │
│  └─────────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              Web-UI Modul (WiFi + WebServer + WS)           │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
       │ SPI          │ RMT/UART       │ I2C          │ SPI
       ▼              ▼                ▼              ▼
  ┌─────────┐   ┌──────────┐    ┌──────────┐   ┌──────────┐
  │MAX31865  │   │ TMC2208  │    │ NAU7802  │   │ SD-Karte │
  │(PT100)   │   │(Stepper) │    │(24-bit)  │   │  (FAT32) │
  └─────────┘   └──────────┘    └──────────┘   └──────────┘
```

---

## 2. Hardware-Schnittstellen

### 2.1 SPI-Bus (VSPI, geteilt)

| Signal | GPIO | Teilnehmer |
|--------|------|------------|
| CLK    | 18   | MAX31865, SD-Karte |
| MOSI   | 23   | MAX31865, SD-Karte |
| MISO   | 19   | MAX31865, SD-Karte |
| CS MAX31865 | 5 | Temperatursensor |
| CS SD  | 4    | SD-Karte |

Zugriff über `spi_mutex` (FreeRTOS-Semaphore) serialisiert.

### 2.2 I2C-Bus

| Signal | GPIO | Frequenz |
|--------|------|----------|
| SDA    | 21   | 100 kHz  |
| SCL    | 22   | 100 kHz  |
| DRDY   | 34   | Input    |

Einziger Teilnehmer: NAU7802 (Adresse 0x2A). Zugriff über `i2c_mutex`.

### 2.3 UART (TMC2208)

| Signal | GPIO | Baudrate |
|--------|------|----------|
| TX     | 33   | 115200   |
| RX     | 32   | 115200   |

Trinamic-proprietäres 8-Byte-Protokoll. Zugriff über `uart_mutex`.

### 2.4 GPIO-Signale

| Signal       | GPIO | Typ           | Funktion |
|--------------|------|---------------|----------|
| HEATER_PWM   | 25   | LEDC (8 Hz, 7-Bit)   | Heizelement-Steuerung |
| FAN_PWM      | 26   | LEDC (25 kHz, 8-Bit) | Lüfter-Steuerung |
| MOTOR_STEP   | 27   | RMT (Pulsfolge)      | Schritt-Pulse |
| MOTOR_DIR    | 14   | Digital OUT           | Drehrichtung |
| MOTOR_EN     | 13   | Digital OUT           | Motor Enable (LOW = aktiv) |

---

## 3. Software-Architektur

### 3.1 Modulstruktur

```
src/
├── main.cpp                 Einstiegspunkt, Init aller Module, serielle Befehle
├── config.h                 Zentrale Konfiguration (Pins, PID, Limits, Tasks)
│
├── hotend/
│   ├── sensor.h/.cpp        MAX31865 SPI-Zustandsautomat (non-blocking)
│   ├── heater.h/.cpp        LEDC-PWM-Steuerung Heizelement
│   ├── fan.h/.cpp           LEDC-PWM-Steuerung Lüfter (temperaturabhängig)
│   ├── pid_controller.h/.cpp PID-Algorithmus (Anti-Windup, D-IIR-Filter)
│   ├── autotune.h/.cpp      Relay-Autotune (Ziegler-Nichols, 5 Zyklen)
│   ├── safety.h/.cpp        Thermal Runaway, Max-Temp, Sensor-Sprung
│   └── hotend.h/.cpp        Master-Modul: FreeRTOS-Task, PID-Loop
│
├── motor/
│   ├── motor_rmt.h/.cpp     RMT-Treiber für Puls-Erzeugung (bis 50 kHz)
│   ├── tmc2208_uart.h/.cpp  TMC2208 UART-Registersteuerung
│   ├── tmc2208_regs.h       Register-Bitfelder
│   └── motor.h/.cpp         High-Level-API: Bewegung, Kalibrierung, Queue
│
├── load_cell/
│   ├── nau7802.h/.cpp       NAU7802 I2C-Treiber (24-Bit ADC, 80 SPS)
│   ├── filter_median.h/.cpp Medianfilter (5 Samples)
│   ├── filter_avg.h/.cpp    Mittelwertfilter (10 Samples)
│   └── load_cell.h/.cpp     Master-Modul: Tara, Kalibrierung, Task
│
├── datalog/
│   └── datalog.h/.cpp       SD-Verwaltung, Dual-Task (Sampler + Writer)
│
└── webui/
    ├── webui.h/.cpp         AsyncWebServer, WebSocket, WiFi, REST-API
    └── sequencer.h/.cpp     Messsequenz-Automat (Heizen→Messen→Weiter)

data/                        Frontend (LittleFS)
├── index.html               Single-Page-App
├── app.js                   WebSocket-Client, Chart, State-Management
└── style.css                Dark/Light Theme, responsive Grid
```

### 3.2 Modulabhängigkeiten

```
                    ┌──────────┐
                    │ config.h │  (alle Module lesen hieraus)
                    └────┬─────┘
                         │
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
   ┌─────────┐     ┌──────────┐     ┌──────────┐
   │ Hotend   │     │  Motor   │     │Wägezelle │
   │          │     │          │     │          │
   │ sensor   │     │ motor_rmt│     │ nau7802  │
   │ heater   │     │ tmc2208  │     │ filter_* │
   │ fan      │     └────┬─────┘     └────┬─────┘
   │ pid      │          │                │
   │ safety   │          │                │
   │ autotune │          │                │
   └────┬─────┘          │                │
        │                │                │
        ▼                ▼                ▼
   ┌─────────────────────────────────────────┐
   │            Sequenzer                     │
   │  (orchestriert Hotend + Motor + Datalog) │
   └──────────────────┬──────────────────────┘
                      │
                      ▼
   ┌─────────────────────────────────────────┐
   │           Datenlogger                    │
   │  (liest alle Sensorwerte, schreibt CSV)  │
   └──────────────────┬──────────────────────┘
                      │
                      ▼
   ┌─────────────────────────────────────────┐
   │            Web-UI                        │
   │  (steuert alle Module, zeigt Daten an)   │
   └─────────────────────────────────────────┘
```

---

## 4. Task-Architektur (FreeRTOS)

### 4.1 Core-Zuordnung

**Core 0** – Netzwerk & I/O: WiFi-Stack, Datalog-Writer
**Core 1** – Echtzeit: Sensorik, Regelung, Motor, Sequenzer

### 4.2 Task-Übersicht

| Task | Core | Priorität | Stack | Intervall | Aufgabe |
|------|------|-----------|-------|-----------|---------|
| `hotend_task` | 1 | 5 | 4096 B | ~1 ms Loop, PID alle 164 ms | Temperaturmessung + PID-Regelung |
| `load_cell_task` | 1 | 6 | 8192 B | 12,5 ms (80 Hz) | ADC-Polling, Filterung, Gewicht |
| `motor_task` | 1 | 4 | 4096 B | 10 ms (Queue) | Befehlsverarbeitung, RMT-Steuerung |
| `sequencer_task` | 1 | 4 | 4096 B | 100 ms | Messablauf-Zustandsautomat |
| `datalog_sampler` | 1 | 3 | 2048 B | 100 ms (10 Hz) | Sensoren lesen → Queue |
| `datalog_writer` | 0 | 2 | 4096 B | Event-driven | Queue → RAM-Puffer → SD-Karte |
| `ws_push_task` | 1 | 3 | 4096 B | 100 ms (10 Hz) | JSON-Status → WebSocket-Clients |
| WiFi-Event-Task | 0 | (System) | (System) | Event-driven | WiFi-Verbindung, Reconnect |

### 4.3 Synchronisation

| Mutex/Semaphore | Geschützte Ressource | Benutzer |
|-----------------|---------------------|----------|
| `spi_mutex` | SPI-Bus (MAX31865 + SD) | Hotend, Datalog |
| `i2c_mutex` | I2C-Bus (NAU7802) | Wägezelle, WebUI |
| `uart_mutex` | UART (TMC2208) | Motor |
| `s_mux` (Spinlock) | Volatile Temp/Duty-Variablen | Hotend-Task R/W |

### 4.4 Task-Diagramm

```
Core 0                          Core 1
──────                          ──────
┌──────────────┐                ┌──────────────────┐
│ WiFi-Stack   │                │ load_cell_task [6]│ ← höchste Prio
│ (System)     │                │  80 Hz ADC-Poll   │
└──────────────┘                └──────────────────┘
                                ┌──────────────────┐
                                │ hotend_task    [5]│
                                │  PID @ 164 ms     │
                                └──────────────────┘
                                ┌──────────────────┐
                                │ motor_task     [4]│
                                │  Queue-basiert    │
                                └──────────────────┘
                                ┌──────────────────┐
                                │ sequencer_task [4]│
                                │  100 ms Loop      │
                                └──────────────────┘
┌──────────────┐                ┌──────────────────┐
│ datalog_     │                │ datalog_         │
│ writer    [2]│ ◄──── Queue ───│ sampler       [3]│
│ SD schreiben │                │ Sensoren lesen   │
└──────────────┘                └──────────────────┘
                                ┌──────────────────┐
                                │ ws_push_task  [3]│
                                │  JSON broadcast   │
                                └──────────────────┘
```

---

## 5. Datenfluss

### 5.1 Sensor → Anzeige (Echtzeit-Pfad)

```
MAX31865 ──SPI──► sensor.cpp ──► pid_controller ──► heater.cpp ──► MOSFET
  (PT100)         Zustandsautomat    PID-Berechnung    LEDC-PWM     Heizelement
                       │
                       ▼
NAU7802 ──I2C──► nau7802.cpp ──► Median ──► Average ──► load_cell
  (ADC)           24-Bit-Wert      Filter     Filter     Gewicht [g]
                                                            │
                       ┌────────────────────────────────────┘
                       ▼
              ws_push_task ──► JSON {"temp":200,"weight":0.12,...}
                       │
                       ▼ WebSocket
              Browser  ──► Dashboard + Live-Chart (Canvas)
```

### 5.2 Sensor → SD-Karte (Aufzeichnungs-Pfad)

```
hotend_get_temperature() ─┐
load_cell_get_weight_g() ─┼──► datalog_sampler (10 Hz)
motor_get_current_speed()─┘         │
                                    ▼ FreeRTOS Queue (200 Einträge)
                              datalog_writer
                                    │
                                    ▼ RAM-Puffer (16 KB)
                              alle 100 Samples (10 s):
                                    │
                                    ▼ SD.open() → append → close()
                              CSV-Datei auf SD-Karte
```

### 5.3 CSV-Datenformat

```csv
sample,millis,kraft_N,temperatur_C,motor_mm_s,error,loop_us
1,5000,0.00981,200.3,3.00,0x00,1234
2,5100,0.00975,200.1,3.00,0x00,1189
...
```

### 5.4 Benutzer → Aktor (Steuerungspfad)

```
Browser ──WebSocket──► webui.cpp ──► handleWsMessage()
                                          │
                    ┌─────────────────────┼──────────────────┐
                    ▼                     ▼                  ▼
            hotend_set_target()    motor_move()    load_cell_tare()
            hotend_set_pid()       motor_stop()    load_cell_calibrate()
            hotend_set_fan()       motor_set_*()   sequencer_start()
```

---

## 6. Zustandsautomaten

### 6.1 Temperatursensor (Non-Blocking SPI)

```
IDLE ──► SETUP_BIAS ──(1ms)──► SETUP_1_SHOT ──(65ms)──► READ_RTD
  ▲                                                         │
  └─────────────────────────────────────────────────────────┘
                    Callendar-Van-Dusen → °C
```

### 6.2 Sicherheitssystem

```
                    ┌──────────────────────────────┐
                    │        safety_check()         │
                    │  (wird bei jedem PID-Zyklus   │
                    │   aufgerufen, ~164 ms)        │
                    └──────────────┬───────────────┘
                                   │
              ┌────────────────────┼──────────────────┐
              ▼                    ▼                   ▼
      Temp > 300°C?        Aufheizphase:         Haltephase:
      Sensor-Sprung        20s ohne ≥2°C          15°C Abfall
      > 50°C?              Annäherung?            in 30s?
              │                    │                   │
              ▼                    ▼                   ▼
         FAULT_MAX_TEMP    FAULT_THERMAL_RUNAWAY  FAULT_THERMAL_HOLD
              │                    │                   │
              └────────────────────┼───────────────────┘
                                   ▼
                          Heizer AUS, Duty = 0
                          Fault-Flag gesetzt
                          → manueller Reset nötig
```

### 6.3 Sequenzer (Messablauf)

```
     IDLE ──[seq_start()]──► HEATING
                                │
                          Ziel-Temp setzen,
                          warten auf ±2°C
                          stabil für 10s
                          (Timeout: 240s)
                                │
                                ▼
                            RUNNING
                                │
                          Motor starten,
                          Datalog starten,
                          Timer läuft
                                │
                                ▼
                             NEXT
                                │
                    ┌───────────┴────────────┐
                    ▼                        ▼
           Weitere Sequenz?            Alle fertig
                    │                        │
                    ▼                        ▼
                HEATING                    DONE
                (nächster Schritt)           │
                                     Heizer aus,
                                     Motor stop,
                                     Datalog stop
```

### 6.4 Datenlogger

```
IDLE ──[start()]──► RECORDING ──[pause()]──► PAUSED
  ▲                     │                       │
  │                [stop()]                [resume()]
  │                     │                       │
  │                     ▼                       │
  │                 STOPPING ◄──────────────────┘
  │                     │
  │              Flush + Close
  │                     │
  └─────────────────────┘

Bei SD-Fehler:  → ERROR (3× Retry mit Remount, dann Stop)
```

---

## 7. Kommunikationsprotokoll (WebSocket)

### 7.1 Server → Client (Status-Push, 10 Hz)

```json
{
  "t": 12345,
  "temp": 195.3,
  "temp_target": 200.0,
  "duty": 0.75,
  "weight": 0.1234,
  "speed": 5.50,
  "motor": 1,
  "seq": 2,
  "seq_state": "running",
  "seq_remain": 23.5,
  "dl_state": "recording"
}
```

### 7.2 Client → Server (Befehle)

| Befehl | Payload | Funktion |
|--------|---------|----------|
| `set_target` | `{"cmd":"set_target","value":210}` | Soll-Temperatur setzen |
| `autotune` | `{"cmd":"autotune","value":200}` | PID-Autotune starten |
| `set_pid` | `{"cmd":"set_pid","kp":20,"ki":1,"kd":100}` | PID-Parameter setzen |
| `set_fan` | `{"cmd":"set_fan","value":150}` | Lüfter-Duty (0 = Auto) |
| `motor_move` | `{"cmd":"motor_move","speed":5.5,"duration":30}` | Motor-Bewegung |
| `motor_stop` | `{"cmd":"motor_stop"}` | Motor sofort stoppen |
| `load_cell_tare` | `{"cmd":"load_cell_tare"}` | Tara durchführen |
| `load_cell_calibrate` | `{"cmd":"load_cell_calibrate","weight":100}` | Kalibrierung |
| `seq_add` | `{"cmd":"seq_add","temp":200,"speed":3,"duration":60}` | Sequenz hinzufügen |
| `seq_start` | `{"cmd":"seq_start","filename":"test"}` | Sequenz starten |

### 7.3 REST-API

| Route | Methode | Funktion |
|-------|---------|----------|
| `/` | GET | Single-Page-App (LittleFS) |
| `/api/status` | GET | Vollständiger Systemstatus |
| `/api/time/set` | POST | Browser-Zeitsynchronisation |
| `/api/motor/config` | GET | TMC2208-Konfiguration |
| `/api/files/list` | GET | SD-Dateiliste |
| `/api/files/delete/{name}` | DELETE | CSV-Datei löschen |
| `/api/files/download/{name}` | GET | CSV-Download (chunked) |
| `/api/seq/preset/list` | GET | Gespeicherte Presets |
| `/api/seq/preset/{name}` | POST/DELETE | Preset speichern/löschen |

---

## 8. Persistierung (NVS)

| Namespace | Schlüssel | Datentyp | Beschreibung |
|-----------|-----------|----------|--------------|
| `motor` | `esteps` | float | E-Steps/mm (Default: 93.0) |
| `motor` | `esteps_valid` | bool | Kalibrierung gültig? |
| `motor` | `dir_invert` | bool | Richtung invertiert? |
| `motor` | `microstep` | uint8 | Microstepping (8/16/32/64) |
| `motor` | `current_run` | uint16 | Laufstrom [mA] |
| `motor` | `current_hold` | uint16 | Haltestrom [mA] |
| `motor` | `stealthchop` | bool | StealthChop aktiv? |
| `load_cell` | `tare_offset` | int32 | Tara-Offset (Roh-ADC) |
| `load_cell` | `cal_factor` | float | Kalibrierfaktor |

---

## 9. Sicherheitskonzept

### 9.1 Thermische Sicherheit

| Prüfung | Schwellwert | Reaktion |
|---------|-------------|----------|
| Max-Temperatur | > 300 °C | Sofort Heizer AUS |
| Sensor-Sprung | > 50 °C/Zyklus | Sofort Heizer AUS |
| Thermal Runaway (Aufheizen) | 20 s ohne 2 °C Annäherung bei Duty ≥ 127 | Heizer AUS |
| Thermal Runaway (Halten) | 15 °C Abfall unter Ziel in 30 s | Heizer AUS |

Alle Faults erfordern **manuellen Reset** (WebSocket `clear_fault` oder serieller Befehl `R`).

### 9.2 Watchdog

- Task-WDT: 15 Sekunden Timeout
- Bei Timeout: automatischer Neustart des ESP32

### 9.3 Motor-Sicherheit

- Motor wird nach 2 s Inaktivität stromlos geschaltet (`MOTOR_IDLE_TIMEOUT_MS`)
- Enable-Pin wird explizit HIGH gesetzt (Motor deaktiviert)

---

## 10. WiFi-Konfiguration

```
Start
  │
  ├── STA-Modus versuchen (SSID aus config.h)
  │       │
  │       ├── Verbunden → mDNS: pruefstand.local
  │       │
  │       └── Timeout (10s) ──► AP-Fallback
  │                                │
  └── AP-Modus: "Extruder-Pruefstand"
          IP: 192.168.4.1
          Kanal: 6, Max 2 Clients
```

---

## 11. Build-Konfiguration

- **Framework:** Arduino auf ESP-IDF v5 (PlatformIO)
- **Board:** ESP32-WROVER (PSRAM aktiviert)
- **Partitionen:** Benutzerdefiniert (`partitions.csv`)
- **Dateisystem:** LittleFS (Frontend-Dateien)
- **Upload:** 921600 Baud
- **Monitor:** 115200 Baud

### Bibliotheken

| Bibliothek | Zweck |
|-----------|-------|
| ESPAsyncWebServer | HTTP-Server + WebSocket |
| AsyncTCP | TCP-Unterbau für WebServer |
| ArduinoJson | JSON-Serialisierung |
| TMCStepper | TMC2208 Registerabstraktion |

---

## 12. Gesamtablauf (Beispiel: Messsequenz)

```
1. Browser: Sequenz definieren [200°C, 3 mm/s, 60s] × 3 Zyklen
   │
2. WebSocket → sequencer_start("test_run")
   │
3. Sequenzer: HEATING
   ├── hotend_set_target(200)
   └── Warten: Temp ≥ 198°C, stabil 10s
   │
4. Sequenzer: RUNNING
   ├── motor_move(3.0, 60.0, FORWARD)
   ├── datalog_start(100, "test_run_1")
   │
5. Parallel laufend (60 Sekunden):
   │   ├── Hotend-Task: PID-Regelung @ 164 ms
   │   ├── Motor: RMT-Pulsfolge @ 279 Hz (autonom)
   │   ├── Wägezelle: 80 Hz Messung → Filter → Gewicht
   │   ├── Datalog-Sampler: 10 Hz → Queue → Writer → SD
   │   └── WS-Push: 10 Hz → JSON → Browser → Live-Chart
   │
6. Nach 60s: RMT fertig → Motor stromlos
   │
7. Sequenzer: NEXT → Zyklus 2 (zurück zu Schritt 3)
   │
8. Nach 3 Zyklen: DONE
   ├── hotend_set_target(0)
   ├── datalog_stop() (Final-Flush)
   └── Browser: Dateiliste aktualisieren
   │
9. Benutzer: CSV-Download über /api/files/download/...
```
