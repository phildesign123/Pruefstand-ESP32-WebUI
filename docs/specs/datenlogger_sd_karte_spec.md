# Modul-Spezifikation: Datenlogger — SD-Karte

> **Version:** 0.1 (Entwurf)
> **Datum:** 2026-03-17
> **Plattform:** ESP32-WROVER-E · ESP-IDF (FreeRTOS)

---

## 1  Zweck

Dieses Modul zeichnet Messdaten (Temperatur, Gewicht, Geschwindigkeit, Zeit)
als CSV-Datei auf eine SD-Karte auf. Die Aufzeichnung läuft parallel zum
laufenden Prozess und darf diesen nicht beeinflussen.

Die gespeicherten Dateien können über die Web-UI im Browser heruntergeladen
werden, ohne die SD-Karte physisch entnehmen zu müssen.

---

## 2  Hardware-Schnittstellen

### 2.1  SD-Karte (VSPI)

| Signal | GPIO | Richtung | Bemerkung               |
| ------ | ---- | -------- | ----------------------- |
| MOSI   | 23   | OUT      | Shared mit MAX31865     |
| MISO   | 19   | IN       | Shared mit MAX31865     |
| SCLK   | 18   | OUT      | Shared mit MAX31865     |
| CS     | 4    | OUT      | Eigener Chip-Select     |

- **Bus-Sharing:** Der VSPI-Bus wird mit dem MAX31865 (Hotend-Temperatursensor)
  geteilt. Zugriffe werden über den gemeinsamen SPI-Mutex serialisiert.
- **SPI-Takt:** 20 MHz (SD-Karte im SPI-Modus, reduzierbar bei Problemen).
- **Dateisystem:** FAT32 über ESP-IDF `esp_vfs_fat_sdspi_mount()`.
- **Mountpoint:** `/sdcard`

### 2.2  SD-Karten-Anforderungen

| Eigenschaft       | Anforderung                    |
| ----------------- | ------------------------------ |
| Format            | FAT32                          |
| Kapazität         | ≥ 1 GB empfohlen               |
| Geschwindigkeit   | Class 4 oder höher             |
| Dateisystem       | Eine Partition, MBR oder GPT   |

---

## 3  CSV-Format

### 3.1  Dateiname

```
/sdcard/log_YYYYMMDD_HHMMSS.csv
```

Pro Aufzeichnungssession wird eine neue Datei angelegt. Der Zeitstempel
im Dateinamen entspricht dem Startzeitpunkt der Aufzeichnung.

Falls keine Echtzeituhr (RTC) oder NTP-Synchronisation vorhanden ist,
wird ein Fallback-Schema verwendet:

```
/sdcard/log_BOOT_NNNNN.csv
```

Dabei ist `NNNNN` ein fortlaufender Zähler, der im NVS gespeichert wird
und bei jedem Boot inkrementiert wird.

### 3.2  Spalten-Definition

| Spalte         | Header             | Typ     | Einheit | Quelle                       |
| -------------- | ------------------ | ------- | ------- | ---------------------------- |
| Zeitstempel    | `timestamp`        | String  | ISO 8601| RTC / NTP / Boot-Uptime      |
| Laufzeit       | `time_ms`          | uint32  | ms      | `esp_timer_get_time() / 1000`|
| Temperatur     | `temperature_c`    | float   | °C      | `hotend_pid_get_temperature()` |
| Gewicht        | `weight_g`         | float   | g       | `load_cell_get_weight_g()`   |
| Geschwindigkeit| `speed_mm_s`       | float   | mm/s    | Motor-Modul (aktuelle Soll-Geschwindigkeit) |
| Motor aktiv    | `motor_active`     | uint8   | 0/1     | `motor_is_moving()`          |

### 3.3  Beispiel-CSV

```csv
timestamp,time_ms,temperature_c,weight_g,speed_mm_s,motor_active
2026-03-17T14:30:00,0,25.3,0.0,0.0,0
2026-03-17T14:30:01,1000,48.7,0.0,0.0,0
2026-03-17T14:30:02,2000,92.4,0.0,0.0,0
2026-03-17T14:30:03,3000,145.1,0.0,0.0,0
2026-03-17T14:30:04,4000,198.6,0.2,3.0,1
2026-03-17T14:30:05,5000,200.1,12.8,3.0,1
2026-03-17T14:30:06,6000,200.0,24.5,3.0,1
```

### 3.4  Abtastrate

| Parameter       | Default | Kconfig-Key               |
| --------------- | ------- | ------------------------- |
| Log-Intervall   | 1000    | `DATALOG_INTERVAL_MS`     |
| Min. Intervall  | 100     | –                         |
| Max. Intervall  | 60000   | –                         |

Das Log-Intervall ist zur Laufzeit über die API änderbar.
Bei 1 Hz und 6 Spalten ergibt sich ca. 60 Byte pro Zeile —
eine 1 GB SD-Karte fasst damit über 17 Millionen Zeilen (≈ 200 Tage Dauerbetrieb).

---

## 4  Write-Strategie und SPI-Bus-Koordination

### 4.1  Problem: Shared SPI-Bus

Die SD-Karte teilt den VSPI-Bus mit dem MAX31865 (Hotend-PID-Modul).
Das Hotend-PID-Modul liest den Sensor alle 250 ms — ein SD-Karten-Schreibzugriff
darf diesen Zyklus nicht blockieren.

### 4.2  Lösung: Gepuffertes Schreiben

Die Messdaten werden **nicht** bei jedem Sample direkt auf die SD-Karte
geschrieben. Stattdessen wird ein zweistufiges Verfahren eingesetzt:

```
Datenquellen (80 Hz, 4 Hz, ...)
    │
    ▼
┌─────────────────────────┐
│  Ringpuffer im RAM      │  Kapazität: 128 Einträge
│  (lockfrei beschreiben) │
└───────────┬─────────────┘
            │
            ▼  Flush alle N Sekunden oder bei Füllstand > 75 %
┌─────────────────────────┐
│  SD-Karten-Schreibtask  │  SPI-Mutex nehmen → fwrite() → Mutex freigeben
│  (niedrige Priorität)   │
└─────────────────────────┘
```

### 4.3  Ringpuffer

| Parameter              | Default | Kconfig-Key                  |
| ---------------------- | ------- | ---------------------------- |
| Puffergröße (Einträge) | 128     | `DATALOG_BUFFER_SIZE`        |
| Flush-Schwelle         | 75 %    | `DATALOG_FLUSH_THRESHOLD`    |
| Flush-Intervall        | 5 s     | `DATALOG_FLUSH_INTERVAL_S`   |

- Der Ringpuffer wird aus dem Sampler-Task beschrieben (Producer)
  und vom Schreib-Task gelesen (Consumer).
- Synchronisation über eine FreeRTOS-Queue oder einen lockfreien
  Ringpuffer mit atomaren Indizes.
- Bei Pufferüberlauf (SD-Karte zu langsam) werden die ältesten
  Einträge überschrieben und ein Warn-Event gepostet.

### 4.4  SPI-Mutex-Nutzung

```
Schreib-Task:
    1. Daten aus Ringpuffer in lokalen Buffer kopieren
    2. xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100))
    3. fwrite() — alle gepufferten Zeilen auf einmal
    4. fflush() / fsync()
    5. xSemaphoreGive(spi_mutex)
```

Der SPI-Mutex wird nur für die Dauer des tatsächlichen SPI-Transfers
gehalten. Das Formatieren der CSV-Zeilen (sprintf) geschieht **vor**
dem Mutex-Take, um die Sperrzeit zu minimieren.

### 4.5  Timing-Auswirkung auf Hotend-PID

| Szenario                | SPI-Mutex belegt durch SD | Auswirkung auf PID          |
| ----------------------- | ------------------------- | --------------------------- |
| Flush: 10 Zeilen à 60 B| ≈ 3 ms bei 20 MHz SPI     | PID-Zyklus (250 ms) nicht betroffen |
| Flush: 128 Zeilen       | ≈ 10 ms                   | Immer noch < 5 % des PID-Zyklus    |
| SD-Karte nicht bereit   | Timeout 100 ms            | PID verzögert sich einmalig um 100 ms — akzeptabel |

Im Worst Case (100 ms Timeout) verliert das PID-Modul einen
Regelzyklus. Dies ist thermisch unkritisch, da die thermische
Trägheit des Hotends im Sekundenbereich liegt.

---

## 5  Datensammlung (Sampler)

### 5.1  Prinzip

Der Sampler-Task fragt in seinem Zyklus (Default: 1 Hz) die aktuellen
Werte von den anderen Modulen ab und schreibt einen Datensatz in den Ringpuffer.

### 5.2  Datenquellen-Zugriff

| Datenquelle       | API-Aufruf                       | Blockiert? | Bemerkung                     |
| ------------------ | -------------------------------- | ---------- | ----------------------------- |
| Temperatur         | `hotend_pid_get_temperature()`   | Nein       | Liest atomare Variable        |
| Gewicht            | `load_cell_get_weight_g()`       | Nein       | Liest atomare Variable        |
| Geschwindigkeit    | `motor_get_current_speed()`      | Nein       | Liest atomare Variable        |
| Motor aktiv        | `motor_is_moving()`              | Nein       | Liest atomaren Status         |
| Zeit               | `esp_timer_get_time()`           | Nein       | Hardware-Timer                |
| Zeitstempel        | `time()` / SNTP                  | Nein       | Systemzeit                    |

Alle Zugriffe sind nicht-blockierend. Der Sampler hält keine Mutexe
und kann daher kein anderes Modul verzögern.

### 5.3  Zeitstempel-Quelle

Die Genauigkeit des Zeitstempels hängt von der verfügbaren Zeitquelle ab:

| Quelle       | Genauigkeit | Konfiguration                        |
| ------------ | ----------- | ------------------------------------ |
| NTP (SNTP)   | ± 50 ms     | Wi-Fi muss verbunden sein            |
| Externe RTC  | ± 2 s/Tag   | Nicht in Pin-Belegung vorgesehen     |
| Boot-Uptime  | ± 0 ms      | Fallback, kein Bezug zur Wanduhrzeit |

Empfehlung: SNTP beim Boot synchronisieren (der ESP32 hat ohnehin
Wi-Fi aktiv für die Web-UI). Die `time_ms`-Spalte liefert unabhängig
davon immer die präzise Laufzeit seit Aufzeichnungsbeginn.

---

## 6  Dateiverwaltung

### 6.1  Aufzeichnungssteuerung

| Zustand         | Beschreibung                                          |
| --------------- | ----------------------------------------------------- |
| `IDLE`          | Keine Aufzeichnung, SD-Karte gemountet                |
| `RECORDING`     | Sampler aktiv, Daten werden geschrieben               |
| `PAUSED`        | Sampler pausiert, Datei bleibt offen                  |
| `ERROR`         | SD-Karte voll, nicht gemountet oder Schreibfehler     |

Zustandsübergänge erfolgen ausschließlich über die API.

### 6.2  Dateirotation

Wenn eine Datei eine konfigurierbare Maximalgröße erreicht, wird sie
geschlossen und eine neue Datei mit fortlaufender Nummer angelegt:

```
/sdcard/log_20260317_143000.csv       ← Original
/sdcard/log_20260317_143000_001.csv   ← Rotation 1
/sdcard/log_20260317_143000_002.csv   ← Rotation 2
```

| Parameter           | Default | Kconfig-Key                 |
| ------------------- | ------- | --------------------------- |
| Max. Dateigröße     | 10 MB   | `DATALOG_MAX_FILE_SIZE_MB`  |
| Max. Dateien gesamt | 100     | `DATALOG_MAX_FILES`         |

Bei Erreichen von `DATALOG_MAX_FILES` wird die älteste Datei gelöscht
(Ringpuffer-Prinzip auf Dateiebene).

### 6.3  Sicheres Schließen

Beim Stoppen der Aufzeichnung oder vor dem Herunterfahren:

1. Restliche Daten aus dem Ringpuffer flushen.
2. `fflush()` + `fsync()` aufrufen.
3. Datei schließen (`fclose()`).
4. SD-Karte unmounten (`esp_vfs_fat_sdcard_unmount()`).

**Stromausfall-Schutz:** Da nach jedem Flush ein `fsync()` erfolgt,
gehen maximal die Daten eines Flush-Intervalls (Default: 5 s) verloren.

---

## 7  Web-UI-Download

### 7.1  Funktionsumfang

Die Web-UI bietet folgende Funktionen für den Zugriff auf die Log-Dateien:

| Funktion                  | Beschreibung                                        |
| ------------------------- | --------------------------------------------------- |
| Dateiliste anzeigen       | Alle CSV-Dateien auf der SD-Karte mit Größe und Datum |
| Einzelne Datei herunterladen | Direkter Download als `.csv` im Browser            |
| Datei löschen             | Einzelne Datei von der SD-Karte entfernen           |
| Alle Dateien löschen      | SD-Karte aufräumen (mit Bestätigung)                |
| Aufzeichnung starten/stoppen | Live-Steuerung der Datenaufzeichnung             |
| Log-Intervall ändern      | Abtastrate zur Laufzeit anpassen                    |
| SD-Karten-Info            | Freier Speicher, Dateisystem-Status                 |

### 7.2  HTTP-Endpunkte

| Methode | Endpunkt                      | Beschreibung                          | Body / Query                |
| ------- | ----------------------------- | ------------------------------------- | --------------------------- |
| GET     | `/api/datalog/status`         | Aufzeichnungsstatus, SD-Info          | —                           |
| POST    | `/api/datalog/start`          | Aufzeichnung starten                  | `{"interval_ms": 1000}`     |
| POST    | `/api/datalog/stop`           | Aufzeichnung stoppen                  | —                           |
| POST    | `/api/datalog/pause`          | Aufzeichnung pausieren                | —                           |
| POST    | `/api/datalog/resume`         | Aufzeichnung fortsetzen               | —                           |
| GET     | `/api/datalog/files`          | Liste aller CSV-Dateien               | —                           |
| GET     | `/api/datalog/files/<name>`   | CSV-Datei herunterladen               | —                           |
| DELETE  | `/api/datalog/files/<name>`   | Einzelne Datei löschen                | —                           |
| DELETE  | `/api/datalog/files`          | Alle Dateien löschen                  | `{"confirm": true}`         |
| GET     | `/api/datalog/sdinfo`         | SD-Karten-Info (frei/belegt/total)    | —                           |

### 7.3  Download-Implementierung

Der Download wird als **Chunked Transfer** über den ESP-IDF HTTP-Server
implementiert. Die Datei wird blockweise gelesen und gesendet, damit
nicht die gesamte Datei im RAM gehalten werden muss:

```
Client: GET /api/datalog/files/log_20260317_143000.csv
Server:
    1. SPI-Mutex nehmen
    2. Datei öffnen (fopen)
    3. SPI-Mutex freigeben
    4. Loop:
       a. SPI-Mutex nehmen
       b. Chunk lesen (4 KB)
       c. SPI-Mutex freigeben
       d. Chunk an HTTP-Response senden
    5. Datei schließen
    6. Content-Disposition Header: attachment; filename="log_20260317_143000.csv"
```

**Wichtig:** Zwischen den Chunk-Lesevorgängen wird der SPI-Mutex
freigegeben, damit das Hotend-PID-Modul den MAX31865 auslesen kann.
Ein Download blockiert die Temperaturregelung nicht.

### 7.4  Gleichzeitiger Download und Aufzeichnung

Download und laufende Aufzeichnung können gleichzeitig stattfinden:

- Der Download liest eine abgeschlossene oder die aktuelle Datei.
- Der Schreib-Task schreibt weiterhin über den Ringpuffer.
- Beide nutzen den SPI-Mutex — die Zugriffe werden serialisiert.
- Der Download kann die Aufzeichnung um wenige Millisekunden verzögern
  (umgekehrt ebenso), aber dies wird durch den Ringpuffer abgefangen.

---

## 8  Software-Architektur

### 8.1  FreeRTOS-Tasks

| Task-Name          | Stack  | Priorität | Funktion                             |
| ------------------ | ------ | --------- | ------------------------------------ |
| `datalog_sample`   | 2048 B | 3         | Datenquellen abtasten, in Ringpuffer |
| `datalog_writer`   | 4096 B | 2         | Ringpuffer → SD-Karte schreiben      |

Beide Tasks haben niedrige Priorität, da sie die zeitkritischen
Module (Wägezelle Prio 6, PID Prio 5, Motor Prio 4) nicht stören dürfen.

### 8.2  Ablauf

```
                  datalog_sample (Prio 3)          datalog_writer (Prio 2)
                  ─────────────────────            ────────────────────────
                  │                                │
  vTaskDelayUntil │                                │ Wartet auf:
  (1000 ms)       │                                │  - Flush-Intervall (5 s)
                  ▼                                │  - ODER Füllstand > 75 %
           Temperatur lesen                        │
           Gewicht lesen                           │
           Geschwindigkeit lesen                   │
           Zeitstempel erzeugen                    │
                  │                                │
                  ▼                                │
           CSV-Zeile formatieren                   │
                  │                                │
                  ▼                                │
           In Ringpuffer schreiben ──────────────► Ringpuffer lesen
                  │                                │
                  │                                ▼
                  │                         SPI-Mutex nehmen
                  │                         fwrite() (Batch)
                  │                         fflush() + fsync()
                  │                         SPI-Mutex freigeben
                  │                                │
                  ▼                                ▼
           Nächster Zyklus                  Nächster Flush
```

### 8.3  Öffentliche API

```c
/**
 * @brief Modul initialisieren (SD-Karte mounten, Tasks erstellen)
 * @param spi_host  VSPI_HOST — muss bereits initialisiert sein
 * @param spi_mutex Gemeinsamer SPI-Bus-Mutex
 * @return ESP_OK bei Erfolg, ESP_ERR_NOT_FOUND wenn keine SD-Karte
 */
esp_err_t datalog_init(spi_host_device_t spi_host,
                       SemaphoreHandle_t spi_mutex);

/* ── Aufzeichnung ────────────────────────────────────── */

/**
 * @brief Aufzeichnung starten (neue CSV-Datei anlegen)
 * @param interval_ms  Log-Intervall in ms (0 = Default aus Kconfig)
 */
esp_err_t datalog_start(uint32_t interval_ms);

/**
 * @brief Aufzeichnung stoppen (Datei sauber schließen)
 */
esp_err_t datalog_stop(void);

/**
 * @brief Aufzeichnung pausieren (Datei bleibt offen)
 */
esp_err_t datalog_pause(void);

/**
 * @brief Aufzeichnung fortsetzen
 */
esp_err_t datalog_resume(void);

/**
 * @brief Log-Intervall zur Laufzeit ändern
 */
esp_err_t datalog_set_interval(uint32_t interval_ms);

/**
 * @brief Aufzeichnungsstatus abfragen
 */
datalog_state_t datalog_get_state(void);

/* ── Dateizugriff ────────────────────────────────────── */

/**
 * @brief Liste aller CSV-Dateien auf der SD-Karte
 * @param[out] files     Array von Dateiinfo-Strukturen
 * @param[in]  max_files Maximale Anzahl Einträge
 * @param[out] count     Tatsächliche Anzahl
 */
esp_err_t datalog_list_files(datalog_file_info_t *files,
                             size_t max_files, size_t *count);

/**
 * @brief Datei blockweise lesen (für HTTP-Chunked-Download)
 * @param filename  Dateiname (ohne Pfad)
 * @param offset    Byte-Offset in der Datei
 * @param buffer    Zielpuffer
 * @param buf_size  Puffergröße
 * @param[out] bytes_read  Tatsächlich gelesene Bytes (0 = EOF)
 */
esp_err_t datalog_read_file_chunk(const char *filename,
                                  size_t offset, void *buffer,
                                  size_t buf_size, size_t *bytes_read);

/**
 * @brief Einzelne Datei löschen
 */
esp_err_t datalog_delete_file(const char *filename);

/**
 * @brief Alle Log-Dateien löschen
 */
esp_err_t datalog_delete_all(void);

/* ── SD-Karten-Info ──────────────────────────────────── */

/**
 * @brief SD-Karten-Speicherinfo abfragen
 */
esp_err_t datalog_get_sd_info(datalog_sd_info_t *info);

/* ── Lifecycle ───────────────────────────────────────── */

/**
 * @brief Aufzeichnung stoppen, SD-Karte unmounten, Ressourcen freigeben
 */
esp_err_t datalog_deinit(void);
```

### 8.4  Datentypen

```c
typedef enum {
    DATALOG_STATE_IDLE,
    DATALOG_STATE_RECORDING,
    DATALOG_STATE_PAUSED,
    DATALOG_STATE_ERROR,
} datalog_state_t;

typedef struct {
    char filename[64];
    size_t size_bytes;
    time_t created;
} datalog_file_info_t;

typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    bool mounted;
} datalog_sd_info_t;

typedef struct {
    char timestamp[24];    // ISO 8601
    uint32_t time_ms;      // Laufzeit seit Recording-Start
    float temperature_c;
    float weight_g;
    float speed_mm_s;
    uint8_t motor_active;
} datalog_sample_t;
```

### 8.5  Events (ESP-IDF Event Loop)

| Event-ID                          | Payload              | Beschreibung                        |
| --------------------------------- | -------------------- | ----------------------------------- |
| `DATALOG_EVENT_STARTED`          | `char[] filename`    | Neue Aufzeichnung begonnen           |
| `DATALOG_EVENT_STOPPED`          | `char[] filename`    | Aufzeichnung beendet                 |
| `DATALOG_EVENT_FILE_ROTATED`    | `char[] filename`    | Neue Datei durch Rotation            |
| `DATALOG_EVENT_SD_FULL`         | —                    | SD-Karte voll, älteste Datei gelöscht|
| `DATALOG_EVENT_SD_ERROR`        | `esp_err_t err`      | SD-Karten-Fehler                     |
| `DATALOG_EVENT_BUFFER_OVERFLOW` | `uint32_t lost`      | Ringpuffer übergelaufen, Samples verloren |

### 8.6  Dateistruktur

```
components/datalog/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── datalog.h              // Öffentliche API, Events, Typen
└── src/
    ├── datalog.c               // Aufzeichnungssteuerung, Dateiverwaltung
    ├── datalog_sampler.c       // Sampler-Task, Datenquellen abtasten
    ├── datalog_writer.c        // Writer-Task, Ringpuffer → SD-Karte
    ├── datalog_ringbuf.c       // Ringpuffer-Implementierung
    ├── datalog_ringbuf.h
    ├── sd_card.c               // SD-Karten-Init, Mount/Unmount
    └── sd_card.h
```

---

## 9  Test-Modus (Serial Monitor)

### 9.1  Befehle (Serial CLI)

| Befehl                        | Beschreibung                                           |
| ----------------------------- | ------------------------------------------------------ |
| `status`                      | SD-Karten-Status, Aufzeichnungsstatus, freier Speicher |
| `start [interval_ms]`         | Aufzeichnung starten, z.B. `start 500`                 |
| `stop`                        | Aufzeichnung stoppen                                   |
| `pause` / `resume`            | Aufzeichnung pausieren / fortsetzen                    |
| `list`                        | Alle CSV-Dateien auflisten                             |
| `cat <filename> [lines]`      | Erste N Zeilen einer Datei ausgeben (Default: 20)      |
| `tail <filename> [lines]`     | Letzte N Zeilen einer Datei ausgeben (Default: 20)     |
| `delete <filename>`           | Einzelne Datei löschen                                 |
| `delete all`                  | Alle Dateien löschen (mit Bestätigung)                 |
| `sdinfo`                      | SD-Karten-Speicher anzeigen                            |
| `help`                        | Alle verfügbaren Befehle auflisten                     |

### 9.2  Ausgabeformat

#### Status

```
[DATALOG] Status: RECORDING
  File:     log_20260317_143000.csv
  Size:     42.3 KB (704 rows)
  Interval: 1000 ms
  Buffer:   12/128 (9%)
  SD Card:  1.84 GB free / 1.86 GB total
```

#### Dateiliste

```
[DATALOG] Files on SD card:
  1. log_20260317_130000.csv     1.2 MB   2026-03-17 13:00
  2. log_20260317_140000.csv     856 KB   2026-03-17 14:00
  3. log_20260317_143000.csv     42 KB    2026-03-17 14:30  ← ACTIVE
  Total: 3 files, 2.1 MB used
```

#### Cat

```
[DATALOG] Head of log_20260317_143000.csv (5 lines):
timestamp,time_ms,temperature_c,weight_g,speed_mm_s,motor_active
2026-03-17T14:30:00,0,25.3,0.0,0.0,0
2026-03-17T14:30:01,1000,48.7,0.0,0.0,0
2026-03-17T14:30:02,2000,92.4,0.0,0.0,0
2026-03-17T14:30:03,3000,145.1,0.0,0.0,0
```

### 9.3  Dateistruktur Test-Code

```
test/datalog_test/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 // SPI-Init, Mock-Datenquellen, datalog_init()
│   └── serial_cli.c
│   └── serial_cli.h
└── sdkconfig.defaults
```

Da der Test ohne die anderen Module (Hotend, Wägezelle, Motor) laufen soll,
werden Mock-Funktionen bereitgestellt, die simulierte Werte liefern
(z.B. linear steigende Temperatur, Sinus-Gewicht).

### 9.4  Testprotokoll

| Schritt | Aktion                              | Erwartetes Ergebnis                              |
| ------- | ----------------------------------- | ------------------------------------------------ |
| 1       | Board starten, `sdinfo`             | SD-Karte erkannt, Kapazität korrekt              |
| 2       | `start`                             | Aufzeichnung läuft, Dateiname angezeigt          |
| 3       | 10 s warten, `status`               | ≈ 10 Zeilen geschrieben, Puffer niedrig          |
| 4       | `pause`, 5 s warten, `resume`       | Lücke in Zeitstempeln, Aufzeichnung läuft weiter |
| 5       | `stop`                              | Datei sauber geschlossen                         |
| 6       | `list`                              | Datei mit korrekter Größe aufgelistet            |
| 7       | `cat <filename> 5`                  | Header + 4 Datenzeilen, CSV korrekt formatiert   |
| 8       | `tail <filename> 3`                 | Letzte 3 Zeilen der Datei                        |
| 9       | `start 100` (schnelles Logging)     | 10 Hz Aufzeichnung, Puffer füllt sich schneller  |
| 10      | `stop`, `delete <filename>`         | Datei gelöscht, `list` zeigt sie nicht mehr       |
| 11      | Board neu starten, `list`           | Dateien aus vorherigen Runs noch vorhanden        |

---

## 10  Prioritäten-Übersicht (Gesamtsystem, aktualisiert)

| Priorität | Task / ISR            | Modul          | Bemerkung                     |
| --------- | --------------------- | -------------- | ----------------------------- |
| 7         | RMT ISR / Callbacks   | Motor          | Hardware-ISR                  |
| 6         | `load_cell_task`      | Wägezelle      | 80 Hz Abtastung               |
| 5         | `hotend_pid_task`     | Hotend-PID     | 4 Hz Regelung                 |
| 4         | `motor_mgr_task`      | Motor          | Bewegungskoordination         |
| 3         | `datalog_sample`      | **Datenlogger**| 1 Hz Sampling                 |
| 3         | HTTP-Server           | Web-UI         | Benutzerinteraktion           |
| 2         | `datalog_writer`      | **Datenlogger**| SD-Karte schreiben            |

---

## 11  Abhängigkeiten

| Abhängigkeit           | Richtung    | Beschreibung                                       |
| ---------------------- | ----------- | -------------------------------------------------- |
| **SPI-Bus (VSPI)**     | nutzt       | Bus muss von übergeordnetem Modul initialisiert werden |
| **SPI-Mutex**          | nutzt       | Gemeinsamer Mutex mit MAX31865 (Hotend-PID)        |
| **NVS**                | nutzt       | Boot-Counter für Dateinamen-Fallback               |
| **Hotend-PID-Modul**   | liest       | `hotend_pid_get_temperature()` — nicht-blockierend |
| **Wägezellen-Modul**   | liest       | `load_cell_get_weight_g()` — nicht-blockierend     |
| **Motor-Modul**        | liest       | `motor_get_current_speed()`, `motor_is_moving()`   |
| **ESP Event Loop**     | publiziert  | Events für Start/Stop, Fehler                       |
| **HTTP-Server**        | wird genutzt von | Web-UI ruft Datalog-API auf für Download      |
| **SNTP (optional)**    | nutzt       | Für korrekte Zeitstempel                            |

---

## 12  Offene Punkte / TODOs

- [ ] Abgleich mit bestehendem Code aus dem Marlin-ESP32-Projekt
- [ ] Klären: Sollen zusätzliche Spalten konfigurierbar sein (z.B. Duty-Cycle, Rohwerte)?
- [ ] SNTP-Konfiguration (NTP-Server, Timezone) in die Web-UI-Spec aufnehmen
- [ ] Download-Performance testen: Wie schnell kann eine 10 MB Datei über HTTP gestreamt werden?
- [ ] Klären: Braucht es ein binäres Format (z.B. CBOR) für höhere Abtastraten?
- [ ] Web-UI-Frontend für Dateiliste und Download als separate Spec schreiben
- [ ] Entscheidung: Soll die Web-UI auch einen Live-Graph der aktuellen Werte zeigen (WebSocket)?
