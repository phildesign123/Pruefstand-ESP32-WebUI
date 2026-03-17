# Modul-Spezifikation: Motorsteuerung — TMC2208 (Extruder)

> **Version:** 0.1 (Entwurf)
> **Datum:** 2026-03-17
> **Plattform:** ESP32-WROVER-E · ESP-IDF (FreeRTOS)

---

## 1  Zweck

Dieses Modul steuert einen Stepper-Motor (Extruder) über den TMC2208-Treiber.
Die Schritterzeugung erfolgt ausschließlich über das RMT-Peripheral des ESP32,
sodass die Step-Pulse hardwaregetaktet und jitterfrei ausgegeben werden —
unabhängig von der CPU-Last anderer Tasks.

Bewegungen werden in physikalischen Einheiten angegeben: **Geschwindigkeit in mm/s**
und **Dauer in Sekunden**. Das Modul rechnet intern über die konfigurierbaren
E-Steps (Steps/mm) in Pulsfrequenzen und Pulszahlen um.

Über die UART-Schnittstelle des TMC2208 können Treiberparameter
(Strom, Mikroschritt, StealthChop/SpreadCycle) zur Laufzeit eingestellt werden.
Ein E-Steps-Kalibrierverfahren ist integriert.

---

## 2  Hardware-Schnittstellen

### 2.1  Step/Dir/Enable — RMT + GPIO

| Signal | GPIO | Richtung | Bemerkung                      |
| ------ | ---- | -------- | ------------------------------ |
| STEP   | 27   | OUT      | RMT Channel 0                  |
| DIR    | 14   | OUT      | Richtung (HIGH/LOW)            |
| EN     | 13   | OUT      | Enable, Active Low             |

- **STEP über RMT:** Der RMT-Kanal erzeugt ein symmetrisches Rechtecksignal
  mit konfigurierbarer Frequenz. Die CPU wird nach dem Start des RMT-Transfers
  **nicht** benötigt — die Pulse laufen autonom in Hardware.
- **Enable:** Wird vor jeder Bewegung auf LOW gezogen (Motor aktiv)
  und kann nach einer konfigurierbaren Idle-Zeit wieder auf HIGH gesetzt werden
  (Motor stromlos, weniger Wärme).

### 2.2  TMC2208 UART

| Signal  | GPIO | Richtung | Bemerkung                      |
| ------- | ---- | -------- | ------------------------------ |
| UART TX | 33   | OUT      | Software-UART zum TMC2208      |
| UART RX | 32   | IN       | Software-UART vom TMC2208      |

- **Protokoll:** TMC2208 UART (Trinamic-proprietär, 8 Byte Frames).
- **Baudrate:** 115200 (TMC2208 Werkseinstellung, einstellbar über Kconfig).
- **Single-Wire-Option:** TX und RX können über einen 1 kΩ Widerstand
  zusammengelegt werden. In diesem Fall wird TX als Open-Drain konfiguriert
  und nach dem Senden auf Input umgeschaltet.

---

## 3  Bewegungsmodell

### 3.1  Einheiten und Umrechnung

Der Aufrufer gibt Bewegungen in physikalischen Einheiten an:

| Parameter        | Einheit | Beschreibung                               |
| ---------------- | ------- | ------------------------------------------ |
| `speed_mm_s`     | mm/s    | Vorschubgeschwindigkeit                    |
| `duration_s`     | s       | Dauer der Bewegung                         |
| `direction`      | enum    | `MOTOR_DIR_FORWARD` / `MOTOR_DIR_REVERSE`  |

Das Modul rechnet intern um:

```
distanz_mm    = speed_mm_s × duration_s
steps_gesamt  = distanz_mm × e_steps_per_mm
frequenz_hz   = speed_mm_s × e_steps_per_mm
puls_periode  = 1 / frequenz_hz
```

### 3.2  Alternativ-API: Distanz statt Dauer

Zusätzlich wird eine distanzbasierte API angeboten:

```
steps_gesamt  = distance_mm × e_steps_per_mm
frequenz_hz   = speed_mm_s × e_steps_per_mm
duration_s    = distance_mm / speed_mm_s
```

### 3.3  E-Steps (Steps/mm)

| Parameter      | Default | Einheit   | Kconfig-Key             |
| -------------- | ------- | --------- | ----------------------- |
| E-Steps        | 93,0    | Steps/mm  | `MOTOR_E_STEPS_PER_MM`  |
| Mikroschritt   | 16      | —         | `MOTOR_MICROSTEP`       |

Der Default (93 Steps/mm) entspricht einem typischen Direct-Drive-Extruder
mit 16 Mikroschritten. Der exakte Wert wird per E-Steps-Kalibrierung
ermittelt (→ Abschnitt 6).

---

## 4  RMT-basierte Schritterzeugung

### 4.1  Warum RMT statt Timer/Task

| Ansatz           | Jitter       | CPU-Last während Bewegung | Eignung              |
| ---------------- | ------------ | ------------------------- | -------------------- |
| `vTaskDelay`     | ±1 ms+       | hoch (busy loop nötig)    | unbrauchbar          |
| HW-Timer ISR     | < 5 µs       | mittel (ISR pro Step)     | funktional, aber ISR-Last |
| **RMT-Peripheral** | **< 1 µs** | **≈ 0 % (DMA-basiert)**  | **ideal**            |

Das RMT-Peripheral erzeugt die Step-Pulse vollständig in Hardware.
Nach dem Befüllen des RMT-Buffers läuft die Pulserzeugung autonom —
die CPU kann andere Tasks bedienen, ohne die Schrittfrequenz zu beeinflussen.

### 4.2  RMT-Konfiguration

| Parameter              | Wert                     | Bemerkung                          |
| ---------------------- | ------------------------ | ---------------------------------- |
| RMT-Kanal              | Channel 0                | —                                  |
| GPIO                   | 27                       | STEP-Signal                        |
| Clock-Divider          | 80                       | 80 MHz / 80 = 1 MHz (1 µs Auflösung) |
| Carrier                | deaktiviert              | —                                  |
| Idle-Level             | LOW                      | STEP ruht auf LOW                  |
| Memory Blocks          | 1 (64 Items)             | Reicht für Loop-Modus              |

### 4.3  Pulse-Erzeugung

Ein Step-Puls besteht aus einem RMT-Item:

```
┌─────────────┐
│   HIGH      │   LOW         │
│  t_pulse    │  t_pause      │
└─────────────┘───────────────┘
              ◄─── Periode ──►
```

- **t_pulse (HIGH-Phase):** Minimum 2 µs (TMC2208-Datenblatt: ≥ 100 ns,
  aber 2 µs gibt Sicherheitsmarge).
- **t_pause (LOW-Phase):** `Periode − t_pulse`
- **Periode:** `1.000.000 / frequenz_hz` µs (bei 1 µs RMT-Ticks)

### 4.4  Frequenzbereich

| Parameter       | Wert       | Berechnung                            |
| --------------- | ---------- | ------------------------------------- |
| Min. Frequenz   | 16 Hz      | 1 mm/s × 16 Steps/mm (Vollschritt)   |
| Max. Frequenz   | 14.880 Hz  | 10 mm/s × 93 Steps/mm × 16 µStep     |
| Min. Periode    | ≈ 67 µs    | Bei Max-Frequenz                      |
| Max. Periode    | 62.500 µs  | Bei Min-Frequenz                      |

Alle Werte liegen komfortabel im RMT-Bereich (1 µs – 32.767 µs pro Phase
bei 1 MHz Clock, verkettbar für längere Perioden).

### 4.5  Bewegungsablauf (Timing-kritisch)

```
    ┌──────────────────────────────────────────────────────┐
    │                 Timing-Diagramm                       │
    │                                                       │
EN  ─┐                                               ┌─────
     └───────────────────────────────────────────────┘
         ▲                                         ▲
         │ t_en_setup ≥ 20 µs                      │ t_idle
         │                                         │
DIR ─────┐                                         │
         └─────────────────────────────────────────────────
         ▲
         │ t_dir_setup ≥ 20 µs
         │
STEP     ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐
    ─────┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘└──────
```

**Kritische Setup-Zeiten (TMC2208-Datenblatt):**

| Parameter          | Min.   | Implementiert | Beschreibung                  |
| ------------------ | ------ | ------------- | ----------------------------- |
| t_dir_setup        | 20 ns  | 20 µs         | DIR stabil bevor erster STEP  |
| t_en_setup         | —      | 20 µs         | EN LOW bevor erster STEP      |
| t_step_high        | 100 ns | 2 µs          | Minimale HIGH-Phase           |

Die implementierten Werte sind bewusst **100×–1000× über dem Minimum**,
um Signallaufzeiten und Pegelwandler zu kompensieren.

### 4.6  RMT End-of-Transfer Callback

Nach dem letzten Puls löst der RMT-Kanal einen `rmt_tx_end_callback` aus.
Dieser Callback:

1. Gibt eine Semaphore frei, auf die der Aufrufer optional warten kann.
2. Postet ein `MOTOR_EVENT_MOVE_DONE`-Event.
3. Startet optional den Idle-Timer (Motor nach Timeout deaktivieren).

Die Callback-Ausführung ist ISR-Kontext — es werden nur Semaphore und
Event-Flags gesetzt, keine blockierenden Aufrufe.

---

## 5  TMC2208-Konfiguration über UART

### 5.1  Konfigurierbare Parameter

| Parameter            | Register     | Default       | Kconfig-Key               |
| -------------------- | ------------ | ------------- | ------------------------- |
| Motorstrom RMS       | IHOLD_IRUN  | 800 mA        | `MOTOR_CURRENT_MA`        |
| Haltestrom           | IHOLD_IRUN  | 400 mA        | `MOTOR_HOLD_CURRENT_MA`   |
| Haltestrom-Delay     | IHOLD_IRUN  | 6 (IHOLDDELAY)| `MOTOR_HOLD_DELAY`        |
| Mikroschritt         | CHOPCONF    | 16            | `MOTOR_MICROSTEP`         |
| StealthChop          | GCONF       | ein           | `MOTOR_STEALTHCHOP`       |
| SpreadCycle-Schwelle | TPWMTHRS    | 0 (aus)       | `MOTOR_TPWMTHRS`          |
| Interpolation 256µS  | CHOPCONF    | ein           | `MOTOR_INTPOL`            |

### 5.2  UART-Kommunikation

Die Kommunikation mit dem TMC2208 erfolgt über das Trinamic-UART-Protokoll:

- **Frame-Aufbau:** Sync (0x05) + Slave-Addr + Register + Daten + CRC8
- **Lese-Request:** 4 Byte senden, 8 Byte Antwort empfangen
- **Schreib-Request:** 8 Byte senden, keine Antwort

Es wird ein dedizierter UART-Zugriffsmutex verwendet, um gleichzeitige
Zugriffe aus verschiedenen Tasks zu verhindern (z.B. Stromänderung während
Statusabfrage).

### 5.3  Register-Verifikation

Nach jedem Schreibzugriff wird das Register zurückgelesen und verglichen.
Bei Abweichung wird ein Retry (max. 3×) durchgeführt und danach ein
Fehler-Event gepostet.

---

## 6  E-Steps-Kalibrierung

### 6.1  Zweck

Die E-Steps (Steps pro mm Filament) hängen von der mechanischen Konfiguration
ab (Zahnrad-Übersetzung, Hobb-Durchmesser, Mikroschritt). Der Werkswert
muss durch Messung korrigiert werden.

### 6.2  Kalibrierablauf

```
1.  Filament markieren bei genau 100 mm über dem Extruder-Eingang
2.  Kalibrierung starten: motor_calibrate_esteps(100.0, speed_mm_s)
3.  Modul extrudiert 100 mm mit aktuellen E-Steps
4.  Nutzer misst die tatsächlich verbleibende Strecke zum Marker
5.  Nutzer gibt den gemessenen Rest ein: motor_calibrate_esteps_apply(remaining_mm)
6.  Neuer E-Steps-Wert wird berechnet:
        e_steps_neu = e_steps_alt × (100.0 / (100.0 − remaining_mm))
7.  Wert wird im NVS gespeichert
```

### 6.3  Beispielrechnung

```
Markierung:        100 mm
Verbleibt:         8 mm  →  tatsächlich extrudiert: 92 mm
Alter E-Steps:     93,0 Steps/mm

e_steps_neu = 93,0 × (100,0 / 92,0) = 101,09 Steps/mm
```

### 6.4  Persistierung (NVS)

| NVS-Key           | Typ     | Beschreibung                     |
| ------------------ | ------- | -------------------------------- |
| `e_steps`          | float   | Kalibrierte E-Steps/mm           |
| `e_steps_valid`    | uint8   | 1 = kalibriert, 0 = Default      |
| `tmc_current_ma`   | uint16  | Gespeicherter Motorstrom          |
| `tmc_microstep`    | uint8   | Gespeicherter Mikroschritt        |

Beim Init werden gespeicherte Werte geladen. Fehlen sie, werden die
Kconfig-Defaults verwendet und ein Warn-Event gepostet.

---

## 7  Timing-Sicherheit und Ressourcentrennung

Dieses Modul interagiert mit zeitkritischen Peripherals. Folgende
Maßnahmen verhindern Timing-Probleme:

### 7.1  Prinzip: CPU-Entkopplung

| Ressource           | Verantwortung       | CPU beteiligt?            |
| -------------------- | ------------------- | ------------------------- |
| Step-Pulse           | RMT-Hardware        | Nein (nach Start autonom) |
| Pulse zählen         | RMT End-Callback    | Nur ISR am Ende           |
| DIR/EN setzen        | GPIO direkt          | Ja, aber nur einmalig     |
| TMC2208 UART         | Software-UART Task   | Ja, aber nicht zeitkritisch |

**Konsequenz:** Kein FreeRTOS-Task muss in Echtzeit laufen, um die
Schrittfrequenz zu halten. Die CPU kann ausgelastet sein (SPI, I2C,
Wi-Fi, HTTP-Server), ohne dass Motor-Schritte verloren gehen.

### 7.2  Keine Bewegung während UART-Konfiguration

TMC2208 UART-Zugriffe und RMT-Bewegungen dürfen **nicht gleichzeitig** erfolgen.
Register-Schreibvorgänge (insbesondere CHOPCONF, GCONF) können den Treiber
kurzzeitig in einen undefinierten Zustand bringen.

**Regel:** Der UART-Mutex und der Bewegungs-Zustand werden gemeinsam geprüft:

```c
// Pseudocode
if (motor_is_moving()) {
    return ESP_ERR_INVALID_STATE;  // UART-Zugriff abgelehnt
}
xSemaphoreTake(uart_mutex, portMAX_DELAY);
// ... Register schreiben/lesen ...
xSemaphoreGive(uart_mutex);
```

### 7.3  Setup-Zeiten per ets_delay_us()

Die DIR- und EN-Setup-Zeiten (20 µs) werden über `ets_delay_us()` realisiert,
**nicht** über `vTaskDelay()` (Granularität nur 1 ms / 1 Tick).
Da es sich um einmalige Mikrosekunden-Delays handelt, ist der Busy-Wait
akzeptabel und beeinflusst keine anderen Tasks.

### 7.4  RMT-Buffer-Management

Bei langen Bewegungen (viele Steps) wird der RMT-Buffer im Loop-Modus
betrieben oder per `rmt_translator_init()` mit einem Callback befüllt,
der die nächsten Pulse-Daten nachliefert. So können Millionen von Steps
erzeugt werden, obwohl der RMT-Buffer nur 64 Items fasst.

**Achtung:** Der Translator-Callback läuft im ISR-Kontext.
Er darf nur einfache Berechnungen durchführen (nächstes RMT-Item befüllen)
und keine blockierenden Aufrufe oder Mutex-Operationen enthalten.

### 7.5  Prioritäten-Übersicht (Gesamtsystem)

| Priorität | Task                | Begründung                                |
| --------- | ------------------- | ----------------------------------------- |
| 7         | RMT ISR / Callbacks | Hardware-ISR, höchste Prio                |
| 6         | `load_cell_task`    | 80-Hz-Abtastung, jitterempfindlich        |
| 5         | `hotend_pid_task`   | 4-Hz-Regelung, toleranter                 |
| 4         | `motor_mgr_task`    | Bewegungskoordination, nicht zeitkritisch |
| 3         | HTTP / Web-UI       | Benutzerinteraktion                       |

Der Motor-Management-Task hat niedrige Priorität, weil die zeitkritische
Arbeit (Pulse) in der RMT-Hardware stattfindet. Der Task koordiniert nur
Start/Stop, UART-Konfiguration und Events.

---

## 8  Software-Architektur

### 8.1  FreeRTOS-Task

| Eigenschaft       | Wert                              |
| ----------------- | --------------------------------- |
| Task-Name         | `motor_mgr_task`                  |
| Stack-Größe       | 4096 Byte                         |
| Priorität         | 4 (mittel)                        |
| Core-Affinität    | Kein Pinning (tskNO_AFFINITY)     |
| Funktion          | Befehlsqueue abarbeiten           |

Der Task wartet auf eine FreeRTOS-Queue (`motor_cmd_queue`), über die
Bewegungsbefehle und Konfigurationsaufträge eingehen. Dadurch sind
alle Zugriffe serialisiert — keine Race Conditions zwischen API-Aufrufen
aus verschiedenen Tasks.

### 8.2  Befehlsqueue

```c
typedef enum {
    MOTOR_CMD_MOVE,           // Bewegung: speed + duration
    MOTOR_CMD_MOVE_DIST,      // Bewegung: speed + distance
    MOTOR_CMD_STOP,           // Sofortstopp (RMT abbrechen)
    MOTOR_CMD_SET_CURRENT,    // Motorstrom ändern (UART)
    MOTOR_CMD_SET_MICROSTEP,  // Mikroschritt ändern (UART)
    MOTOR_CMD_READ_STATUS,    // TMC2208 Status auslesen
    MOTOR_CMD_CAL_ESTEPS,     // E-Steps-Kalibrierung Phase 1
    MOTOR_CMD_CAL_APPLY,      // E-Steps-Kalibrierung Phase 2
} motor_cmd_type_t;

typedef struct {
    motor_cmd_type_t type;
    union {
        struct { float speed_mm_s; float duration_s; motor_dir_t dir; } move;
        struct { float speed_mm_s; float distance_mm; motor_dir_t dir; } move_dist;
        struct { float known_distance_mm; } cal_esteps;
        struct { float remaining_mm; } cal_apply;
        struct { uint16_t current_ma; } set_current;
        struct { uint8_t microstep; } set_microstep;
    };
} motor_cmd_t;
```

### 8.3  Öffentliche API

```c
/**
 * @brief Modul initialisieren (RMT, GPIO, UART, NVS laden, Task starten)
 * @return ESP_OK bei Erfolg
 */
esp_err_t motor_init(void);

/* ── Bewegung ────────────────────────────────────────── */

/**
 * @brief Bewegung mit Geschwindigkeit und Dauer
 * @param speed_mm_s   Vorschub in mm/s (> 0)
 * @param duration_s   Dauer in Sekunden (> 0)
 * @param dir          MOTOR_DIR_FORWARD oder MOTOR_DIR_REVERSE
 * @return ESP_OK wenn Befehl in Queue eingereiht
 */
esp_err_t motor_move(float speed_mm_s, float duration_s, motor_dir_t dir);

/**
 * @brief Bewegung mit Geschwindigkeit und Distanz
 * @param speed_mm_s   Vorschub in mm/s (> 0)
 * @param distance_mm  Strecke in mm (> 0)
 * @param dir          MOTOR_DIR_FORWARD oder MOTOR_DIR_REVERSE
 */
esp_err_t motor_move_distance(float speed_mm_s, float distance_mm,
                              motor_dir_t dir);

/**
 * @brief Bewegung sofort abbrechen (RMT Stop)
 */
esp_err_t motor_stop(void);

/**
 * @brief Blockierend warten, bis aktuelle Bewegung abgeschlossen
 * @param timeout_ms  Max. Wartezeit (portMAX_DELAY für unendlich)
 */
esp_err_t motor_wait_done(uint32_t timeout_ms);

/**
 * @brief Prüfen, ob Motor gerade fährt
 */
bool motor_is_moving(void);

/* ── TMC2208 Konfiguration (UART) ───────────────────── */

/**
 * @brief Motorstrom setzen
 * @param run_ma   Fahrstrom in mA
 * @param hold_ma  Haltestrom in mA (0 = automatisch 50% von run_ma)
 */
esp_err_t motor_set_current(uint16_t run_ma, uint16_t hold_ma);

/**
 * @brief Mikroschritt-Auflösung setzen
 * @param microstep  1, 2, 4, 8, 16, 32, 64, 128, 256
 */
esp_err_t motor_set_microstep(uint8_t microstep);

/**
 * @brief StealthChop ein-/ausschalten
 */
esp_err_t motor_set_stealthchop(bool enable);

/**
 * @brief TMC2208-Statusregister auslesen
 * @param[out] status  Zielstruktur für Treiberstatus
 */
esp_err_t motor_get_tmc_status(motor_tmc_status_t *status);

/* ── E-Steps Kalibrierung ────────────────────────────── */

/**
 * @brief Kalibrierung Phase 1: Referenzstrecke extrudieren
 * @param distance_mm  Soll-Strecke (empfohlen: 100 mm)
 * @param speed_mm_s   Extrusionsgeschwindigkeit
 */
esp_err_t motor_calibrate_esteps(float distance_mm, float speed_mm_s);

/**
 * @brief Kalibrierung Phase 2: Restlänge eingeben, neuen Wert berechnen
 * @param remaining_mm  Gemessene verbleibende Strecke bis zur Markierung
 */
esp_err_t motor_calibrate_esteps_apply(float remaining_mm);

/**
 * @brief Aktuelle E-Steps abfragen
 */
float motor_get_esteps(void);

/**
 * @brief E-Steps manuell setzen und im NVS speichern
 */
esp_err_t motor_set_esteps(float steps_per_mm);

/* ── Lifecycle ───────────────────────────────────────── */

/**
 * @brief Motor deaktivieren (EN HIGH) und Modul herunterfahren
 */
esp_err_t motor_deinit(void);
```

### 8.4  Events (ESP-IDF Event Loop)

| Event-ID                         | Payload              | Beschreibung                         |
| -------------------------------- | -------------------- | ------------------------------------ |
| `MOTOR_EVENT_MOVE_DONE`         | `float distance_mm`  | Bewegung abgeschlossen               |
| `MOTOR_EVENT_MOVE_ABORTED`      | —                    | Bewegung abgebrochen (motor_stop)    |
| `MOTOR_EVENT_CAL_PHASE1_DONE`   | —                    | Referenzstrecke extrudiert            |
| `MOTOR_EVENT_CAL_DONE`          | `float new_esteps`   | Neue E-Steps berechnet und gespeichert |
| `MOTOR_EVENT_TMC_FAULT`         | `uint32_t status`    | TMC2208 meldet Fehler (Überstrom, Übertemp.) |
| `MOTOR_EVENT_TMC_COMM_ERR`      | —                    | UART-Kommunikation fehlgeschlagen     |

### 8.5  Dateistruktur

```
components/motor/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── motor.h                // Öffentliche API, Events, Typen
└── src/
    ├── motor.c                 // Task, Queue, Bewegungslogik, E-Steps
    ├── motor_rmt.c             // RMT-Setup, Pulse-Erzeugung, Translator
    ├── motor_rmt.h             // Interner Header
    ├── tmc2208_uart.c          // UART-Treiber für TMC2208
    ├── tmc2208_uart.h          // Register-Definitionen, Read/Write
    └── tmc2208_regs.h          // Register-Adressen und Bitmasken
```

---

## 9  Test-Modus (Serial Monitor)

### 9.1  Zweck

Eigenständige Test-App zur Inbetriebnahme des Motors, ohne dass
Web-UI oder andere Module laufen müssen.

### 9.2  Befehle (Serial CLI)

| Befehl                            | Beschreibung                                                  |
| --------------------------------- | ------------------------------------------------------------- |
| `status`                          | TMC2208-Status ausgeben (Strom, µStep, Temperatur, Fehler)    |
| `move <speed> <duration> [fwd\|rev]` | Bewegung starten, z.B. `move 5.0 10.0 fwd` (5 mm/s, 10 s)  |
| `move_dist <speed> <dist> [fwd\|rev]`| Distanz-Bewegung, z.B. `move_dist 3.0 50.0 fwd` (50 mm)    |
| `stop`                            | Bewegung sofort abbrechen                                     |
| `current <run_ma> [hold_ma]`      | Motorstrom setzen, z.B. `current 800 400`                     |
| `microstep <1\|2\|4\|8\|16\|...\|256>`| Mikroschritt-Auflösung, z.B. `microstep 16`              |
| `stealthchop <on\|off>`           | StealthChop umschalten                                        |
| `esteps`                          | Aktuellen E-Steps-Wert anzeigen                               |
| `esteps set <wert>`               | E-Steps manuell setzen, z.B. `esteps set 101.09`             |
| `cal start [dist] [speed]`        | Kalibrierung Phase 1 (Default: 100 mm, 3 mm/s)               |
| `cal apply <remaining_mm>`        | Kalibrierung Phase 2, z.B. `cal apply 8.0`                   |
| `enable`                          | Motor aktivieren (EN LOW)                                     |
| `disable`                         | Motor deaktivieren (EN HIGH)                                  |
| `help`                            | Alle verfügbaren Befehle auflisten                            |

### 9.3  Ausgabeformat

#### Status

```
[MOTOR] TMC2208 Status:
  Run Current:  800 mA
  Hold Current: 400 mA
  Microstep:    1/16
  StealthChop:  ON
  Interpolation:ON (256 µStep)
  Driver Temp:  OK
  OT Warning:   NO
  E-Steps:      93.00 Steps/mm (DEFAULT — nicht kalibriert)
```

#### Bewegung

```
[MOTOR] Move: 5.00 mm/s × 10.00 s = 50.00 mm (4650 steps @ 465 Hz) FWD
[MOTOR] Moving... ████████████████████ 100%
[MOTOR] Move done. Actual steps: 4650
```

#### Kalibrierung

```
[MOTOR] E-Steps Calibration Phase 1:
  Extruding 100.00 mm at 3.00 mm/s (9300 steps)...
[MOTOR] Done. Measure remaining distance to marker and enter:
  > cal apply <remaining_mm>

> cal apply 8.0
[MOTOR] E-Steps Calibration Phase 2:
  Commanded: 100.00 mm | Actual: 92.00 mm
  Old E-Steps: 93.00 | New E-Steps: 101.09
  Saved to NVS.
```

### 9.4  Dateistruktur Test-Code

```
test/motor_test/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 // App-Entry: motor_init(), CLI starten
│   └── serial_cli.c           // UART-Lese-Task, Befehlsparser
│   └── serial_cli.h
└── sdkconfig.defaults         // UART0 115200, Log-Level INFO
```

### 9.5  Testprotokoll

| Schritt | Aktion                                   | Erwartetes Ergebnis                              |
| ------- | ---------------------------------------- | ------------------------------------------------ |
| 1       | Board starten, `status`                  | TMC2208 antwortet, Default-Werte korrekt         |
| 2       | `current 600`                            | Strom geändert, Rücklesen bestätigt              |
| 3       | `microstep 16`                           | µStep geändert, Rücklesen bestätigt              |
| 4       | `move 2.0 5.0 fwd`                      | Motor dreht 5 s lang, gleichmäßig, kein Stottern |
| 5       | `move 2.0 5.0 rev`                      | Motor dreht rückwärts                             |
| 6       | `move 10.0 2.0 fwd`, währenddessen `stop`| Motor stoppt sofort                              |
| 7       | Filament markieren, `cal start 100 3.0`  | Motor extrudiert 100 mm (ca. 33 s)               |
| 8       | Rest messen → `cal apply 8.0`           | Neuer E-Steps-Wert ≈ 101 Steps/mm, in NVS        |
| 9       | Board neu starten, `esteps`              | Kalibrierter Wert aus NVS geladen                 |
| 10      | `move_dist 3.0 50.0 fwd`                | Motor extrudiert exakt 50 mm (Verifikation)       |

---

## 10  Web-UI-Anbindung (Produktivbetrieb)

### 10.1  Abgrenzung

Im Produktivbetrieb wird das Motor-Modul über die Web-UI gesteuert.
Das `motor`-Component bleibt **unverändert** — die Web-UI nutzt
ausschließlich die öffentliche API (Abschnitt 8.3) und Events (Abschnitt 8.4).

### 10.2  Vorgesehene Web-UI-Funktionen

| Funktion                  | API-Aufruf                               | HTTP-Endpunkt (Vorschlag)     |
| ------------------------- | ---------------------------------------- | ----------------------------- |
| Extrudieren (speed+dauer) | `motor_move()`                           | `POST /api/motor/move`        |
| Extrudieren (speed+dist)  | `motor_move_distance()`                  | `POST /api/motor/move_dist`   |
| Sofortstopp               | `motor_stop()`                           | `POST /api/motor/stop`        |
| Motor-Status              | `motor_get_tmc_status()` + `motor_is_moving()` | `GET /api/motor/status`  |
| Strom einstellen          | `motor_set_current()`                    | `POST /api/motor/current`     |
| Mikroschritt einstellen   | `motor_set_microstep()`                  | `POST /api/motor/microstep`   |
| E-Steps anzeigen/setzen   | `motor_get_esteps()` / `motor_set_esteps()` | `GET/POST /api/motor/esteps` |
| Kalibrierung starten      | `motor_calibrate_esteps()`               | `POST /api/motor/cal/start`   |
| Kalibrierung abschließen  | `motor_calibrate_esteps_apply()`         | `POST /api/motor/cal/apply`   |

### 10.3  Architektur-Hinweis

```
┌──────────────┐       Queue / Events       ┌──────────────────┐
│    motor     │ ◄──────────────────────── │   HTTP-Server    │
│  Component   │ ──────────────────────►   │   (Web-UI-Modul) │
│  (diese Spec)│    move(), stop(),        │                  │
│              │    set_current(), ...     └──────────────────┘
└──────────────┘                                    │
                                                    ▼
                                            Browser / Web-UI
```

Das `motor`-Component hat **keine Abhängigkeit** zum HTTP-Server.

---

## 11  Abhängigkeiten

| Abhängigkeit           | Richtung    | Beschreibung                                        |
| ---------------------- | ----------- | --------------------------------------------------- |
| **RMT-Peripheral**     | nutzt       | Channel 0 für Step-Pulse                            |
| **NVS**                | nutzt       | E-Steps, Motorstrom, Mikroschritt persistieren      |
| **GPIO-Driver**        | nutzt       | DIR (14), EN (13)                                   |
| **ESP Event Loop**     | publiziert  | Bewegungs- und Fehler-Events                         |

---

## 12  Offene Punkte / TODOs

- [ ] Abgleich mit bestehendem Code aus dem Marlin-ESP32-Projekt
- [ ] Beschleunigungsrampen (Trapezprofil) — aktuell nur konstante Geschwindigkeit
- [ ] StallGuard-Erkennung (TMC2208 hat kein StallGuard, ggf. TMC2209 evaluieren)
- [ ] Maximale Geschwindigkeit und Beschleunigung per Kconfig begrenzen
- [ ] Klären: Braucht das Modul einen Positions-Tracker (Schritte zählen)?
- [ ] Single-Wire-UART implementieren oder 2-Wire beibehalten?
- [ ] Web-UI-Modul als separate Spec schreiben
