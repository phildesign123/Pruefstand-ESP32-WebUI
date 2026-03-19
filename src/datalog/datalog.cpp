#include "datalog.h"
#include "../config.h"
#include "../hotend/hotend.h"
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
// Datenlogger: Ringpuffer im RAM → CSV auf SD-Karte
// Zwei Tasks: Sampler (Prio 3) + Writer (Prio 2)
// SPI-Mutex wird geteilt mit MAX31865
// =============================================================

struct LogEntry {
    char    timestamp[24];
    uint32_t time_ms;
    float    temperature_c;
    float    weight_g;
    float    speed_mm_s;
    uint8_t  motor_active;
};

// ── Ringpuffer ───────────────────────────────────────────────

static LogEntry s_ring[DATALOG_BUFFER_SIZE];
static volatile int s_ring_head = 0;   // Writer liest hier
static volatile int s_ring_tail = 0;   // Sampler schreibt hier
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static int ring_count() {
    int d = s_ring_tail - s_ring_head;
    return (d < 0) ? d + DATALOG_BUFFER_SIZE : d;
}

static bool ring_push(const LogEntry &e) {
    portENTER_CRITICAL(&s_ring_mux);
    int next = (s_ring_tail + 1) % DATALOG_BUFFER_SIZE;
    if (next == s_ring_head) {
        // Überlauf: ältesten überschreiben
        s_ring_head = (s_ring_head + 1) % DATALOG_BUFFER_SIZE;
        Serial.println("[DATALOG] Ringpuffer-Überlauf!");
    }
    s_ring[s_ring_tail] = e;
    s_ring_tail = next;
    portEXIT_CRITICAL(&s_ring_mux);
    return true;
}

static bool ring_pop(LogEntry *e) {
    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_head == s_ring_tail) {
        portEXIT_CRITICAL(&s_ring_mux);
        return false;
    }
    *e = s_ring[s_ring_head];
    s_ring_head = (s_ring_head + 1) % DATALOG_BUFFER_SIZE;
    portEXIT_CRITICAL(&s_ring_mux);
    return true;
}

// ── Status-Variablen ─────────────────────────────────────────

static SPIClass         *s_spi          = nullptr;
static SemaphoreHandle_t s_spi_mutex    = nullptr;
static volatile DatalogState s_state    = DATALOG_IDLE;
static volatile uint32_t s_interval_ms = DATALOG_INTERVAL_MS;
static char              s_filename[64] = {};
static uint32_t          s_start_ms    = 0;
static SemaphoreHandle_t s_flush_sem   = nullptr;
static Preferences       s_prefs;

// ── Dateiname generieren ─────────────────────────────────────

static void make_filename(char *buf, size_t len) {
    struct tm ti;
    if (getLocalTime(&ti)) {
        snprintf(buf, len, "/log_%04d%02d%02d_%02d%02d%02d.csv",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        s_prefs.begin("datalog", false);
        uint32_t cnt = s_prefs.getUInt("boot_cnt", 0) + 1;
        s_prefs.putUInt("boot_cnt", cnt);
        s_prefs.end();
        snprintf(buf, len, "/log_BOOT_%05lu.csv", (unsigned long)cnt);
    }
}

// ── SD-Karte initialisieren ───────────────────────────────────

static bool sd_mounted = false;

static bool sd_mount() {
    Serial.println("[DATALOG] SD-Karte wird gesucht...");
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(3000));
    bool ok = SD.begin(SD_CS, *s_spi, 4000000);
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    sd_mounted = ok;
    if (!ok) Serial.println("[DATALOG] SD-Karte nicht gefunden!");
    else     Serial.printf("[DATALOG] SD-Karte: %lluMB\n", SD.totalBytes() / (1024 * 1024));
    return ok;
}

// ── Sampler-Task ─────────────────────────────────────────────

static void sampler_task(void *arg) {
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_interval_ms));

        if (s_state != DATALOG_RECORDING) continue;

        LogEntry e;
        uint32_t now_ms = millis() - s_start_ms;

        // Zeitstempel
        struct tm ti;
        if (getLocalTime(&ti)) {
            snprintf(e.timestamp, sizeof(e.timestamp),
                     "%04d-%02d-%02dT%02d:%02d:%02d",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
        } else {
            snprintf(e.timestamp, sizeof(e.timestamp), "T+%lus",
                     (unsigned long)(now_ms / 1000));
        }

        e.time_ms       = now_ms;
        e.temperature_c = hotend_get_temperature();
        e.weight_g      = load_cell_get_weight_g();
        e.speed_mm_s    = motor_get_current_speed();
        e.motor_active  = motor_is_moving() ? 1 : 0;

        ring_push(e);

        // Flush-Signal wenn Puffer > 75 %
        if (ring_count() > (DATALOG_BUFFER_SIZE * 3 / 4)) {
            xSemaphoreGive(s_flush_sem);
        }
    }
}

// ── Writer-Task ───────────────────────────────────────────────

static void writer_task(void *arg) {
    TickType_t last_flush = xTaskGetTickCount();

    for (;;) {
        // Warten: entweder Flush-Interval oder Signal vom Sampler
        xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(DATALOG_FLUSH_INTERVAL_S * 1000));

        if (s_state == DATALOG_IDLE) continue;
        if (!sd_mounted)             continue;
        if (ring_count() == 0)       continue;

        // Zeilen formatieren (außerhalb Mutex)
        static char lines[DATALOG_BUFFER_SIZE][96];
        int  nlines = 0;
        LogEntry e;
        while (ring_pop(&e) && nlines < DATALOG_BUFFER_SIZE) {
            snprintf(lines[nlines], 96, "%s,%lu,%.2f,%.3f,%.2f,%d\n",
                     e.timestamp, (unsigned long)e.time_ms,
                     e.temperature_c, e.weight_g, e.speed_mm_s, e.motor_active);
            nlines++;
        }

        if (nlines == 0) continue;

        // SD-Karte beschreiben (unter Mutex)
        if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(200));
        File f = SD.open(s_filename, FILE_APPEND);
        if (f) {
            for (int i = 0; i < nlines; i++) f.print(lines[i]);
            f.flush();
            f.close();
        } else {
            s_state = DATALOG_ERROR;
        }
        if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);

        // Datei-Rotation prüfen
        if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(100));
        File info = SD.open(s_filename, FILE_READ);
        if (info) {
            size_t sz = info.size();
            info.close();
            if (sz >= (size_t)DATALOG_MAX_FILE_SIZE_MB * 1024 * 1024) {
                char new_name[72];
                make_filename(new_name, sizeof(new_name));
                strncpy(s_filename, new_name, sizeof(s_filename));
                File header = SD.open(s_filename, FILE_WRITE);
                if (header) {
                    header.println("timestamp,time_ms,temperature_c,weight_g,speed_mm_s,motor_active");
                    header.close();
                }
            }
        }
        if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
    }
}

// ── Öffentliche API ──────────────────────────────────────────

bool datalog_init(SPIClass &spi, SemaphoreHandle_t spi_mutex) {
    s_spi      = &spi;
    s_spi_mutex = spi_mutex;
    s_flush_sem = xSemaphoreCreateBinary();

    // SD-Karte wird nicht beim Boot gemountet (SD.begin blockiert ohne Karte).
    // Mount über API: POST /api/datalog/mount oder beim ersten datalog_start
    Serial.println("[DATALOG] SD-Mount verzögert (kein Auto-Mount beim Boot).");

    xTaskCreatePinnedToCore(sampler_task, "datalog_s", TASK_STACK_DATALOG_S,
                            nullptr, TASK_PRIO_DATALOG_S, nullptr, CORE_REALTIME);
    xTaskCreatePinnedToCore(writer_task,  "datalog_w", TASK_STACK_DATALOG_W,
                            nullptr, TASK_PRIO_DATALOG_W, nullptr, CORE_REALTIME);
    Serial.println("[DATALOG] Initialisiert.");
    return true;
}

bool datalog_mount_sd() {
    if (sd_mounted) return true;
    return sd_mount();
}

bool datalog_start(uint32_t interval_ms) {
    if (!sd_mounted) {
        Serial.println("[DATALOG] Keine SD-Karte. Bitte erst einstecken und mounten.");
        return false;
    }
    if (interval_ms >= 100 && interval_ms <= 60000) s_interval_ms = interval_ms;

    make_filename(s_filename, sizeof(s_filename));
    s_start_ms = millis();

    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    File f = SD.open(s_filename, FILE_WRITE);
    if (!f) {
        if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);
        return false;
    }
    f.println("timestamp,time_ms,temperature_c,weight_g,speed_mm_s,motor_active");
    f.close();
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);

    s_state = DATALOG_RECORDING;
    Serial.printf("[DATALOG] Aufzeichnung gestartet: %s (%lu ms)\n",
                  s_filename, (unsigned long)s_interval_ms);
    return true;
}

bool datalog_stop() {
    s_state = DATALOG_IDLE;
    xSemaphoreGive(s_flush_sem);  // Finalen Flush auslösen
    vTaskDelay(pdMS_TO_TICKS(200));
    Serial.println("[DATALOG] Aufzeichnung gestoppt.");
    return true;
}

bool datalog_pause()  { s_state = DATALOG_PAUSED;    return true; }
bool datalog_resume() { s_state = DATALOG_RECORDING; return true; }
bool datalog_set_interval(uint32_t ms) {
    if (ms < 100 || ms > 60000) return false;
    s_interval_ms = ms;
    return true;
}

DatalogState datalog_get_state()    { return s_state; }
const char*  datalog_get_filename() { return s_filename; }

int datalog_list_files(DatalogFileInfo *files, int max_files) {
    if (!sd_mounted) return 0;
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);

    File root = SD.open("/");
    int count = 0;
    while (count < max_files) {
        File f = root.openNextFile();
        if (!f) break;
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
    if (!sd_mounted) { info->mounted = false; return false; }
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
