// =============================================================
// Datenlogger-Test: SD-Karte, Ringpuffer, CSV, CLI
// Serielle Befehle (115200 Baud):
//   status                   – SD/Aufzeichnungs-Status
//   start [interval_ms]      – Aufzeichnung starten
//   stop                     – Aufzeichnung stoppen
//   pause                    – Aufzeichnung pausieren
//   resume                   – Aufzeichnung fortsetzen
//   list                     – CSV-Dateien auflisten
//   cat <file> [n]           – Erste N Zeilen ausgeben (Default: 20)
//   tail <file> [n]          – Letzte N Zeilen ausgeben (Default: 20)
//   delete <file>            – Datei löschen
//   delete all               – Alle Dateien löschen (Bestätigung)
//   sdinfo                   – SD-Speicher anzeigen
//   help                     – Befehlsübersicht
// =============================================================

#include <Arduino.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "datalog/datalog.h"

static SPIClass           vspi(VSPI);
static SemaphoreHandle_t  spi_mutex;

// Zustand für "delete all"-Bestätigung
static bool s_delete_all_pending = false;

// ── Hilfsfunktion: Bytes lesbar formatieren ───────────────────
static void format_bytes(char *buf, size_t bufsize, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, bufsize, "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(buf, bufsize, "%.1f MB", (double)bytes / (1024.0 * 1024));
    else if (bytes >= 1024ULL)
        snprintf(buf, bufsize, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, bufsize, "%llu B", (unsigned long long)bytes);
}

// ── Befehl: status ────────────────────────────────────────────
static void cmd_status() {
    DatalogState  state    = datalog_get_state();
    const char   *filename = datalog_get_filename();
    DatalogSDInfo sdinfo;
    bool          sd_ok    = datalog_get_sd_info(&sdinfo);

    const char *state_str;
    switch (state) {
        case DATALOG_RECORDING: state_str = "RECORDING"; break;
        case DATALOG_PAUSED:    state_str = "PAUSED";    break;
        case DATALOG_ERROR:     state_str = "ERROR";     break;
        default:                state_str = "IDLE";      break;
    }

    Serial.printf("[DATALOG] Status: %s\n", state_str);
    if (filename && filename[0])
        Serial.printf("  File:     %s\n", filename);

    if (sd_ok) {
        char free_str[20], total_str[20];
        format_bytes(free_str,  sizeof(free_str),  sdinfo.free_bytes);
        format_bytes(total_str, sizeof(total_str), sdinfo.total_bytes);
        Serial.printf("  SD Card:  %s free / %s total\n", free_str, total_str);
    } else {
        Serial.println("  SD Card:  nicht erreichbar");
    }
}

// ── Befehl: start [interval_ms] ──────────────────────────────
static void cmd_start(const String &args) {
    uint32_t interval = DATALOG_INTERVAL_MS;
    if (args.length() > 0) {
        int v = args.toInt();
        if (v > 0) interval = (uint32_t)v;
    }
    if (datalog_start(interval))
        Serial.printf("[DATALOG] Aufzeichnung gestartet (Interval: %u ms).\n", interval);
    else
        Serial.println("[ERR] Start fehlgeschlagen (läuft bereits oder SD-Fehler).");
}

// ── Befehl: list ─────────────────────────────────────────────
static void cmd_list() {
    static DatalogFileInfo files[DATALOG_MAX_FILES];
    int count = datalog_list_files(files, DATALOG_MAX_FILES);

    if (count < 0) {
        Serial.println("[ERR] Dateiliste konnte nicht gelesen werden.");
        return;
    }
    if (count == 0) {
        Serial.println("[DATALOG] Keine Dateien auf der SD-Karte.");
        return;
    }

    const char *active = datalog_get_filename();
    Serial.println("[DATALOG] Files on SD card:");

    uint64_t total_bytes = 0;
    for (int i = 0; i < count; i++) {
        char size_str[20];
        format_bytes(size_str, sizeof(size_str), files[i].size_bytes);
        bool is_active = (active && strcmp(files[i].name, active) == 0);
        Serial.printf("  %2d. %-36s %8s   %s%s\n",
                      i + 1, files[i].name, size_str, files[i].date_str,
                      is_active ? "  \u2190 ACTIVE" : "");
        total_bytes += files[i].size_bytes;
    }
    char total_str[20];
    format_bytes(total_str, sizeof(total_str), total_bytes);
    Serial.printf("  Total: %d file%s, %s used\n",
                  count, count == 1 ? "" : "s", total_str);
}

// ── Hilfsfunktion: Datei zeilenweise lesen (cat / tail) ───────
// Liest max 'line_limit' Zeilen ab Offset 'skip_lines' (tail: skip berechnet)
static void print_file_lines(const char *name, int n_lines, bool do_tail) {
    const size_t CHUNK = 512;
    uint8_t buf[CHUNK + 1];

    // ── cat: erste N Zeilen ausgeben ──────────────────────────
    if (!do_tail) {
        size_t offset = 0;
        int    printed = 0;
        while (printed < n_lines) {
            size_t got = 0;
            if (!datalog_read_chunk(name, offset, buf, CHUNK, &got) || got == 0) break;
            buf[got] = '\0';

            char *p = (char *)buf;
            while (*p && printed < n_lines) {
                char *nl = strchr(p, '\n');
                if (nl) {
                    *nl = '\0';
                    Serial.println(p);
                    printed++;
                    p = nl + 1;
                } else {
                    // Keine Newline im Chunk – Restzeile merken, nächsten Chunk laden
                    // Vereinfachung: ganzen Restblock ausgeben und Zeile zählen
                    Serial.println(p);
                    printed++;
                    break;
                }
            }
            offset += got;
        }
        return;
    }

    // ── tail: letzte N Zeilen ausgeben ───────────────────────
    // Strategie: gesamte Datei einlesen, letzte N Newlines finden
    // (Bei großen Dateien vom Ende rückwärts in Chunks lesen)
    // Vereinfachte Version: Datei vollständig lesen, Zeilen sammeln

    // Gesamtgröße ermitteln durch chunk-reads
    // Zuletzt gelesene Zeilen in einem Ringpuffer der Größe n_lines speichern
    const int MAX_LINES = 200;
    if (n_lines > MAX_LINES) n_lines = MAX_LINES;

    static String lines[MAX_LINES];
    int    head    = 0;
    int    total   = 0;
    size_t offset  = 0;
    String partial = "";

    while (true) {
        size_t got = 0;
        if (!datalog_read_chunk(name, offset, buf, CHUNK, &got) || got == 0) break;
        buf[got] = '\0';
        offset += got;

        String chunk = partial + String((char *)buf);
        partial = "";

        int pos = 0;
        while (pos < (int)chunk.length()) {
            int nl = chunk.indexOf('\n', pos);
            if (nl < 0) {
                partial = chunk.substring(pos);
                break;
            }
            lines[head % MAX_LINES] = chunk.substring(pos, nl);
            head++;
            total++;
            pos = nl + 1;
        }
    }
    if (partial.length() > 0) {
        lines[head % MAX_LINES] = partial;
        head++;
        total++;
    }

    int start = (total > n_lines) ? (head - n_lines) : (head - total);
    for (int i = start; i < head; i++) {
        Serial.println(lines[i % MAX_LINES]);
    }
}

// ── Befehl: cat <file> [n] ────────────────────────────────────
static void cmd_cat(const String &args) {
    int sp = args.indexOf(' ');
    String filename, n_str;
    if (sp < 0) {
        filename = args;
        n_str    = "";
    } else {
        filename = args.substring(0, sp);
        n_str    = args.substring(sp + 1);
        n_str.trim();
    }
    if (filename.length() == 0) {
        Serial.println("[ERR] Syntax: cat <filename> [lines]"); return;
    }
    int n = (n_str.length() > 0) ? n_str.toInt() : 20;
    if (n <= 0) n = 20;

    Serial.printf("[DATALOG] Head of %s (%d lines):\n", filename.c_str(), n);
    print_file_lines(filename.c_str(), n, false);
}

// ── Befehl: tail <file> [n] ───────────────────────────────────
static void cmd_tail(const String &args) {
    int sp = args.indexOf(' ');
    String filename, n_str;
    if (sp < 0) {
        filename = args;
        n_str    = "";
    } else {
        filename = args.substring(0, sp);
        n_str    = args.substring(sp + 1);
        n_str.trim();
    }
    if (filename.length() == 0) {
        Serial.println("[ERR] Syntax: tail <filename> [lines]"); return;
    }
    int n = (n_str.length() > 0) ? n_str.toInt() : 20;
    if (n <= 0) n = 20;

    Serial.printf("[DATALOG] Tail of %s (%d lines):\n", filename.c_str(), n);
    print_file_lines(filename.c_str(), n, true);
}

// ── Befehl: delete <file> / delete all ───────────────────────
static void cmd_delete(const String &args) {
    String a = args;
    a.trim();

    if (a.equalsIgnoreCase("all")) {
        if (!s_delete_all_pending) {
            s_delete_all_pending = true;
            Serial.println("[DATALOG] ACHTUNG: Alle CSV-Dateien werden gelöscht.");
            Serial.println("          Nochmals 'delete all' eingeben zur Bestätigung.");
        } else {
            s_delete_all_pending = false;
            if (datalog_delete_all())
                Serial.println("[DATALOG] Alle Dateien gelöscht.");
            else
                Serial.println("[ERR] Löschen fehlgeschlagen.");
        }
        return;
    }

    s_delete_all_pending = false;
    if (a.length() == 0) {
        Serial.println("[ERR] Syntax: delete <filename>  |  delete all");
        return;
    }

    if (datalog_delete_file(a.c_str()))
        Serial.printf("[DATALOG] '%s' gelöscht.\n", a.c_str());
    else
        Serial.printf("[ERR] '%s' nicht gefunden oder Löschen fehlgeschlagen.\n", a.c_str());
}

// ── Befehl: sdinfo ────────────────────────────────────────────
static void cmd_sdinfo() {
    DatalogSDInfo info;
    if (!datalog_get_sd_info(&info)) {
        Serial.println("[ERR] SD-Karte nicht erreichbar.");
        return;
    }
    char free_str[20], total_str[20], used_str[20];
    format_bytes(free_str,  sizeof(free_str),  info.free_bytes);
    format_bytes(total_str, sizeof(total_str), info.total_bytes);
    format_bytes(used_str,  sizeof(used_str),
                 info.total_bytes > info.free_bytes
                     ? info.total_bytes - info.free_bytes : 0);

    Serial.println("[DATALOG] SD-Karten-Info:");
    Serial.printf("  Eingebunden: %s\n", info.mounted ? "ja" : "NEIN");
    Serial.printf("  Gesamt:      %s\n", total_str);
    Serial.printf("  Belegt:      %s\n", used_str);
    Serial.printf("  Frei:        %s\n", free_str);
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Datenlogger-Test (Prüfstand ESP32) ===");
    Serial.println("(Mock-Datenquellen: linear Temp, Sinus-Gewicht)");

    spi_mutex = xSemaphoreCreateMutex();
    vspi.begin(MAX_CLK, MAX_MISO, MAX_MOSI, SD_CS);

    if (!datalog_init(vspi, spi_mutex)) {
        Serial.println("[ERR] Datalog-Init fehlgeschlagen! SD-Karte prüfen.");
    } else {
        Serial.println("[OK] Datalog bereit.");
    }

    delay(200);

    Serial.println("Befehle:");
    Serial.println("  status               – SD/Aufzeichnungs-Status");
    Serial.println("  start [interval_ms]  – Aufzeichnung starten");
    Serial.println("  stop                 – Aufzeichnung stoppen");
    Serial.println("  pause / resume       – Pausieren/Fortsetzen");
    Serial.println("  list                 – Dateien auflisten");
    Serial.println("  cat <file> [n]       – Erste N Zeilen");
    Serial.println("  tail <file> [n]      – Letzte N Zeilen");
    Serial.println("  delete <file>        – Datei löschen");
    Serial.println("  delete all           – Alle Dateien löschen");
    Serial.println("  sdinfo               – SD-Speicher");
    Serial.println("  help                 – Diese Übersicht");
}

// ── Loop ──────────────────────────────────────────────────────
static unsigned long s_last_status = 0;

void loop() {
    // Alle 5 s automatischer Kurzstatus
    if (millis() - s_last_status >= 5000) {
        s_last_status = millis();
        DatalogState st = datalog_get_state();
        const char *fn  = datalog_get_filename();
        if (st == DATALOG_RECORDING)
            Serial.printf("[DATALOG] REC  %s\n", fn ? fn : "");
        else if (st == DATALOG_PAUSED)
            Serial.printf("[DATALOG] PAUSED  %s\n", fn ? fn : "");
    }

    if (!Serial.available()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("> %s\n", cmd.c_str());

    // Bestätigungs-Zustand zurücksetzen wenn andere Eingabe
    String cmd_lower = cmd;
    cmd_lower.toLowerCase();
    if (!cmd_lower.startsWith("delete")) s_delete_all_pending = false;

    int    sp   = cmd_lower.indexOf(' ');
    String verb = (sp < 0) ? cmd_lower : cmd_lower.substring(0, sp);
    String args = (sp < 0) ? String("") : cmd.substring(sp + 1);
    args.trim();

    if (verb == "status") {
        cmd_status();

    } else if (verb == "start") {
        cmd_start(args);

    } else if (verb == "stop") {
        if (datalog_stop())
            Serial.println("[DATALOG] Aufzeichnung gestoppt.");
        else
            Serial.println("[ERR] Kein aktiver Lauf.");

    } else if (verb == "pause") {
        if (datalog_pause())
            Serial.println("[DATALOG] Pausiert.");
        else
            Serial.println("[ERR] Pause nicht möglich (nicht aktiv?).");

    } else if (verb == "resume") {
        if (datalog_resume())
            Serial.println("[DATALOG] Fortgesetzt.");
        else
            Serial.println("[ERR] Resume nicht möglich (nicht pausiert?).");

    } else if (verb == "list") {
        cmd_list();

    } else if (verb == "cat") {
        cmd_cat(args);

    } else if (verb == "tail") {
        cmd_tail(args);

    } else if (verb == "delete") {
        cmd_delete(args);

    } else if (verb == "sdinfo") {
        cmd_sdinfo();

    } else if (verb == "help") {
        Serial.println("status | start [ms] | stop | pause | resume");
        Serial.println("list | cat <f> [n] | tail <f> [n] | delete <f> | delete all");
        Serial.println("sdinfo | help");

    } else {
        Serial.println("Unbekannt. 'help' für Übersicht.");
    }
}
