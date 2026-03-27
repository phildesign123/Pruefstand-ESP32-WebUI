#include "webui.h"
#include "../config.h"
#include "../hotend/hotend.h"
#include "../load_cell/load_cell.h"
#include "../motor/motor.h"
#include "../datalog/datalog.h"
#include "sequencer.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>

static Preferences s_prefs;

// =============================================================
// Web-UI: ESPAsyncWebServer + WebSocket + REST-API
// Frontend aus LittleFS (/data-Partition)
// =============================================================

static AsyncWebServer  s_server(WEBUI_PORT);
static AsyncWebSocket  s_ws(WEBUI_WS_PATH);
static volatile time_t s_pending_epoch = 0;  // Browser-Zeit, wird im Push-Task gesetzt

// ── WebSocket-Push-Task (10 Hz) ──────────────────────────────

static void ws_push_task(void *arg) {
    uint8_t cleanup_cnt = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WEBUI_WS_INTERVAL_MS));

        // Browser-Zeit setzen (verzögert aus async-Kontext)
        if (s_pending_epoch) {
            struct timeval tv = { .tv_sec = (time_t)s_pending_epoch, .tv_usec = 0 };
            settimeofday(&tv, nullptr);
            Serial.printf("[TIME] Browser-Zeit gesetzt: %lu\n", (unsigned long)s_pending_epoch);
            s_pending_epoch = 0;
        }

        // Alle 2 Sekunden tote Clients aufräumen
        if (++cleanup_cnt >= 20) {
            cleanup_cnt = 0;
            s_ws.cleanupClients();
        }

        if (s_ws.count() == 0) continue;

        // Kompaktes JSON zusammenstellen
        const char *dl_states[] = {"idle","recording","paused","error","stopping"};
        char buf[300];
        snprintf(buf, sizeof(buf),
            "{\"t\":%lu,\"temp\":%.2f,\"temp_target\":%.2f,\"duty\":%.3f,"
            "\"weight\":%.4f,\"speed\":%.2f,\"motor\":%d,"
            "\"seq\":%d,\"seq_state\":\"%s\",\"seq_remain\":%.1f,"
            "\"dl_state\":\"%s\"}",
            (unsigned long)millis(),
            hotend_get_temperature(),
            hotend_get_target(),
            hotend_get_duty(),
            load_cell_get_weight_g() * 0.00981f,
            motor_get_current_speed(),
            motor_is_moving() ? 1 : 0,
            sequencer_get_active_index(),
            sequencer_state_string(),
            sequencer_get_remaining_s(),
            dl_states[(int)datalog_get_state()]);

        // Nur senden wenn kein Client eine volle Queue hat
        // Verhindert "Too many messages queued" → Connection-Drop
        bool all_ready = true;
        for (auto &c : s_ws.getClients()) {
            if (c.status() == WS_CONNECTED && c.queueIsFull()) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) {
            s_ws.textAll(buf);
        }
    }
}

// ── WebSocket Befehlsverarbeitung ────────────────────────────

static void on_ws_message(AsyncWebSocketClient *client,
                           const char *data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;

    const char *cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "set_target") == 0) {
        float v = doc["value"] | 0.0f;
        hotend_set_target(v);
    } else if (strcmp(cmd, "motor_jog") == 0) {
        float  dist  = doc["dist"]  | 1.0f;
        float  speed = doc["speed"] | 3.0f;
        MotorDir dir = (strcmp(doc["dir"] | "fwd", "rev") == 0)
                       ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
        motor_move_distance(speed, dist, dir);
    } else if (strcmp(cmd, "motor_stop") == 0) {
        motor_stop();
    } else if (strcmp(cmd, "tare") == 0) {
        load_cell_tare();
    }
}

static void on_ws_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                         AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->opcode == WS_TEXT) {
            data[len] = 0;
            on_ws_message(client, (char *)data, len);
        }
    } else if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] Client #%u verbunden\n", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] Client #%u getrennt\n", client->id());
    }
}

// ── REST-API Handler ─────────────────────────────────────────

// --- Hotend ---
static void api_hotend_get(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["temp"]       = hotend_get_temperature();
    doc["target"]     = hotend_get_target();
    doc["duty"]       = hotend_get_duty();
    doc["fault"]      = (int)hotend_get_fault();
    doc["fault_str"]  = hotend_get_fault_string();
    char buf[256];
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
}

static void api_hotend_set_target(AsyncWebServerRequest *req,
                                   JsonVariant &body) {
    float t = body["target_c"] | -1.0f;
    if (t < 0 || !hotend_set_target(t))
        req->send(400, "application/json", "{\"error\":\"invalid\"}");
    else
        req->send(200, "application/json", "{\"ok\":true}");
}

static void api_hotend_set_pid(AsyncWebServerRequest *req,
                                JsonVariant &body) {
    hotend_set_pid(body["kp"] | DEFAULT_Kp,
                   body["ki"] | DEFAULT_Ki,
                   body["kd"] | DEFAULT_Kd);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_hotend_clear_fault(AsyncWebServerRequest *req) {
    hotend_clear_fault();
    req->send(200, "application/json", "{\"ok\":true}");
}

// --- Wägezelle ---
static void api_loadcell_get(AsyncWebServerRequest *req) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"weight_N\":%.4f,\"raw\":%ld,\"calibrated\":%s}",
             load_cell_get_weight_g() * 0.00981f,
             (long)load_cell_get_raw(),
             load_cell_is_calibrated() ? "true" : "false");
    req->send(200, "application/json", buf);
}

static void api_loadcell_tare(AsyncWebServerRequest *req) {
    load_cell_tare();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_loadcell_cal(AsyncWebServerRequest *req, JsonVariant &body) {
    float w = body["weight_g"] | 0.0f;
    if (w <= 0 || !load_cell_calibrate(w))
        req->send(400, "application/json", "{\"error\":\"invalid\"}");
    else
        req->send(200, "application/json", "{\"ok\":true}");
}

// --- Motor ---
static void api_motor_status(AsyncWebServerRequest *req) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"moving\":%s,\"speed_mm_s\":%.2f,\"steps_per_mm\":%.2f,\"calibrated\":%s}",
             motor_is_moving() ? "true" : "false",
             motor_get_current_speed(),
             motor_get_esteps(),
             motor_esteps_is_calibrated() ? "true" : "false");
    req->send(200, "application/json", buf);
}

static void api_motor_move(AsyncWebServerRequest *req, JsonVariant &body) {
    float speed = body["speed"] | 3.0f;
    float dur   = body["duration_s"] | 0.0f;
    MotorDir dir = (strcmp(body["dir"] | "fwd", "rev") == 0)
                   ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
    motor_move(speed, dur, dir);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_move_dist(AsyncWebServerRequest *req, JsonVariant &body) {
    float speed = body["speed"] | 3.0f;
    float dist  = body["distance_mm"] | 0.0f;
    MotorDir dir = (strcmp(body["dir"] | "fwd", "rev") == 0)
                   ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
    motor_move_distance(speed, dist, dir);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_stop(AsyncWebServerRequest *req) {
    motor_stop();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_set_current(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_set_current(body["run_ma"] | 800, body["hold_ma"] | 400);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_set_microstep(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_set_microstep(body["microstep"] | 16);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_set_stealthchop(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_set_stealthchop(body["enable"] | true);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_set_interpolation(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_set_interpolation(body["enable"] | true);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_get_config(AsyncWebServerRequest *req) {
    TMC2208Config cfg;
    if (!motor_get_tmc_config(&cfg)) {
        req->send(500, "application/json", "{\"error\":\"read failed\"}");
        return;
    }
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"run_ma\":%u,\"hold_ma\":%u,\"microsteps\":%u,"
        "\"stealthchop\":%s,\"interpolation\":%s,\"dir_invert\":%s}",
        cfg.run_current_ma, cfg.hold_current_ma, cfg.microsteps,
        cfg.stealthchop ? "true" : "false",
        cfg.interpolation ? "true" : "false",
        motor_get_dir_invert() ? "true" : "false");
    req->send(200, "application/json", buf);
}

static void api_motor_get_dir(AsyncWebServerRequest *req) {
    char buf[40];
    snprintf(buf, sizeof(buf), "{\"invert\":%s}", motor_get_dir_invert() ? "true" : "false");
    req->send(200, "application/json", buf);
}

static void api_motor_set_dir(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_set_dir_invert(body["invert"] | false);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_get_esteps(AsyncWebServerRequest *req) {
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"steps_per_mm\":%.2f,\"valid\":%s}",
             motor_get_esteps(), motor_esteps_is_calibrated() ? "true" : "false");
    req->send(200, "application/json", buf);
}

static void api_motor_set_esteps(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_set_esteps(body["steps_per_mm"] | MOTOR_E_STEPS_PER_MM);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_cal_start(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_calibrate_start(body["distance_mm"] | 100.0f, body["speed"] | 3.0f);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_motor_cal_apply(AsyncWebServerRequest *req, JsonVariant &body) {
    motor_calibrate_apply(body["remaining_mm"] | 0.0f);
    req->send(200, "application/json", "{\"ok\":true}");
}

// --- Datenlogger ---
static void api_datalog_status(AsyncWebServerRequest *req) {
    const char *states[] = {"idle","recording","paused","error","stopping"};
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"file\":\"%s\",\"interval_ms\":%lu}",
             states[(int)datalog_get_state()],
             datalog_get_filename(),
             (unsigned long)DATALOG_INTERVAL_MS);
    req->send(200, "application/json", buf);
}

// Verzögerter datalog_start (außerhalb async_tcp-Kontext)
static volatile bool     s_dl_start_pending = false;
static uint32_t          s_dl_start_iv      = 0;
static char              s_dl_start_fname[64];
static TaskHandle_t      s_dl_start_task    = nullptr;

static void datalog_start_worker(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool ok = datalog_start(s_dl_start_iv,
                                s_dl_start_fname[0] ? s_dl_start_fname : nullptr);
        Serial.printf("[WEBUI] datalog_start(%lu, %s) => %s\n",
                      (unsigned long)s_dl_start_iv,
                      s_dl_start_fname[0] ? s_dl_start_fname : "auto",
                      ok ? "OK" : "FAIL");
        s_dl_start_pending = false;
    }
}

static void api_datalog_start(AsyncWebServerRequest *req, JsonVariant &body) {
    if (s_dl_start_pending) {
        req->send(409, "application/json", "{\"error\":\"start already pending\"}");
        return;
    }
    s_dl_start_iv = body["interval_ms"] | (uint32_t)DATALOG_INTERVAL_MS;
    const char *fname = body["filename"] | (const char*)nullptr;
    if (fname) snprintf(s_dl_start_fname, sizeof(s_dl_start_fname), "%s", fname);
    else s_dl_start_fname[0] = '\0';
    s_dl_start_pending = true;
    xTaskNotifyGive(s_dl_start_task);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_datalog_stop(AsyncWebServerRequest *req) {
    datalog_stop();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_datalog_files(AsyncWebServerRequest *req) {
    DatalogFileInfo files[50];
    int n = datalog_list_files(files, 50);
    JsonDocument doc;
    JsonArray arr = doc["files"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]  = files[i].name;
        o["size"]  = files[i].size_bytes;
        o["date"]  = files[i].date_str;
        o["active"] = (strcmp(files[i].name, datalog_get_filename()) == 0);
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

static void api_datalog_download(AsyncWebServerRequest *req) {
    String name = req->pathArg(0);
    char path[72];
    snprintf(path, sizeof(path), "/%s", name.c_str());

    // Streaming-Download mit ChunkedResponse
    AsyncWebServerResponse *resp =
        req->beginChunkedResponse("text/csv",
            [name](uint8_t *buf, size_t max, size_t offset) -> size_t {
                size_t read = 0;
                datalog_read_chunk(name.c_str(), offset, buf, max, &read);
                return read;
            });
    resp->addHeader("Content-Disposition",
                    ("attachment; filename=\"" + name + "\"").c_str());
    req->send(resp);
}

static void api_datalog_delete(AsyncWebServerRequest *req) {
    String name = req->pathArg(0);
    datalog_delete_file(name.c_str());
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_datalog_delete_all(AsyncWebServerRequest *req, JsonVariant &body) {
    if (body["confirm"] | false) datalog_delete_all();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_datalog_sdinfo(AsyncWebServerRequest *req) {
    DatalogSDInfo info;
    datalog_get_sd_info(&info);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"total\":%llu,\"free\":%llu,\"mounted\":%s}",
             info.total_bytes, info.free_bytes,
             info.mounted ? "true" : "false");
    req->send(200, "application/json", buf);
}

// --- Sequencer ---
static void api_seq_get(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["state"]  = sequencer_state_string();
    doc["active"] = sequencer_get_active_index();
    JsonArray arr = doc["sequences"].to<JsonArray>();
    for (int i = 0; i < sequencer_count(); i++) {
        Sequence s; sequencer_get(i, &s);
        JsonObject o = arr.add<JsonObject>();
        o["temp_c"]     = s.temperature_c;
        o["speed_mm_s"] = s.speed_mm_s;
        o["duration_s"] = s.duration_s;
    }
    char buf[1024];
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
}

static void api_seq_add(AsyncWebServerRequest *req, JsonVariant &body) {
    sequencer_add(body["temp_c"] | 200.0f,
                  body["speed_mm_s"] | 3.0f,
                  body["duration_s"] | 60.0f);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_seq_start(AsyncWebServerRequest *req, JsonVariant &body) {
    const char *fn = body["filename"] | (const char*)nullptr;
    if (!sequencer_start(fn))
        req->send(400, "application/json", "{\"error\":\"cannot start\"}");
    else
        req->send(200, "application/json", "{\"ok\":true}");
}

static void api_seq_stop(AsyncWebServerRequest *req) {
    sequencer_stop();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_seq_clear(AsyncWebServerRequest *req) {
    sequencer_clear();
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_seq_delete(AsyncWebServerRequest *req, JsonVariant &body) {
    int idx = body["index"] | -1;
    if (!sequencer_delete(idx))
        req->send(400, "application/json", "{\"error\":\"invalid index\"}");
    else
        req->send(200, "application/json", "{\"ok\":true}");
}

// --- Sequencer-Presets (SD-Karte) ---
#define PRESETS_PATH "/presets.json"
#define PRESETS_MAX_SIZE 8192

static bool presets_load(JsonDocument &doc) {
    char *buf = (char*)malloc(PRESETS_MAX_SIZE);
    if (!buf) return false;
    size_t len = 0;
    bool ok = datalog_read_raw_file(PRESETS_PATH, buf, PRESETS_MAX_SIZE, &len);
    if (ok && len > 0) {
        if (deserializeJson(doc, buf, len) != DeserializationError::Ok)
            doc.clear();
    }
    free(buf);
    return ok;
}

static bool presets_save(JsonDocument &doc) {
    String out;
    serializeJson(doc, out);
    return datalog_write_raw_file(PRESETS_PATH, out.c_str(), out.length());
}

static void api_seq_presets_get(AsyncWebServerRequest *req) {
    JsonDocument doc;
    if (!presets_load(doc)) {
        req->send(500, "application/json", "{\"error\":\"sd read\"}");
        return;
    }
    // Response: {"presets": { ... }}
    JsonDocument resp;
    resp["presets"] = doc.as<JsonObject>();
    String out;
    serializeJson(resp, out);

    req->send(200, "application/json", out);
}

static void api_seq_presets_save(AsyncWebServerRequest *req, JsonVariant &body) {
    const char *name = body["name"] | (const char*)nullptr;
    if (!name || strlen(name) == 0) {
        req->send(400, "application/json", "{\"error\":\"name missing\"}");
        return;
    }
    JsonArray seqs = body["sequences"].as<JsonArray>();
    if (seqs.isNull() || seqs.size() == 0) {
        req->send(400, "application/json", "{\"error\":\"sequences missing\"}");
        return;
    }
    JsonDocument doc;
    presets_load(doc);
    JsonArray arr = doc[name].to<JsonArray>();
    for (JsonObject s : seqs) {
        JsonObject o = arr.add<JsonObject>();
        o["temp_c"]     = s["temp_c"] | 0.0f;
        o["speed_mm_s"] = s["speed_mm_s"] | 0.0f;
        o["duration_s"] = s["duration_s"] | 0.0f;
    }

    bool ok = presets_save(doc);
    req->send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"sd write\"}");
}

static void api_seq_presets_delete(AsyncWebServerRequest *req, JsonVariant &body) {
    const char *name = body["name"] | (const char*)nullptr;
    if (!name || strlen(name) == 0) {
        req->send(400, "application/json", "{\"error\":\"name missing\"}");
        return;
    }
    JsonDocument doc;
    presets_load(doc);
    doc.remove(name);

    bool ok = presets_save(doc);
    req->send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"sd write\"}");
}

// ── WiFi-API ─────────────────────────────────────────────────

// WiFi-Credentials (RAM, geladen aus NVS)
static char s_ssid_sta[33];
static char s_pass_sta[33];
static char s_ssid_ap[33];
static char s_pass_ap[33];
static bool s_wifi_sta_mode = false;  // true = STA verbunden, false = AP-Modus

static void api_wifi_status(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["ap_ssid"]       = s_ssid_ap;
    doc["ap_password"]   = s_pass_ap;
    doc["ap_ip"]         = s_wifi_sta_mode ? "" : WiFi.softAPIP().toString();
    doc["sta_ssid"]      = s_ssid_sta;
    doc["sta_password"]  = s_pass_sta;
    doc["sta_connected"] = s_wifi_sta_mode;
    doc["sta_ip"]        = s_wifi_sta_mode ? WiFi.localIP().toString() : "";
    doc["sta_rssi"]      = s_wifi_sta_mode ? WiFi.RSSI() : 0;
    doc["mac"]           = WiFi.macAddress();
    doc["mode"]          = s_wifi_sta_mode ? "STA" : "AP";
    char buf[512];
    serializeJson(doc, buf, sizeof(buf));
    req->send(200, "application/json", buf);
}

static void api_wifi_save(AsyncWebServerRequest *req, JsonVariant &body) {
    const char *sta_ssid = body["sta_ssid"];
    const char *sta_pass = body["sta_password"];
    const char *ap_ssid  = body["ap_ssid"];
    const char *ap_pass  = body["ap_password"];

    if (sta_ssid) {
        strncpy(s_ssid_sta, sta_ssid, 32); s_ssid_sta[32] = 0;
        s_prefs.putString("sta_ssid", s_ssid_sta);
    }
    if (sta_pass) {
        strncpy(s_pass_sta, sta_pass, 32); s_pass_sta[32] = 0;
        s_prefs.putString("sta_pass", s_pass_sta);
    }
    if (ap_ssid && strlen(ap_ssid) > 0) {
        strncpy(s_ssid_ap, ap_ssid, 32); s_ssid_ap[32] = 0;
        s_prefs.putString("ap_ssid", s_ssid_ap);
    }
    if (ap_pass) {
        strncpy(s_pass_ap, ap_pass, 32); s_pass_ap[32] = 0;
        s_prefs.putString("ap_pass", s_pass_ap);
    }
    Serial.printf("[WIFI] Gespeichert: STA='%s' AP='%s'\n", s_ssid_sta, s_ssid_ap);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void api_wifi_scan(AsyncWebServerRequest *req) {
    Serial.println("[WIFI] Scan gestartet...");
    int n = WiFi.scanNetworks();
    Serial.printf("[WIFI] Scan fertig: %d Netzwerke\n", n);

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    // Duplikate vermeiden
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        // Duplikat-Check
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (WiFi.SSID(j) == ssid) { dup = true; break; }
        }
        if (dup) continue;
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = ssid;
        net["rssi"] = WiFi.RSSI(i);
        net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    // Aktuelle STA-SSID immer mit aufnehmen falls nicht gefunden
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == s_ssid_sta) { found = true; break; }
    }
    if (!found && strlen(s_ssid_sta) > 0) {
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = s_ssid_sta;
        net["rssi"] = 0;
        net["open"] = false;
    }
    WiFi.scanDelete();

    char buf[1536];
    serializeJson(doc, buf, sizeof(buf));
    req->send(200, "application/json", buf);
}

// ── Routen registrieren ──────────────────────────────────────

static void register_routes() {
    // Favicon (IMPT Logo – Hexagon mit Netzwerk)
    s_server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *r){
        r->send(200, "image/svg+xml",
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
            "<polygon points='32,2 58,17 58,47 32,62 6,47 6,17' fill='none' stroke='#222' stroke-width='5'/>"
            "<polygon points='32,10 50,20 50,40 32,50 14,40 14,20' fill='none' stroke='#f06a00' stroke-width='3'/>"
            "<line x1='32' y1='10' x2='32' y2='50' stroke='#f06a00' stroke-width='3'/>"
            "<line x1='14' y1='20' x2='50' y2='40' stroke='#f06a00' stroke-width='3'/>"
            "<line x1='50' y1='20' x2='14' y2='40' stroke='#f06a00' stroke-width='3'/>"
            "<circle cx='32' cy='10' r='4' fill='#c040a0'/>"
            "<circle cx='50' cy='20' r='4' fill='#c040a0'/>"
            "<circle cx='50' cy='40' r='4' fill='#c040a0'/>"
            "<circle cx='32' cy='50' r='4' fill='#c040a0'/>"
            "<circle cx='14' cy='40' r='4' fill='#c040a0'/>"
            "<circle cx='14' cy='20' r='4' fill='#c040a0'/>"
            "<circle cx='32' cy='30' r='4' fill='#c040a0'/>"
            "</svg>");
    });

    // WebSocket
    s_ws.onEvent(on_ws_event);
    s_server.addHandler(&s_ws);

    // Hotend
    s_server.on("/api/hotend", HTTP_GET, api_hotend_get);
    s_server.on("/api/hotend/clear_fault", HTTP_POST, api_hotend_clear_fault);
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/hotend/target",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_hotend_set_target(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/hotend/pid",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_hotend_set_pid(r, b); }));

    // Wägezelle
    s_server.on("/api/loadcell", HTTP_GET, api_loadcell_get);
    s_server.on("/api/loadcell/tare", HTTP_POST, [](AsyncWebServerRequest *r){ api_loadcell_tare(r); });
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/loadcell/cal",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_loadcell_cal(r, b); }));

    // Motor
    s_server.on("/api/motor/status", HTTP_GET, api_motor_status);
    s_server.on("/api/motor/config", HTTP_GET, api_motor_get_config);
    s_server.on("/api/motor/stop",   HTTP_POST, [](AsyncWebServerRequest *r){ api_motor_stop(r); });
    s_server.on("/api/motor/esteps", HTTP_GET, api_motor_get_esteps);
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/move",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_move(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/move_dist",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_move_dist(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/current",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_set_current(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/microstep",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_set_microstep(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/stealthchop",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_set_stealthchop(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/interpolation",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_set_interpolation(r, b); }));
    s_server.on("/api/motor/dir", HTTP_GET, api_motor_get_dir);
    {
        auto *h = new AsyncCallbackJsonWebHandler("/api/motor/dir",
            [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_set_dir(r, b); });
        h->setMethod(HTTP_POST);
        s_server.addHandler(h);
    }
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/esteps",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_set_esteps(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/cal/start",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_cal_start(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/motor/cal/apply",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_motor_cal_apply(r, b); }));

    // Datenlogger
    s_server.on("/api/datalog/mount", HTTP_POST, [](AsyncWebServerRequest *r){
        bool ok = datalog_mount_sd();
        r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"mount failed\"}");
    });
    s_server.on("/api/datalog/status", HTTP_GET, api_datalog_status);
    s_server.on("/api/datalog/stop",   HTTP_POST, [](AsyncWebServerRequest *r){ api_datalog_stop(r); });
    s_server.on("/api/datalog/filelist", HTTP_GET, api_datalog_files);
    s_server.on("/api/datalog/sdinfo", HTTP_GET, api_datalog_sdinfo);
    s_server.on("^\\/api\\/datalog\\/files\\/(.+)$", HTTP_GET, api_datalog_download);
    s_server.on("^\\/api\\/datalog\\/files\\/(.+)$", HTTP_DELETE,
                [](AsyncWebServerRequest *r){ api_datalog_delete(r); });
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/datalog/start",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_datalog_start(r, b); }));
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/datalog/delete_all",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_datalog_delete_all(r, b); }));

    // Sequencer – Presets
    s_server.on("/api/preset-list", HTTP_GET, api_seq_presets_get);
    {
        auto *h = new AsyncCallbackJsonWebHandler("/api/preset-save",
            [](AsyncWebServerRequest *r, JsonVariant &b){ api_seq_presets_save(r, b); });
        h->setMethod(HTTP_POST);
        h->setMaxContentLength(4096);
        s_server.addHandler(h);
    }
    {
        auto *h = new AsyncCallbackJsonWebHandler("/api/preset-del",
            [](AsyncWebServerRequest *r, JsonVariant &b){ api_seq_presets_delete(r, b); });
        h->setMethod(HTTP_POST);
        h->setMaxContentLength(4096);
        s_server.addHandler(h);
    }
    s_server.on("/api/sequence",       HTTP_GET,  api_seq_get);
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/sequence/start",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_seq_start(r, b); }));
    s_server.on("/api/sequence/stop",  HTTP_POST, [](AsyncWebServerRequest *r){ api_seq_stop(r); });
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/sequence/add",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_seq_add(r, b); }));
    s_server.on("/api/sequence/clear",  HTTP_POST, [](AsyncWebServerRequest *r){ api_seq_clear(r); });
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/sequence/delete",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_seq_delete(r, b); }));

    // Browser-Zeit setzen (Fallback wenn kein NTP verfügbar)
    // Nur Epoch merken – settimeofday() wird im ws_push_task ausgeführt
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/time/set",
        [](AsyncWebServerRequest *r, JsonVariant &b){
            time_t epoch = (time_t)(b["epoch"].as<unsigned long>());
            if (epoch > 1600000000UL) {
                s_pending_epoch = epoch;
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"error\":\"invalid epoch\"}");
            }
        }));

    // WiFi
    s_server.on("/api/wifi/scan", HTTP_GET, api_wifi_scan);
    s_server.on("/api/wifi/status", HTTP_GET, api_wifi_status);
    s_server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi/save",
        [](AsyncWebServerRequest *r, JsonVariant &b){ api_wifi_save(r, b); }));
    s_server.on("/api/wifi/restart", HTTP_POST, [](AsyncWebServerRequest *r){
        r->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        ESP.restart();
    });

    // Statische Dateien aus LittleFS (NACH allen API-Routen!)
    s_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    s_server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "Not Found");
    });
}

// ── Öffentliche API ──────────────────────────────────────────

void webui_init() {
    // NVS für WiFi-Einstellungen laden
    s_prefs.begin("wifi", false);

    // Defaults aus config.h in RAM laden
    strncpy(s_ssid_sta, WIFI_STA_SSID, 32);
    strncpy(s_pass_sta, WIFI_STA_PASSWORD, 32);
    strncpy(s_ssid_ap,  WIFI_AP_SSID, 32);
    strncpy(s_pass_ap,  WIFI_AP_PASSWORD, 32);

    // Falls in NVS gespeichert, überschreiben
    if (s_prefs.isKey("sta_ssid")) s_prefs.getString("sta_ssid").toCharArray(s_ssid_sta, 33);
    if (s_prefs.isKey("sta_pass")) s_prefs.getString("sta_pass").toCharArray(s_pass_sta, 33);
    if (s_prefs.isKey("ap_ssid"))  s_prefs.getString("ap_ssid").toCharArray(s_ssid_ap, 33);
    if (s_prefs.isKey("ap_pass"))  s_prefs.getString("ap_pass").toCharArray(s_pass_ap, 33);

    Serial.printf("[WIFI] NVS geladen: STA='%s' AP='%s'\n", s_ssid_sta, s_ssid_ap);

    // STA-Modus versuchen (wie CG_scale)
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_ssid_sta, s_pass_sta);
    Serial.printf("[WIFI] Verbinde mit '%s'...\n", s_ssid_sta);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        s_wifi_sta_mode = true;
        Serial.printf("\n[WIFI] STA verbunden! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        // Fallback: AP-Modus (wie CG_scale)
        s_wifi_sta_mode = false;
        Serial.printf("\n[WIFI] STA fehlgeschlagen – starte AP: %s\n", s_ssid_ap);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
        WiFi.softAP(s_ssid_ap, s_pass_ap, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CONN);

        // AP-Optimierungen: TX-Power hoch, AMPDU aktiviert
        esp_wifi_set_max_tx_power(WIFI_AP_TX_POWER);
        // Protokoll auf 802.11n beschränken (weniger Overhead)
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        // Bandwidth auf 40 MHz (doppelter Durchsatz vs Default 20 MHz)
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);

        Serial.printf("[WIFI] AP IP: %s (Ch%d, TxPwr=%d)\n",
                      WiFi.softAPIP().toString().c_str(), WIFI_AP_CHANNEL, WIFI_AP_TX_POWER);
    }

    // LittleFS mounten (Frontend-Dateien)
    if (!LittleFS.begin()) {
        Serial.println("[WEBUI] LittleFS nicht gefunden – Web-Frontend fehlt!");
    }

    register_routes();
    s_server.begin();
    Serial.printf("[WEBUI] HTTP-Server läuft auf Port %d\n", WEBUI_PORT);

    // WebSocket-Push-Task starten
    xTaskCreatePinnedToCore(ws_push_task, "ws_push", TASK_STACK_WS_PUSH,
                            nullptr, TASK_PRIO_WS_PUSH, nullptr, CORE_WIFI);

    // Datalog-Start-Worker (führt datalog_start außerhalb async_tcp aus)
    xTaskCreatePinnedToCore(datalog_start_worker, "dl_start", 4096,
                            nullptr, 3, &s_dl_start_task, CORE_REALTIME);
}

void webui_notify_clients(const char *json) {
    if (s_ws.count() > 0) s_ws.textAll(json);
}
