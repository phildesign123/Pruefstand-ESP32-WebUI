# Modul-Spezifikation: Wägezelle — NAU7802

> **Version:** 0.1 (Entwurf)
> **Datum:** 2026-03-17
> **Plattform:** ESP32-WROVER-E · ESP-IDF (FreeRTOS)

---

## 1  Zweck

Dieses Modul liest eine Wägezelle über den 24-Bit-ADC NAU7802 per I2C aus.
Es stellt Funktionen für Tarierung (Nullpunkt setzen) und Kalibrierung
(Skalierung auf Gramm/Kilogramm) bereit. Die Rohdaten werden mit 80 Hz
abgetastet und über eine zweistufige Filterpipeline (Median → Moving Average)
geglättet, um ein stabiles Gewichtssignal zu liefern.

---

## 2  Hardware-Schnittstellen

### 2.1  NAU7802 (I2C)

| Signal | GPIO | Richtung | Bemerkung               |
| ------ | ---- | -------- | ----------------------- |
| SDA    | 21   | I/O      | I2C Default, shared     |
| SCL    | 22   | OUT      | I2C Default, shared     |
| DRDY   | 39   | IN       | Input-only, Interrupt   |

- **I2C-Adresse:** 0x2A (fest, nicht konfigurierbar)
- **I2C-Takt:** 400 kHz (Fast Mode)
- **Bus-Sharing:** Der I2C-Bus wird mit dem ADS1115 (0x48) geteilt.
  Kein Adresskonflikt. Zugriffe werden über einen gemeinsamen I2C-Mutex serialisiert.
- **Pull-ups:** 4,7 kΩ nach 3,3 V auf SDA und SCL
  (bei Leitungslängen > 30 cm auf 2,2 kΩ reduzieren).

### 2.2  DRDY-Interrupt

- GPIO 39 wird als fallende Flanke (oder steigende, je nach NAU7802-Konfiguration)
  per GPIO-ISR konfiguriert.
- Die ISR gibt eine binäre Semaphore frei, auf die der Lese-Task wartet.
  So wird exakt mit der ADC-Abtastrate (80 SPS) synchronisiert,
  ohne Polling oder Timer-Jitter.

---

## 3  NAU7802-Konfiguration

### 3.1  Register-Setup beim Init

| Register       | Wert / Bits          | Beschreibung                        |
| -------------- | -------------------- | ----------------------------------- |
| PU_CTRL (0x00) | RR=1, PUD=1, PUA=1, CS=1 | Power-Up-Sequenz, Cycle Start  |
| CTRL1 (0x01)   | GAINS = 128          | Verstärkung 128× (Standard Wägezelle) |
| CTRL2 (0x02)   | CRS = 0b111          | Conversion Rate Select: 80 SPS     |
| I2C_CTRL (0x11)| —                    | Default belassen                    |
| ADC (0x15)     | REG_CHPS = 1         | Chopper-Stabilisierung aktiv        |

### 3.2  Interne Kalibrierung

Nach dem Power-Up wird die interne Offset-Kalibrierung des NAU7802 ausgelöst
(CTRL2.CALS = 1). Der Task wartet, bis das CAL_ERR-Bit quittiert ist
(Timeout: 500 ms). Bei Fehler wird `ESP_ERR_TIMEOUT` zurückgegeben.

---

## 4  Filterpipeline

Die Rohwerte vom NAU7802 (24 Bit, vorzeichenbehaftet) durchlaufen zwei
aufeinanderfolgende Filterstufen, bevor sie in das Gewichtsergebnis umgerechnet werden.

### 4.1  Stufe 1 — Median-Filter

| Parameter       | Default | Kconfig-Key              |
| --------------- | ------- | ------------------------ |
| Fenstergröße    | 5       | `LOAD_CELL_MEDIAN_SIZE`  |

- Es wird ein gleitendes Fenster der letzten N Rohwerte geführt.
- Pro neuem Sample wird der Median des Fensters berechnet.
- Der Median-Filter entfernt Ausreißer und Spike-Störungen,
  ohne das Signal zu verschmieren.
- Die Fenstergröße muss ungerade sein (wird ggf. aufgerundet).

### 4.2  Stufe 2 — Moving Average

| Parameter       | Default | Kconfig-Key              |
| --------------- | ------- | ------------------------ |
| Fenstergröße    | 10      | `LOAD_CELL_AVG_SIZE`     |

- Die Median-gefilterten Werte werden in einen Ringpuffer geschrieben.
- Pro neuem Wert wird der arithmetische Mittelwert über die letzten M Werte berechnet.
- Der Moving Average glättet verbleibendes Rauschen und liefert
  ein stabiles Gewichtssignal.

### 4.3  Signalfluss

```
NAU7802 (80 SPS, 24 Bit)
    │
    ▼
┌──────────────────┐
│  Median-Filter   │  Fenstergröße: 5 Samples
│  (Ausreißer)     │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│  Moving Average  │  Fenstergröße: 10 Samples
│  (Glättung)      │
└────────┬─────────┘
         │
         ▼
  Tarierung & Kalibrierung
    │
    ▼
  Gewicht in Gramm
```

### 4.4  Latenz und effektive Bandbreite

Bei 80 SPS und den Default-Filtergrößen ergibt sich:

- **Latenz Median:** (5 − 1) / 2 = 2 Samples → 25 ms
- **Latenz Moving Average:** (10 − 1) / 2 = 4,5 Samples → 56 ms
- **Gesamtlatenz:** ca. 80 ms (6,5 Samples bei 80 Hz)
- **Effektive Ausgaberate:** 80 Hz (ein gefilterter Wert pro ADC-Sample)

---

## 5  Tarierung und Kalibrierung

### 5.1  Tarierung (Nullpunkt)

Beim Aufruf von `load_cell_tare()` wird:

1. Eine konfigurierbare Anzahl Samples gesammelt (Default: 80 = 1 Sekunde bei 80 SPS).
2. Der Mittelwert dieser Samples wird als **Tara-Offset** gespeichert.
3. Ab sofort wird dieser Offset von jedem gefilterten Rohwert abgezogen.

| Parameter        | Default | Kconfig-Key              |
| ---------------- | ------- | ------------------------ |
| Tara-Samples     | 80      | `LOAD_CELL_TARE_SAMPLES` |

- Tarierung ist jederzeit wiederholbar.
- Der Tara-Offset wird im RAM gehalten und geht bei Neustart verloren
  (optional: NVS-Persistierung über API).

### 5.2  Kalibrierung (Skalierung)

Die Kalibrierung rechnet den tara-korrigierten Rohwert in eine physikalische
Einheit (Gramm) um. Dazu wird ein **Kalibrierfaktor** benötigt:

```
gewicht_g = (rohwert_filtered − tara_offset) / kalibrierfaktor
```

#### Kalibrierablauf

1. Wägezelle ohne Last → `load_cell_tare()` aufrufen.
2. Bekanntes Referenzgewicht (z.B. 500 g) auflegen.
3. `load_cell_calibrate(float known_weight_g)` aufrufen.
4. Das Modul sammelt erneut Samples (wie bei Tarierung),
   berechnet den Mittelwert und leitet den Kalibrierfaktor ab:
   `kalibrierfaktor = (rohwert_mittel − tara_offset) / known_weight_g`
5. Der Faktor wird im NVS gespeichert und beim nächsten Boot automatisch geladen.

| Parameter         | Default    | Kconfig-Key                |
| ----------------- | ---------- | -------------------------- |
| Kalibrier-Samples | 80         | `LOAD_CELL_CAL_SAMPLES`    |
| NVS-Namespace     | `loadcell` | `LOAD_CELL_NVS_NAMESPACE`  |

### 5.3  Persistierung (NVS)

| NVS-Key           | Typ     | Beschreibung                    |
| ------------------ | ------- | ------------------------------- |
| `cal_factor`       | float   | Kalibrierfaktor (Rohwert/Gramm) |
| `cal_valid`        | uint8   | 1 = kalibriert, 0 = ungültig   |

Beim Init prüft das Modul, ob ein gültiger Kalibrierfaktor im NVS vorliegt.
Falls nicht, wird ein Default-Faktor verwendet und ein Warn-Event gepostet.

---

## 6  Software-Architektur

### 6.1  FreeRTOS-Task

| Eigenschaft       | Wert                              |
| ----------------- | --------------------------------- |
| Task-Name         | `load_cell_task`                  |
| Stack-Größe       | 4096 Byte                         |
| Priorität         | 6 (hoch — zeitkritisches Sampling)|
| Core-Affinität    | Kein Pinning (tskNO_AFFINITY)     |
| Synchronisation   | DRDY-Semaphore (kein Polling)     |

Die Priorität ist höher als die des Hotend-PID-Tasks (5),
da die 80-Hz-Abtastung keinen Jitter toleriert.

### 6.2  Ablauf pro Zyklus

```
1.  Auf DRDY-Semaphore warten (max. 20 ms Timeout)
2.  I2C-Mutex nehmen
3.  24-Bit-Rohwert aus ADC-Register lesen (3 Byte)
4.  I2C-Mutex freigeben
5.  Rohwert in Median-Filter einspeisen → Median berechnen
6.  Median-Wert in Moving-Average einspeisen → Mittelwert berechnen
7.  Tara-Offset abziehen, Kalibrierfaktor anwenden → Gewicht in Gramm
8.  Ergebnis in Thread-sichere Variable schreiben (atomarer Zugriff)
9.  Optional: Event posten bei signifikanter Gewichtsänderung
```

### 6.3  Öffentliche API

```c
/**
 * @brief Modul initialisieren (I2C-Device, GPIO-ISR, Task starten)
 * @param i2c_port  I2C-Port (I2C_NUM_0 oder I2C_NUM_1)
 * @param i2c_mutex Gemeinsamer I2C-Bus-Mutex
 * @return ESP_OK bei Erfolg
 */
esp_err_t load_cell_init(i2c_port_t i2c_port,
                         SemaphoreHandle_t i2c_mutex);

/**
 * @brief Tarierung durchführen (Nullpunkt setzen)
 *
 * Blockiert für ca. 1 Sekunde (80 Samples bei 80 SPS).
 * Kann von jedem Task aufgerufen werden.
 */
esp_err_t load_cell_tare(void);

/**
 * @brief Kalibrierung mit bekanntem Referenzgewicht
 * @param known_weight_g  Referenzgewicht in Gramm
 *
 * Blockiert für ca. 1 Sekunde. Speichert Ergebnis im NVS.
 */
esp_err_t load_cell_calibrate(float known_weight_g);

/**
 * @brief Aktuelles Gewicht abfragen (gefiltert, tariert, kalibriert)
 * @return Gewicht in Gramm (0.0 wenn nicht kalibriert)
 */
float load_cell_get_weight_g(void);

/**
 * @brief Rohwert abfragen (gefiltert, aber ohne Tarierung/Kalibrierung)
 * @return 24-Bit-Rohwert nach Filterpipeline
 */
int32_t load_cell_get_raw(void);

/**
 * @brief Prüfen, ob gültige Kalibrierung vorhanden ist
 */
bool load_cell_is_calibrated(void);

/**
 * @brief Modul stoppen und Ressourcen freigeben
 */
esp_err_t load_cell_deinit(void);
```

### 6.4  Events (ESP-IDF Event Loop)

| Event-ID                          | Payload           | Beschreibung                         |
| --------------------------------- | ----------------- | ------------------------------------ |
| `LOAD_CELL_EVENT_TARE_DONE`      | —                 | Tarierung abgeschlossen              |
| `LOAD_CELL_EVENT_CAL_DONE`       | `float factor`    | Kalibrierung abgeschlossen           |
| `LOAD_CELL_EVENT_CAL_MISSING`    | —                 | Keine gültige Kalibrierung im NVS    |
| `LOAD_CELL_EVENT_DRDY_TIMEOUT`   | —                 | DRDY-Signal ausgeblieben (Sensorfehler) |

### 6.5  Dateistruktur

```
components/load_cell/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── load_cell.h            // Öffentliche API, Events, Typen
└── src/
    ├── load_cell.c             // Task, Tarierung, Kalibrierung, NVS
    ├── nau7802.c               // I2C-Treiber für NAU7802
    ├── nau7802.h               // Interner Header (Register-Definitionen)
    ├── filter_median.c         // Median-Filter Implementierung
    ├── filter_median.h
    ├── filter_avg.c            // Moving-Average Implementierung
    └── filter_avg.h
```

---

## 7  Abhängigkeiten

| Abhängigkeit           | Richtung    | Beschreibung                                       |
| ---------------------- | ----------- | -------------------------------------------------- |
| **I2C-Bus**            | nutzt       | Bus muss von übergeordnetem Modul initialisiert werden |
| **I2C-Mutex**          | nutzt       | Gemeinsamer Mutex mit ADS1115-Modul               |
| **NVS**                | nutzt       | Speicherung des Kalibrierfaktors                   |
| **GPIO-ISR-Service**   | nutzt       | Muss vor Init installiert sein (`gpio_install_isr_service()`) |
| **ESP Event Loop**     | publiziert  | Events für Tarierung, Kalibrierung, Fehler          |

---

## 8  Test-Modus (Serial Monitor)

Zum Testen und Inbetriebnehmen des Moduls wird ein separater Test-Code
bereitgestellt, der über den UART0 Serial Monitor bedient wird.

### 8.1  Zweck

- Hardware-Validierung: Prüfen, ob der NAU7802 korrekt antwortet und Werte liefert.
- Tarierung und Kalibrierung interaktiv durchführen, ohne dass die restliche
  Firmware (Web-UI, andere Module) laufen muss.
- Filterpipeline beobachten und Parameter schnell iterieren.
- Dient als Referenz-Implementierung für die spätere Web-UI-Anbindung.

### 8.2  Befehle (Serial CLI)

Die Befehle werden über UART0 (115200 Baud) als Textzeilen empfangen.
Groß-/Kleinschreibung wird ignoriert.

| Befehl                        | Beschreibung                                              |
| ----------------------------- | --------------------------------------------------------- |
| `status`                      | NAU7802-Status ausgeben (Verbindung, Gain, SPS, Kalibrierung) |
| `raw`                         | Einmalig den aktuellen Rohwert (ungefiltert) ausgeben     |
| `stream [on\|off]`            | Kontinuierliche Ausgabe starten/stoppen (80 Hz)           |
| `stream slow [on\|off]`       | Reduzierte Ausgabe (1 Hz) für manuelles Ablesen           |
| `tare`                        | Tarierung durchführen (1 s Sampling)                      |
| `calibrate <gewicht_g>`       | Kalibrierung mit Referenzgewicht, z.B. `calibrate 500`    |
| `calinfo`                     | Aktuellen Kalibrierfaktor und Tara-Offset anzeigen        |
| `calreset`                    | Kalibrierung aus NVS löschen                              |
| `filter median <size>`        | Median-Fenstergröße ändern, z.B. `filter median 7`        |
| `filter avg <size>`           | Moving-Average-Fenstergröße ändern, z.B. `filter avg 20`  |
| `help`                        | Alle verfügbaren Befehle auflisten                        |

### 8.3  Ausgabeformat

#### Einzelwert (`raw`, `status`)

```
[LOADCELL] Status: OK | Gain: 128 | SPS: 80 | Cal: valid (factor=1823.45)
[LOADCELL] Raw: 1482937 | Filtered: 1482512 | Weight: 245.3 g
```

#### Stream-Modus (80 Hz)

CSV-Format für einfaches Kopieren in Tabellenkalkulationen oder Serial Plotter:

```
# time_ms, raw, median, avg, weight_g
12500, 1482937, 1482800, 1482512, 245.3
12512, 1483102, 1482937, 1482580, 245.4
12525, 1483015, 1482937, 1482610, 245.3
...
```

#### Stream-Modus langsam (1 Hz)

```
[LOADCELL] t=12500ms | Raw=1482937 | Filt=1482512 | 245.3 g | stable
```

Das Feld `stable` / `unstable` zeigt an, ob das Gewicht innerhalb der
letzten 0,5 s um weniger als ±0,5 g geschwankt hat.

### 8.4  Dateistruktur Test-Code

```
test/load_cell_test/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 // App-Entry: I2C-Init, ISR-Service, load_cell_init()
│   └── serial_cli.c           // UART-Lese-Task, Befehlsparser, Ausgabe
│   └── serial_cli.h
└── sdkconfig.defaults         // UART0 115200, Log-Level INFO
```

Der Test-Code ist eine eigenständige ESP-IDF-Applikation, die nur das
`load_cell`-Component einbindet. Er wird **nicht** in die finale Firmware
kompiliert, sondern lebt in einem separaten `test/`-Verzeichnis.

### 8.5  Testprotokoll

Ein manuelles Testprotokoll zur Inbetriebnahme:

| Schritt | Aktion                                | Erwartetes Ergebnis                              |
| ------- | ------------------------------------- | ------------------------------------------------ |
| 1       | Board einschalten, `status` eingeben  | `Status: OK`, Gain und SPS korrekt               |
| 2       | `stream slow on`                      | Werte erscheinen im 1-s-Takt                     |
| 3       | Wägezelle unbelastet → `tare`         | `Tare done`, Gewicht springt auf ≈ 0 g           |
| 4       | Referenzgewicht auflegen (z.B. 500 g) | Rohwert steigt, Gewicht zeigt noch falschen Wert  |
| 5       | `calibrate 500`                       | `Cal done`, Gewicht zeigt ≈ 500 g                |
| 6       | Gewicht entfernen                     | Gewicht zeigt ≈ 0 g                              |
| 7       | Anderes Gewicht auflegen (z.B. 200 g) | Gewicht zeigt ≈ 200 g (Verifikation)             |
| 8       | Board neu starten, `calinfo`          | Kalibrierfaktor aus NVS geladen                   |
| 9       | `stream on`, Wägezelle antippen       | Spike im Raw-Wert, Median filtert Ausreißer       |

---

## 9  Web-UI-Anbindung (Produktivbetrieb)

### 9.1  Abgrenzung

Im Produktivbetrieb wird das Wägezellen-Modul **nicht** über den Serial Monitor
gesteuert, sondern über eine Web-UI, die auf dem ESP32 gehostet wird.
Das `load_cell`-Component bleibt dabei **unverändert** — die Web-UI nutzt
ausschließlich die öffentliche API (Abschnitt 6.3) und die Events (Abschnitt 6.4).

### 9.2  Vorgesehene Web-UI-Funktionen

| Funktion                | API-Aufruf                          | HTTP-Endpunkt (Vorschlag) |
| ----------------------- | ----------------------------------- | ------------------------- |
| Gewicht live anzeigen   | `load_cell_get_weight_g()`          | `GET /api/loadcell`       |
| Tarieren                | `load_cell_tare()`                  | `POST /api/loadcell/tare` |
| Kalibrieren             | `load_cell_calibrate(weight_g)`     | `POST /api/loadcell/cal`  |
| Kalibrier-Status        | `load_cell_is_calibrated()`         | `GET /api/loadcell/cal`   |
| Live-Stream (WebSocket) | `load_cell_get_weight_g()` @ 10 Hz  | `WS /ws/loadcell`         |

### 9.3  Architektur-Hinweis

```
┌──────────────┐       Events / API        ┌──────────────────┐
│  load_cell   │ ◄──────────────────────── │   HTTP-Server    │
│  Component   │ ──────────────────────►   │   (Web-UI-Modul) │
│  (dieser Spec)│    get_weight_g()        │                  │
└──────────────┘    tare() / calibrate()   └──────────────────┘
                                                    │
                                                    ▼
                                            Browser / Web-UI
```

Die Web-UI-Anbindung wird in einer separaten Spec spezifiziert.
Das `load_cell`-Component hat **keine Abhängigkeit** zum HTTP-Server —
die Kopplung erfolgt ausschließlich über die öffentliche API und Events.

---

## 10  Offene Punkte / TODOs

- [ ] Abgleich mit bestehendem Code aus dem Marlin-ESP32-Projekt
- [ ] Verstärkung (Gain) je nach Wägezellen-Typ anpassen (128× ggf. zu hoch/niedrig)
- [ ] Entscheidung: Braucht das Modul einen Stabilitätsdetektor
      (Gewicht als „stabil" melden, wenn Δ < Schwelle über N Samples)?
- [ ] Maximale Filtergrößen festlegen (RAM-Budget für Ringpuffer)
- [ ] Telemetrie-Format definieren (Rohwert + Gewicht für Debugging / Plot)
- [ ] Web-UI-Modul als separate Spec schreiben (HTTP-Server, WebSocket, Frontend)
