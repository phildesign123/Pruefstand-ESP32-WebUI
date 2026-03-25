#include "datalog.h"
#include "../config.h"
#include "../hotend/hotend.h"
#include "../hotend/sensor.h"
#include "../load_cell/load_cell.h"
#include "../motor/motor.h"
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// =============================================================
// Datenlogger: Zwei Tasks auf getrennten Cores
//   Sampler (Core 1, Prio 3): Sensoren lesen → Queue
//   Writer  (Core 0, Prio 2): Queue → RAM-Puffer → SD-Karte
// SD-Writes blockieren den Mess-Takt NICHT mehr.
// =============================================================

// ── Sample-Struct für die Queue ──────────────────────────────

struct SampleData {
    uint32_t sample_seq;
    uint32_t millis_ms;
    float    gewicht;
    float    temperatur;
    float    motor_geschwindigkeit;
    uint8_t  error_flags;
    uint32_t loop_duration_us;
};

// ── CSV-Header ───────────────────────────────────────────────

static const char CSV_HEADER[] =
    "sample_seq,millis_ms,kraft_N,temperatur,motor_geschwindigkeit,"
    "error_flags,sd_write_us,loop_duration_us";

// ── Status-Variablen ─────────────────────────────────────────

static SPIClass         *s_spi          = nullptr;
static SemaphoreHandle_t s_spi_mutex    = nullptr;
static volatile DatalogState s_state    = DATALOG_IDLE;
static volatile uint32_t s_interval_ms  = DATALOG_INTERVAL_MS;
static char              s_filename[64] = {};
static char              s_preamble[512] = {};
static uint32_t          s_start_ms     = 0;
static Preferences       s_prefs;
static volatile uint32_t s_sample_seq   = 0;

// Queue: Sampler → Writer
static QueueHandle_t     s_sample_queue = nullptr;
#define SAMPLE_QUEUE_LEN  100   // 10 Sekunden Puffer bei 10 Hz

// ── Dateiname generieren ─────────────────────────────────────

static void make_filename(char *buf, size_t len) {
    struct tm ti;
    // Persistenten Zähler hochzählen
    s_prefs.begin("datalog", false);
    uint32_t cnt = s_prefs.getUInt("file_cnt", 0) + 1;
    s_prefs.putUInt("file_cnt", cnt);
    s_prefs.end();

    if (getLocalTime(&ti)) {
        snprintf(buf, len, "/messdaten_%02d-%02d-%04d_%02d-%02d-%02d_%03lu.csv",
                 ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900,
                 ti.tm_hour, ti.tm_min, ti.tm_sec, (unsigned long)cnt);
    } else {
        snprintf(buf, len, "/messdaten_BOOT_%03lu.csv", (unsigned long)cnt);
    }
}

// ── SD-Karte initialisieren ─────────────────────────────────

static volatile bool sd_mounted = false;

static bool sd_mount() {
    if (sd_mounted) return true;
    Serial.println("[DATALOG] SD-Karte wird gesucht...");
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(3000));
    bool ok = SD.begin(SD_CS, *s_spi, 400000);
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    sd_mounted = ok;
    if (!ok) Serial.println("[DATALOG] SD-Karte nicht gefunden!");
    else     Serial.printf("[DATALOG] SD-Karte: %lluMB\n", SD.totalBytes() / (1024 * 1024));
    return ok;
}

static bool sd_remount() {
    Serial.println("[DATALOG] SD-Remount...");
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(3000));
    SD.end();
    vTaskDelay(pdMS_TO_TICKS(200));
    bool ok = SD.begin(SD_CS, *s_spi, 400000);
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    sd_mounted = ok;
    Serial.printf("[DATALOG] SD-Remount: %s\n", ok ? "OK" : "FEHLER");
    return ok;
}

// ── Sampler-Task (Core 1, Prio HOCH) ─────────────────────────
// Liest Sensoren bei 10 Hz, schreibt in Queue.
// Greift NICHT auf die SD-Karte zu.

static void sampler_task(void *arg) {
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_interval_ms));

        if (s_state != DATALOG_RECORDING) continue;

        uint32_t loop_start = micros();

        // Sensoren lesen (alles gecachte Werte, ~5 µs)
        float gewicht = load_cell_get_weight_g() * 0.00981f;  // Gramm → Newton
        float temp    = hotend_get_temperature();
        float speed   = motor_get_current_speed();

        uint8_t flags = 0;
        if (hotend_has_fault())  flags |= 0x01;
        if (sensor_has_fault())  flags |= 0x02;
        if (!sd_mounted)         flags |= 0x04;

        uint32_t loop_us = micros() - loop_start;

        SampleData sample = {
            .sample_seq          = s_sample_seq++,
            .millis_ms           = millis(),
            .gewicht             = gewicht,
            .temperatur          = temp,
            .motor_geschwindigkeit = speed,
            .error_flags         = flags,
            .loop_duration_us    = loop_us,
        };

        // Non-blocking in Queue schreiben
        if (xQueueSend(s_sample_queue, &sample, 0) != pdTRUE) {
            // Queue voll — Writer kommt nicht hinterher
            // Nächstes Sample bekommt das Flag
        }
    }
}

// ── Writer-Task (Core 0, Prio NIEDRIG) ───────────────────────
// Liest Samples aus Queue, sammelt in RAM-Puffer, schreibt auf SD.
// Kann beliebig lange blockieren — stört den Sampler nicht.

static File   s_log_file;
static char   s_buf[DATALOG_BUFFER_SIZE];
static size_t s_buf_pos = 0;

static void writer_task(void *arg) {
    SampleData sample;
    int      lines_in_buf   = 0;
    uint32_t last_sd_write_us = 0;
    size_t   bytes_since_rot  = 0;

    for (;;) {
        // Auf Sample warten (blockiert bis Daten da)
        if (xQueueReceive(s_sample_queue, &sample, pdMS_TO_TICKS(1000)) != pdTRUE) {
            // Timeout — kein Sample bekommen (nicht recording oder idle)
            // Restpuffer schreiben wenn Aufzeichnung gestoppt
            if (s_state != DATALOG_RECORDING && s_buf_pos > 0) {
                if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(500));
                if (!s_log_file) s_log_file = SD.open(s_filename, FILE_APPEND);
                if (s_log_file) {
                    s_log_file.write((const uint8_t*)s_buf, s_buf_pos);
                    s_log_file.flush();
                    s_log_file.close();
                    s_buf_pos = 0;
                }
                if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
            }
            continue;
        }

        // CSV-Zeile in RAM-Puffer formatieren
        char line[150];
        int len = snprintf(line, sizeof(line),
                 "%lu,%lu,%.3f,%.2f,%.2f,%u,%lu,%lu\n",
                 (unsigned long)sample.sample_seq,
                 (unsigned long)sample.millis_ms,
                 sample.gewicht, sample.temperatur,
                 sample.motor_geschwindigkeit,
                 (unsigned)sample.error_flags,
                 (unsigned long)last_sd_write_us,
                 (unsigned long)sample.loop_duration_us);

        if (len > 0 && s_buf_pos + len < sizeof(s_buf)) {
            memcpy(s_buf + s_buf_pos, line, len);
            s_buf_pos += len;
            bytes_since_rot += len;
        }
        lines_in_buf++;
        last_sd_write_us = 0;

        // Alle N Samples oder bei fast vollem Puffer: auf SD schreiben
        bool need_flush = (lines_in_buf >= DATALOG_FLUSH_SAMPLES)
                       || (s_buf_pos + 150 > sizeof(s_buf));

        if (need_flush && sd_mounted) {
            if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);

            bool write_ok = false;
            for (int retry = 0; retry < 3 && !write_ok; retry++) {
                if (retry > 0) {
                    // Bei Retry: SD neu initialisieren
                    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
                    sd_remount();
                    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
                    if (!sd_mounted) break;
                }
                s_log_file = SD.open(s_filename, FILE_APPEND);
                if (s_log_file) {
                    size_t written = s_log_file.write((const uint8_t*)s_buf, s_buf_pos);
                    s_log_file.flush();
                    s_log_file.close();
                    if (written == s_buf_pos) {
                        write_ok = true;
                        s_buf_pos = 0;
                    }
                }
                if (!write_ok) {
                    Serial.printf("[DATALOG] SD-Write Retry %d (remount)\n", retry + 1);
                }
            }
            if (!write_ok) {
                Serial.println("[DATALOG] SD-Write fehlgeschlagen, versuche nächsten Flush.");
            }

            if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
            lines_in_buf = 0;
        }

        // Datei-Rotation prüfen
        if (bytes_since_rot >= (size_t)DATALOG_MAX_FILE_SIZE_MB * 1024 * 1024) {
            if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(500));
            char new_name[72];
            make_filename(new_name, sizeof(new_name));
            strncpy(s_filename, new_name, sizeof(s_filename));
            s_log_file = SD.open(s_filename, FILE_WRITE);
            if (s_log_file) { s_log_file.println(CSV_HEADER); s_log_file.close(); }
            if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
            bytes_since_rot = 0;
        }
    }
}

// ── Öffentliche API ──────────────────────────────────────────

bool datalog_init(SPIClass &spi, SemaphoreHandle_t spi_mutex) {
    s_spi       = &spi;
    s_spi_mutex = spi_mutex;

    s_sample_queue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(SampleData));

    // SD-Karte synchron mounten (verhindert Race-Conditions mit WebUI)
    sd_mount();

    // Sampler: Core 1 (Realtime), hohe Priorität
    xTaskCreatePinnedToCore(sampler_task, "datalog_s", TASK_STACK_DATALOG_S,
                            nullptr, TASK_PRIO_DATALOG_S, nullptr, CORE_REALTIME);

    // Writer: Core 0, niedrige Priorität
    // SD-Writes blockieren nie den Sampler auf Core 1.
    xTaskCreatePinnedToCore(writer_task, "datalog_w", TASK_STACK_DATALOG_W,
                            nullptr, TASK_PRIO_DATALOG_W, nullptr, CORE_WIFI);

    Serial.println("[DATALOG] Initialisiert (Sampler Core1, Writer Core0).");
    return true;
}

bool datalog_mount_sd() {
    if (sd_mounted) return true;
    return sd_mount();
}

void datalog_set_preamble(const char *text) {
    if (text) snprintf(s_preamble, sizeof(s_preamble), "%s", text);
    else s_preamble[0] = '\0';
}

bool datalog_start(uint32_t interval_ms, const char *filename) {
    if (!sd_mounted) {
        Serial.println("[DATALOG] Keine SD-Karte. Bitte erst einstecken und mounten.");
        return false;
    }
    if (interval_ms == 0) interval_ms = DATALOG_INTERVAL_MS;
    if (interval_ms >= 10 && interval_ms <= 60000) s_interval_ms = interval_ms;
    Serial.printf("[DATALOG] Intervall: %lu ms\n", (unsigned long)s_interval_ms);

    if (filename && filename[0]) {
        // Benutzerdefinierter Dateiname
        if (filename[0] == '/')
            snprintf(s_filename, sizeof(s_filename), "%s", filename);
        else
            snprintf(s_filename, sizeof(s_filename), "/%s", filename);
        // .csv anhängen falls nicht vorhanden
        if (!strstr(s_filename, ".csv")) {
            size_t l = strlen(s_filename);
            if (l + 4 < sizeof(s_filename)) strcat(s_filename, ".csv");
        }
    } else {
        make_filename(s_filename, sizeof(s_filename));
    }
    s_start_ms = millis();
    s_sample_seq = 0;
    s_buf_pos = 0;

    // Queue leeren
    SampleData dummy;
    while (xQueueReceive(s_sample_queue, &dummy, 0) == pdTRUE) {}

    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    if (s_log_file) { s_log_file.flush(); s_log_file.close(); }
    s_log_file = SD.open(s_filename, FILE_WRITE);
    if (!s_log_file) {
        Serial.printf("[DATALOG] FEHLER: Kann %s nicht erstellen!\n", s_filename);
        if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
        return false;
    }
    Serial.printf("[DATALOG] Datei erstellt: %s\n", s_filename);
    // Sequenz-Preamble schreiben (falls vorhanden)
    if (s_preamble[0]) {
        s_log_file.print(s_preamble);
        s_preamble[0] = '\0';
    }
    s_log_file.println(CSV_HEADER);
    s_log_file.flush();
    s_log_file.close();

    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);

    s_state = DATALOG_RECORDING;
    Serial.printf("[DATALOG] Aufzeichnung gestartet: %s (%lu ms)\n",
                  s_filename, (unsigned long)s_interval_ms);
    return true;
}

bool datalog_stop() {
    s_state = DATALOG_STOPPING;
    vTaskDelay(pdMS_TO_TICKS(1500));  // Writer-Task hat 1s Timeout + Flush-Zeit
    s_state = DATALOG_IDLE;
    Serial.println("[DATALOG] Aufzeichnung gestoppt.");
    return true;
}

bool datalog_pause()  { s_state = DATALOG_PAUSED;    return true; }
bool datalog_resume() { s_state = DATALOG_RECORDING; return true; }
bool datalog_set_interval(uint32_t ms) {
    if (ms < 10 || ms > 60000) return false;
    s_interval_ms = ms;
    return true;
}

DatalogState datalog_get_state()    { return s_state; }
const char*  datalog_get_filename() { return s_filename; }

int datalog_list_files(DatalogFileInfo *files, int max_files) {
    if (!sd_mounted) { Serial.println("[DATALOG] list_files: SD nicht gemountet"); return 0; }
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);

    File root = SD.open("/");
    if (!root) { Serial.println("[DATALOG] list_files: root open failed"); if (s_spi_mutex) xSemaphoreGive(s_spi_mutex); return 0; }
    int count = 0;
    while (count < max_files) {
        File f = root.openNextFile();
        if (!f) break;
        Serial.printf("[DATALOG] list: '%s' dir=%d size=%u\n", f.name(), f.isDirectory(), (unsigned)f.size());
        if (!f.isDirectory() && strstr(f.name(), ".csv")) {
            snprintf(files[count].name, sizeof(files[count].name), "%s", f.name());
            files[count].size_bytes = f.size();
            snprintf(files[count].date_str, sizeof(files[count].date_str), "N/A");
            count++;
        }
        f.close();
    }
    root.close();
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    Serial.printf("[DATALOG] list_files: %d Dateien gefunden\n", count);
    return count;
}

bool datalog_delete_file(const char *name) {
    if (!sd_mounted) return false;
    char path[72];
    snprintf(path, sizeof(path), "/%s", name);
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    bool ok = SD.remove(path);
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    return ok;
}

bool datalog_delete_all() {
    DatalogFileInfo files[DATALOG_MAX_FILES];
    int n = datalog_list_files(files, DATALOG_MAX_FILES);
    for (int i = 0; i < n; i++) datalog_delete_file(files[i].name);
    return true;
}

bool datalog_get_sd_info(DatalogSDInfo *info) {
    memset(info, 0, sizeof(*info));
    if (!sd_mounted) { return false; }
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    info->total_bytes = SD.totalBytes();
    info->free_bytes  = SD.totalBytes() - SD.usedBytes();
    info->mounted     = true;
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    return true;
}

bool datalog_read_chunk(const char *name, size_t offset,
                        uint8_t *buf, size_t buf_size, size_t *bytes_read) {
    char path[72];
    snprintf(path, sizeof(path), "/%s", name);

    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(500));
    File f = SD.open(path, FILE_READ);
    if (!f) {
        if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
        *bytes_read = 0;
        return false;
    }
    f.seek(offset);
    *bytes_read = f.read(buf, buf_size);
    f.close();
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    return true;
}
