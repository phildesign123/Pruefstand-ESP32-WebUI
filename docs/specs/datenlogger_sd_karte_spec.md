# Modul-Spezifikation: Datenlogger — SD-Karte

> **Version:** 1.0
> **Datum:** 2026-04-07
> **Plattform:** ESP32-WROVER-E · Arduino-Framework + FreeRTOS

---

## 1  Zweck

Dieses Modul zeichnet Messdaten (Temperatur, Gewicht/Kraft, Geschwindigkeit, Fehler-Flags)
als CSV-Datei auf eine SD-Karte auf. Die Aufzeichnung läuft parallel zum
laufenden Prozess und darf diesen nicht beeinflussen.

Die gespeicherten Dateien können über die Web-UI im Browser heruntergeladen
werden, ohne die SD-Karte physisch entnehmen zu müssen.

Zusätzlich stellt das Modul generische Datei-Operationen (`datalog_read_raw_file`,
`datalog_write_raw_file`) bereit, die von anderen Modulen (z.B. Preset-Speicherung)
genutzt werden.

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
- **SPI-Takt:** 4 MHz (`SD_SPI_MHZ` in `config.h`).
- **Dateisystem:** FAT32 über Arduino `SD.begin()`.
- **Mountpoint:** Root-Verzeichnis der SD-Karte (`/`)

### 2.2  SD-Karten-Anforderungen

| Eigenschaft       | Anforderung                    |
| ----------------- | ------------------------------ |
| Format            | FAT32                          |
| Kapazität         | >= 1 GB empfohlen              |
| Geschwindigkeit   | Class 4 oder höher             |
| Dateisystem       | Eine Partition, MBR oder GPT   |

---

## 3  CSV-Format

### 3.1  Dateiname

Mit RTC/NTP-Synchronisation:

```
/messdaten_DD-MM-YYYY_HH-MM-SS_NNN.csv
```

Fallback ohne Uhrzeit:

```
/messdaten_BOOT_NNN.csv
```

`NNN` ist ein fortlaufender Zähler, der in NVS (`Preferences`, Key `datalog/file_cnt`)
gespeichert und bei jeder neuen Datei inkrementiert wird.

Optional kann ein benutzerdefinierter Dateiname über die API übergeben werden.
Falls kein `.csv`-Suffix vorhanden ist, wird es automatisch angehängt.

### 3.2  Spalten-Definition

| Spalte               | Header                   | Typ     | Einheit | Quelle                                       |
| -------------------- | ------------------------ | ------- | ------- | --------------------------------------------- |
| Sequenznummer        | `sample_seq`             | uint32  | —       | Fortlaufender Zähler pro Aufzeichnung         |
| Laufzeit             | `millis_ms`              | uint32  | ms      | `millis()` (Boot-Uptime)                      |
| Kraft                | `kraft_N`                | float   | N       | `load_cell_get_weight_g() * 0.00981` (g → N)  |
| Temperatur           | `temperatur`             | float   | °C      | `hotend_get_temperature()`                     |
| Geschwindigkeit      | `motor_geschwindigkeit`  | float   | mm/s    | `motor_get_current_speed()`                    |
| Fehler-Flags         | `error_flags`            | uint8   | Bitfeld | Siehe 3.3                                      |
| SD-Schreibzeit       | `sd_write_us`            | uint32  | us      | Dauer des letzten SD-Flush (Writer-Task)       |
| Sampler-Zykluszeit   | `loop_duration_us`       | uint32  | us      | Dauer eines Sampler-Durchlaufs                 |

### 3.3  Error-Flags (Bitfeld)

| Bit | Maske | Bedeutung                  |
| --- | ----- | -------------------------- |
| 0   | 0x01  | Hotend-Fault               |
| 1   | 0x02  | Sensor-Fault (MAX31865)    |
| 2   | 0x04  | SD-Karte nicht gemountet   |
| 4   | 0x10  | Puffer-Verlust (Queue war voll, Samples gingen verloren) |

### 3.4  Beispiel-CSV

```csv
sample_seq,millis_ms,kraft_N,temperatur,motor_geschwindigkeit,error_flags,sd_write_us,loop_duration_us
0,12345,0.000,25.30,0.00,0,0,5
1,12445,0.000,48.70,0.00,0,0,4
2,12545,0.098,92.40,0.00,0,3200,5
3,12645,0.196,145.10,3.00,0,0,4
4,12745,0.490,198.60,3.00,0,0,5
```

### 3.5  Preamble (optional)

Vor dem CSV-Header kann ein optionaler Preamble-Text geschrieben werden
(max. 512 Bytes). Dieser wird vom Sequencer genutzt, um Metadaten
(Sequenz-Parameter, Zieltemperatur etc.) in die Datei zu schreiben.
Der Preamble wird über `datalog_set_preamble()` gesetzt und nach dem
Schreiben automatisch gelöscht.

### 3.6  Abtastrate

| Parameter       | Default | config.h-Key              |
| --------------- | ------- | ------------------------- |
| Log-Intervall   | 100 ms  | `DATALOG_INTERVAL_MS`     |
| Min. Intervall  | 10 ms   | —                         |
| Max. Intervall  | 60000 ms| —                         |

Default ist 10 Hz (100 ms). Das Intervall ist zur Laufzeit über
`datalog_set_interval()` änderbar.

Bei 10 Hz und ~80 Byte pro Zeile ergibt sich ca. 2.8 MB pro Stunde —
eine 1 GB SD-Karte fasst damit über 350 Stunden Dauerbetrieb.

---

## 4  Write-Strategie und SPI-Bus-Koordination

### 4.1  Problem: Shared SPI-Bus

Die SD-Karte teilt den VSPI-Bus mit dem MAX31865 (Hotend-PID-Modul).
Das Hotend-PID-Modul liest den Sensor alle ~164 ms — ein SD-Karten-Schreibzugriff
darf diesen Zyklus nicht blockieren.

### 4.2  Lösung: Zwei-Task-Architektur mit FreeRTOS-Queue

Die Messdaten werden **nicht** bei jedem Sample direkt auf die SD-Karte
geschrieben. Stattdessen wird ein zweistufiges Verfahren eingesetzt:

```
Datenquellen (Sensoren, ~5 us Lesezeit)
    |
    v
+-----------------------------+
|  Sampler-Task (Core 1)      |  10 Hz, liest Sensoren
|  Kein SPI-Zugriff!          |
+------------+----------------+
             |
             v  xQueueSend (non-blocking)
+-----------------------------+
|  FreeRTOS Queue             |  200 Einträge (20 s Reserve bei 10 Hz)
+------------+----------------+
             |
             v  xQueueReceive (1 s Timeout)
+-----------------------------+
|  Writer-Task (Core 0)       |  Formatiert CSV, sammelt in RAM-Puffer
|  SPI-Mutex nur beim Flush   |  16 KB Puffer, Flush alle 100 Samples
+-----------------------------+
```

### 4.3  RAM-Puffer und Flush-Strategie

| Parameter              | Default  | config.h-Key                 |
| ---------------------- | -------- | ---------------------------- |
| RAM-Puffergröße        | 16384 B  | `DATALOG_BUFFER_SIZE`        |
| Flush nach N Samples   | 100      | `DATALOG_FLUSH_SAMPLES`      |
| Queue-Kapazität        | 200      | `DATALOG_QUEUE_LEN`          |

Der Writer-Task flusht auf die SD-Karte wenn:
- **100 Samples** im Puffer sind (= alle 10 s bei 10 Hz), ODER
- der Puffer zu **90 %** voll ist (weniger als 150 Bytes frei)

Bei Queue-Überlauf (Sampler schneller als Writer) werden Samples
verworfen und ein Warn-Flag (`error_flags` Bit 4) im nächsten
erfolgreichen Sample gesetzt.

### 4.4  SPI-Mutex-Nutzung

```
Writer-Task:
    1. Samples aus Queue lesen (kein Mutex)
    2. CSV-Zeilen in RAM-Puffer formatieren (kein Mutex)
    3. xSemaphoreTake(spi_mutex, portMAX_DELAY)
    4. fwrite() — alle gepufferten Zeilen auf einmal
    5. flush()
    6. xSemaphoreGive(spi_mutex)
```

Der SPI-Mutex wird nur für die Dauer des tatsächlichen SPI-Transfers
gehalten. Das Formatieren der CSV-Zeilen (snprintf) geschieht **vor**
dem Mutex-Take, um die Sperrzeit zu minimieren.

### 4.5  Retry-Logik bei SD-Schreibfehlern

| Retry | Aktion                          | Wartezeit |
| ----- | ------------------------------- | --------- |
| 1     | Datei schließen + neu öffnen    | 100 ms    |
| 2     | SD komplett remounten           | 100 ms    |
| 3     | Puffer verwerfen, Fehler loggen | —         |

Nach 3 fehlgeschlagenen Retries wird der Puffer verworfen und
`sd_error_flag` gesetzt (Bit 4 im nächsten Sample). Die Statistiken
(`s_sd_write_failures`, `s_sd_retry_success`, `s_sd_retry_failures`,
`s_sd_buffers_lost`, `s_sd_max_write_us`) werden über Serial geloggt.

### 4.6  Timing-Auswirkung auf Hotend-PID

| Szenario                  | SPI-Mutex belegt durch SD | Auswirkung auf PID           |
| ------------------------- | ------------------------- | ---------------------------- |
| Flush: 100 Zeilen (~8 KB) | ca. 3-10 ms bei 4 MHz SPI | PID-Zyklus (~164 ms) nicht betroffen |
| SD-Karte langsam           | bis zu 50 ms              | Immer noch < 30 % des PID-Zyklus    |

---

## 5  Datensammlung (Sampler)

### 5.1  Prinzip

Der Sampler-Task läuft auf Core 1 (Realtime-Core) mit Priorität 3
und fragt mit `vTaskDelayUntil()` alle 100 ms (Default) die aktuellen
Sensorwerte ab. Die Werte werden in ein `SampleData`-Struct gepackt
und non-blocking per `xQueueSend()` an den Writer übergeben.

### 5.2  Sample-Struct

```c
struct SampleData {
    uint32_t sample_seq;             // Fortlaufende Sequenznummer
    uint32_t millis_ms;              // millis() zum Zeitpunkt der Messung
    float    gewicht;                // Kraft in Newton (Gramm * 0.00981)
    float    temperatur;             // Temperatur in °C
    float    motor_geschwindigkeit;  // Geschwindigkeit in mm/s
    uint8_t  error_flags;            // Fehler-Bitfeld (siehe 3.3)
    uint32_t loop_duration_us;       // Dauer des Sampler-Durchlaufs in us
};
```

### 5.3  Datenquellen-Zugriff

| Datenquelle       | API-Aufruf                       | Blockiert? | Bemerkung                     |
| ----------------- | -------------------------------- | ---------- | ----------------------------- |
| Kraft (Gewicht)   | `load_cell_get_weight_g()`       | Nein       | Liest atomare Variable, wird zu Newton umgerechnet |
| Temperatur        | `hotend_get_temperature()`       | Nein       | Liest atomare Variable        |
| Geschwindigkeit   | `motor_get_current_speed()`      | Nein       | Liest atomare Variable        |
| Hotend-Fehler     | `hotend_has_fault()`             | Nein       | Bit 0 in error_flags          |
| Sensor-Fehler     | `sensor_has_fault()`             | Nein       | Bit 1 in error_flags          |
| Zeit              | `millis()` / `micros()`         | Nein       | Hardware-Timer                |

Alle Zugriffe sind nicht-blockierend (~5 us gesamt). Der Sampler hält
keine Mutexe und kann daher kein anderes Modul verzögern.

---

## 6  Dateiverwaltung

### 6.1  Aufzeichnungssteuerung

| Zustand         | Wert | Beschreibung                                          |
| --------------- | ---- | ----------------------------------------------------- |
| `IDLE`          | 0    | Keine Aufzeichnung, bereit                            |
| `RECORDING`     | 1    | Sampler aktiv, Daten werden geschrieben               |
| `PAUSED`        | 2    | Sampler pausiert, Datei bleibt offen                  |
| `ERROR`         | 3    | SD-Karte voll, nicht gemountet oder Schreibfehler     |
| `STOPPING`      | 4    | Übergangs-Zustand: Writer flusht Restdaten            |

Zustandsübergänge:
- `IDLE` → `RECORDING`: `datalog_start()`
- `RECORDING` → `PAUSED`: `datalog_pause()`
- `PAUSED` → `RECORDING`: `datalog_resume()`
- `RECORDING` → `STOPPING` → `IDLE`: `datalog_stop()` (wartet 1.5 s auf Writer-Flush)
- Jeder Zustand → `ERROR`: Bei SD-Fehler

### 6.2  Dateirotation

Wenn eine Datei eine konfigurierbare Maximalgröße erreicht, wird sie
geschlossen und eine neue Datei mit neuem Dateinamen angelegt:

| Parameter           | Default | config.h-Key                |
| ------------------- | ------- | --------------------------- |
| Max. Dateigröße     | 10 MB   | `DATALOG_MAX_FILE_SIZE_MB`  |
| Max. Dateien gesamt | 100     | `DATALOG_MAX_FILES`         |

Die Rotation wird vom Writer-Task durchgeführt. Der neue Dateiname
folgt dem gleichen Schema wie beim Start (Zeitstempel oder Fallback).

### 6.3  Sicheres Schließen

Beim Stoppen der Aufzeichnung (`datalog_stop()`):

1. State auf `STOPPING` setzen (Sampler stoppt sofort)
2. 1.5 Sekunden warten (Writer-Task hat 1 s Queue-Timeout + Flush-Zeit)
3. SPI-Mutex nehmen
4. Restdaten aus RAM-Puffer auf SD schreiben
5. `flush()` aufrufen
6. Datei schließen
7. SPI-Mutex freigeben
8. State auf `IDLE` setzen

### 6.4  SD-Karte mounten/remounten

- `sd_mount()`: Nimmt SPI-Mutex (3 s Timeout), ruft `SD.begin()` auf
- `sd_remount()`: `SD.end()` + 200 ms Pause + `SD.begin()` — für
  Fehlerwiederherstellung bei transienten SD-Problemen
- `datalog_mount_sd()`: Öffentliche API zum manuellen Mounten (z.B. via Web-UI)

---

## 7  Web-UI-Integration

### 7.1  Funktionsumfang

Die Web-UI bietet folgende Funktionen für den Zugriff auf die Log-Dateien:

| Funktion                     | Beschreibung                                           |
| ---------------------------- | ------------------------------------------------------ |
| Dateiliste anzeigen          | Alle CSV-Dateien auf der SD-Karte mit Größe            |
| Einzelne Datei herunterladen | Direkter Download als `.csv` im Browser                |
| Datei löschen                | Einzelne Datei von der SD-Karte entfernen              |
| Alle Dateien löschen         | SD-Karte aufräumen (mit Bestätigung)                   |
| Aufzeichnung starten/stoppen | Steuerung mit optionalem Dateinamen                    |
| SD-Karten-Info               | Freier/Gesamt-Speicher, Mount-Status                   |
| SD-Karte mounten             | Manuelles Mounten über Button                          |

### 7.2  HTTP-Endpunkte

| Methode | Endpunkt                        | Beschreibung                          | Body                               |
| ------- | ------------------------------- | ------------------------------------- | ---------------------------------- |
| GET     | `/api/datalog/status`           | Status, Dateiname, Intervall          | —                                  |
| POST    | `/api/datalog/start`            | Aufzeichnung starten (deferred)       | `{"interval_ms":100, "filename":"..."}` |
| POST    | `/api/datalog/stop`             | Aufzeichnung stoppen                  | —                                  |
| GET     | `/api/datalog/filelist`         | Liste aller CSV-Dateien               | —                                  |
| GET     | `/api/datalog/sdinfo`           | SD-Karten-Speicherinfo                | —                                  |
| GET     | `/api/datalog/files/<name>`     | CSV-Datei herunterladen (chunked)     | —                                  |
| DELETE  | `/api/datalog/files/<name>`     | Einzelne Datei löschen                | —                                  |
| POST    | `/api/datalog/delete_all`       | Alle Dateien löschen                  | `{"confirm":true}`                 |
| POST    | `/api/datalog/mount`            | SD-Karte manuell mounten              | —                                  |

**Hinweis:** `/api/datalog/start` wird über einen **Deferred Worker Task**
(`datalog_start_worker`, Stack 4096, Core 0) ausgeführt. Der HTTP-Handler
löst nur eine `xTaskNotifyGive()` aus und kehrt sofort zurück. Das verhindert,
dass `SD.begin()` / `SD.open()` den async_tcp-Task blockieren und den
Watchdog auslösen.

### 7.3  Response-Formate

**Status:**
```json
{"state":"recording","file":"/messdaten_07-04-2026_14-30-00_001.csv","interval_ms":100}
```

**Filelist:**
```json
{"files":[{"name":"messdaten_07-04-2026_14-30-00_001.csv","size":42300,"date":"N/A","active":true}]}
```

**SD-Info:**
```json
{"total":1073741824,"free":1020000000,"mounted":true}
```

### 7.4  Download-Implementierung

Der Download wird als **Chunked Transfer** über AsyncWebServer implementiert.
`AsyncChunkedResponse` ruft wiederholt `datalog_read_chunk()` auf:

```
Client: GET /api/datalog/files/messdaten_07-04-2026_14-30-00_001.csv
Server:
    AsyncChunkedResponse-Callback:
        1. datalog_read_chunk(name, offset, buf, max, &read)
           → SPI-Mutex nehmen
           → SD.open(), seek(offset), read(buf, max)
           → Datei schließen
           → SPI-Mutex freigeben
        2. Chunk an HTTP-Response senden
        3. Offset += read, wiederholen bis read == 0
    Content-Disposition: attachment; filename="messdaten_..."
    Content-Type: text/csv
```

**Wichtig:** Zwischen den Chunk-Lesevorgängen wird der SPI-Mutex
freigegeben, damit das Hotend-PID-Modul den MAX31865 auslesen kann.

### 7.5  Gleichzeitiger Download und Aufzeichnung

Download und laufende Aufzeichnung können gleichzeitig stattfinden:
- Der Download liest über `datalog_read_chunk()` (eigene Datei-Handle pro Aufruf)
- Der Writer-Task schreibt über `s_log_file` (persistente Handle)
- Beide nutzen den SPI-Mutex — Zugriffe werden serialisiert
- Die FreeRTOS-Queue (200 Einträge = 20 s Reserve) puffert Verzögerungen ab

---

## 8  Software-Architektur

### 8.1  FreeRTOS-Tasks

| Task-Name          | Stack  | Priorität | Core | Funktion                          |
| ------------------ | ------ | --------- | ---- | --------------------------------- |
| `datalog_s`        | 2048 B | 3         | 1    | Sensoren abtasten → Queue         |
| `datalog_w`        | 4096 B | 2         | 0    | Queue → RAM-Puffer → SD-Karte     |
| `dl_start`         | 4096 B | —         | 0    | Deferred Start (für Web-API)      |

Core-Zuordnung: Core 1 = Echtzeit (Sampler, kein SPI-Zugriff),
Core 0 = WiFi-Core (Writer, SD-Zugriffe).

### 8.2  Ablauf

```
                  datalog_s (Core 1, Prio 3)         datalog_w (Core 0, Prio 2)
                  --------------------------         ---------------------------
                  |                                  |
  vTaskDelayUntil |                                  | xQueueReceive
  (100 ms)        |                                  | (1000 ms Timeout)
                  v                                  |
           load_cell_get_weight_g()                  |
           hotend_get_temperature()                  |
           motor_get_current_speed()                 |
           hotend_has_fault() / sensor_has_fault()   |
           micros() → loop_duration_us               |
                  |                                  |
                  v                                  |
           SampleData in Queue ---------> snprintf → RAM-Puffer
           (xQueueSend, non-blocking)                |
                  |                                  v  (100 Samples oder Puffer voll)
                  |                           SPI-Mutex nehmen
                  |                           fwrite(s_buf, s_buf_pos)
                  |                           flush()
                  |                           SPI-Mutex freigeben
                  |                                  |
                  v                                  v
           Nächster Zyklus                    Nächster Flush / Retry
```

### 8.3  Öffentliche API (`datalog.h`)

```c
// Modul initialisieren (SD mounten, Tasks erstellen)
bool datalog_init(SPIClass &spi, SemaphoreHandle_t spi_mutex);

// SD-Karte manuell mounten
bool datalog_mount_sd();

// Aufzeichnung steuern
void datalog_set_preamble(const char *text);   // Optional: Text vor CSV-Header
bool datalog_start(uint32_t interval_ms = 0,   // 0 = Default aus config.h
                   const char *filename = nullptr);  // nullptr = auto-generiert
bool datalog_stop();
bool datalog_pause();
bool datalog_resume();
bool datalog_set_interval(uint32_t interval_ms);  // 10-60000 ms

// Status
DatalogState datalog_get_state();
const char*  datalog_get_filename();

// Dateiverwaltung
int  datalog_list_files(DatalogFileInfo *files, int max_files);
bool datalog_delete_file(const char *name);
bool datalog_delete_all();
bool datalog_get_sd_info(DatalogSDInfo *info);

// Chunked-Read für HTTP-Download (mutex-aware)
bool datalog_read_chunk(const char *name, size_t offset,
                        uint8_t *buf, size_t buf_size, size_t *bytes_read);

// Generische Datei-Operationen (mutex-geschützt, für kleine Dateien)
bool datalog_read_raw_file(const char *path, char *buf, size_t max_len, size_t *out_len);
bool datalog_write_raw_file(const char *path, const char *data, size_t len);
```

**Hinweis:** `datalog_start()` darf **nicht** aus dem async_tcp-Kontext
aufgerufen werden — SPI-Mutex + SD-I/O blockieren den async_tcp-Task
und lösen den Watchdog aus. Die Web-UI nutzt dafür den `dl_start`-Worker-Task.

### 8.4  Datentypen

```c
enum DatalogState : uint8_t {
    DATALOG_IDLE      = 0,
    DATALOG_RECORDING = 1,
    DATALOG_PAUSED    = 2,
    DATALOG_ERROR     = 3,
    DATALOG_STOPPING  = 4,
};

struct DatalogFileInfo {
    char   name[64];
    size_t size_bytes;
    char   date_str[20];   // Aktuell immer "N/A"
};

struct DatalogSDInfo {
    uint64_t total_bytes;
    uint64_t free_bytes;
    bool     mounted;
};
```

### 8.5  Dateistruktur

```
src/datalog/
    datalog.h           // Öffentliche API, Enums, Structs
    datalog.cpp         // Sampler-Task, Writer-Task, Dateiverwaltung, API
src/config.h            // Alle Konfigurationsparameter
```

Die gesamte Implementierung liegt in einer einzigen `.cpp`-Datei.
Es gibt keine separaten Dateien für Sampler, Writer oder Ringpuffer.

---

## 9  Konfiguration (config.h)

| Parameter                | Wert       | Beschreibung                                  |
| ------------------------ | ---------- | --------------------------------------------- |
| `SD_CS`                  | 4          | Chip-Select GPIO                              |
| `SD_SPI_MHZ`             | 4000000    | SPI-Takt (4 MHz)                              |
| `DATALOG_INTERVAL_MS`    | 100        | Default-Abtastintervall (10 Hz)               |
| `DATALOG_BUFFER_SIZE`    | 16384      | RAM-Puffer in Bytes                           |
| `DATALOG_FLUSH_SAMPLES`  | 100        | Flush nach N Samples                          |
| `DATALOG_QUEUE_LEN`      | 200        | Queue-Kapazität (Sampler → Writer)            |
| `DATALOG_MAX_FILE_SIZE_MB` | 10       | Dateirotation bei 10 MB                       |
| `DATALOG_MAX_FILES`      | 100        | Maximale Anzahl CSV-Dateien                   |
| `TASK_STACK_DATALOG_S`   | 2048       | Sampler-Stack in Bytes                        |
| `TASK_STACK_DATALOG_W`   | 4096       | Writer-Stack in Bytes                         |
| `TASK_PRIO_DATALOG_S`    | 3          | Sampler-Priorität                             |
| `TASK_PRIO_DATALOG_W`    | 2          | Writer-Priorität                              |

---

## 10  Prioritäten-Übersicht (Gesamtsystem)

| Priorität | Task                | Modul          | Core | Bemerkung                     |
| --------- | ------------------- | -------------- | ---- | ----------------------------- |
| 6         | `load_cell_task`    | Wägezelle      | 1    | 80 Hz Abtastung               |
| 5         | `hotend_pid_task`   | Hotend-PID     | 1    | ~6 Hz Regelung                |
| 4         | `motor_mgr_task`    | Motor          | 1    | Bewegungskoordination         |
| 4         | `sequencer_task`    | Sequencer      | 1    | Messreihen-Steuerung          |
| 3         | `datalog_s`         | **Datenlogger**| 1    | 10 Hz Sampling                |
| 3         | `ws_push_task`      | Web-UI         | 0    | WebSocket-Push                |
| 2         | `datalog_w`         | **Datenlogger**| 0    | SD-Karte schreiben            |

---

## 11  Abhängigkeiten

| Abhängigkeit           | Richtung    | Beschreibung                                       |
| ---------------------- | ----------- | -------------------------------------------------- |
| **SPIClass (VSPI)**    | nutzt       | Muss vor `datalog_init()` initialisiert sein        |
| **SPI-Mutex**          | nutzt       | Gemeinsamer Mutex mit MAX31865 (Hotend-PID)        |
| **NVS (Preferences)**  | nutzt       | Datei-Counter für Dateinamen-Fallback              |
| **Hotend-Modul**       | liest       | `hotend_get_temperature()`, `hotend_has_fault()`   |
| **Sensor-Modul**       | liest       | `sensor_has_fault()` (MAX31865-Fehlerstatus)       |
| **Wägezellen-Modul**   | liest       | `load_cell_get_weight_g()`                          |
| **Motor-Modul**        | liest       | `motor_get_current_speed()`                         |
| **Arduino SD-Library** | nutzt       | `SD.begin()`, `SD.open()`, Datei-I/O               |
| **AsyncWebServer**     | wird genutzt von | Web-UI ruft Datalog-API auf für Download      |

---

## 12  Bekannte Einschränkungen

- **Kein Pause/Resume in Web-UI:** Die Funktionen existieren in der API, sind aber nicht über HTTP-Endpunkte oder die Web-UI erreichbar.
- **Kein Set-Interval Endpunkt:** `datalog_set_interval()` existiert, aber kein HTTP-Endpunkt dafür.
- **Datei-Datum immer "N/A":** `DatalogFileInfo.date_str` wird aktuell nicht befüllt.
- **Kein Event-System:** Die in der ursprünglichen Spec geplanten ESP-IDF Events (STARTED, STOPPED, FILE_ROTATED etc.) sind nicht implementiert. Stattdessen wird über Serial geloggt.
- **Kein `datalog_deinit()`:** Die Tasks werden beim Init erstellt und laufen dauerhaft.
- **Keine `delete all` über DELETE:** Alle-Löschen geht nur über POST `/api/datalog/delete_all` mit `{"confirm":true}`.
- **Keine Serial-CLI:** Die in der ursprünglichen Spec geplanten Serial-Befehle (start, stop, list, cat, tail etc.) sind nicht implementiert.

---

## 13  Änderungshistorie

| Datum      | Version | Änderung                                                    |
| ---------- | ------- | ----------------------------------------------------------- |
| 2026-03-17 | 0.1     | Entwurf (Spec vor Implementierung)                          |
| 2026-04-07 | 1.0     | Komplett überarbeitet basierend auf aktuellem Code-Stand:    |
|            |         | - Arduino SD-Library statt ESP-IDF VFS                      |
|            |         | - Queue statt Ringpuffer                                    |
|            |         | - 8 CSV-Spalten statt 6 (error_flags, sd_write_us, loop_duration_us, sample_seq) |
|            |         | - Kraft in Newton statt Gewicht in Gramm                    |
|            |         | - Deutsche Spaltenbezeichnungen (kraft_N, temperatur)       |
|            |         | - Dateiname DD-MM-YYYY statt YYYYMMDD                       |
|            |         | - Default 10 Hz statt 1 Hz                                  |
|            |         | - SPI-Takt 4 MHz statt 20 MHz                               |
|            |         | - STOPPING-Zustand hinzugefügt                              |
|            |         | - Retry-Logik bei SD-Schreibfehlern                         |
|            |         | - Preamble-Support (Sequencer-Metadaten)                    |
|            |         | - Deferred Start (Worker-Task für Web-API)                  |
|            |         | - Generische Datei-Ops (read_raw_file, write_raw_file)      |
|            |         | - SD-Mount/Remount-Funktionen                               |
|            |         | - Benutzerdefinierte Dateinamen                             |
|            |         | - Einzelne .cpp statt Komponenten-Architektur               |
|            |         | - Endpunkt-Pfade angepasst (/filelist statt /files)         |
