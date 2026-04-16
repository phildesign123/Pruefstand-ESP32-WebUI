# Modul-Spezifikation: Web-UI

> **Version:** 0.1 (Entwurf)
> **Datum:** 2026-03-17
> **Plattform:** ESP32-WROVER-E · ESP-IDF (FreeRTOS)

---

## 1  Zweck

Das Web-UI ist die zentrale Bedienoberfläche der Steuerung. Es wird als
Single-Page-Application (SPA) auf dem ESP32 gehostet und ist über jeden
modernen Browser im lokalen Netzwerk erreichbar.

Das UI gliedert sich in zwei Hauptbereiche, erreichbar über eine Navigationsleiste:

1. **Dashboard** — Prozessüberwachung (Temperatur, Motor, Kraft, Manuelle Aufzeichnung), Live-Diagramm, Messreihen, SD-Dateien
2. **Einstellungen** — Kalibrierung (Wägezelle, E-Steps), TMC2208-Konfiguration, SD-Speicherinfo
3. **WiFi** — WiFi-Konfiguration (STA/AP)

---

## 2  Architektur-Übersicht

### 2.1  Technologie-Stack

| Schicht        | Technologie                          | Bemerkung                          |
| -------------- | ------------------------------------ | ---------------------------------- |
| HTTP-Server    | ESP-IDF `httpd` (ESP HTTP Server)    | REST-API + statische Dateien       |
| WebSocket      | ESP-IDF `httpd_ws`                   | Live-Daten (10 Hz)                 |
| Frontend       | Vanilla HTML/CSS/JS (kein Framework) | Minimaler Footprint für PSRAM      |
| Diagramm       | Lightweight Chart-Library (µPlot o.ä.)| ≈ 30 KB gzipped                   |
| Speicher       | SPIFFS oder LittleFS Partition       | Frontend-Dateien im Flash          |

### 2.2  Warum kein Framework (React, Vue)?

Der ESP32-WROVER-E hat 4 MB Flash und 8 MB PSRAM. Die Frontend-Dateien
werden aus einer Flash-Partition (SPIFFS/LittleFS) ausgeliefert.
Ein Framework-Bundle würde 200–500 KB belegen — Vanilla JS bleibt unter 50 KB
(gzipped) und lädt schneller über die Wi-Fi-Verbindung.

### 2.3  Kommunikations-Architektur

```
┌──────────────────────────────────────────────────────────┐
│                       Browser                             │
│                                                           │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────┐ │
│  │  Dashboard   │  │ Einstellungen│  │  Messreihen-     │ │
│  │  (Seite 1)  │  │  (Seite 2)  │  │  Verwaltung      │ │
│  └──────┬──────┘  └──────┬──────┘  └────────┬─────────┘ │
│         │                │                   │            │
│         ▼                ▼                   ▼            │
│  ┌─────────────────────────────────────────────────────┐ │
│  │              WebSocket (10 Hz)    REST-API           │ │
│  │              ws://esp32/ws        http://esp32/api/* │ │
│  └──────────────────────┬──────────────────────────────┘ │
└─────────────────────────┼────────────────────────────────┘
                          │ Wi-Fi (STA oder AP)
                          ▼
┌──────────────────────────────────────────────────────────┐
│                      ESP32                                │
│                                                           │
│  ┌──────────────┐  ┌────────────┐  ┌──────────────────┐ │
│  │ HTTP-Server   │  │ WebSocket  │  │ Messreihen-      │ │
│  │ (REST-API)    │  │ (Push)     │  │ Sequencer        │ │
│  └──────┬───────┘  └─────┬──────┘  └────────┬─────────┘ │
│         │                │                   │            │
│         ▼                ▼                   ▼            │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  hotend_pid  │  load_cell  │  motor  │  datalog     │ │
│  └─────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

---

## 3  Seite 1 — Dashboard

### 3.1  Layout-Übersicht

```
┌──────────────────────────────────────────────────────────┐
│  [Logo]        DASHBOARD        EINSTELLUNGEN        [≡] │  ← NavBar
├──────────────────────────────────────────────────────────┤
│                                                           │
│  ┌─────────────────────┐  ┌────────────────────────────┐ │
│  │   TEMPERATUR        │  │   KRAFT                    │ │
│  │                     │  │                            │ │
│  │   ██ 200.3 °C       │  │   ██ 24.7 g               │ │
│  │   Soll: 200.0 °C    │  │                            │ │
│  │   Duty: 42 %        │  │   [TARIEREN]               │ │
│  │                     │  │                            │ │
│  │   Soll: [___] [SET] │  │                            │ │
│  └─────────────────────┘  └────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   MOTOR — Manuelle Steuerung                        │ │
│  │                                                     │ │
│  │   ◄◄       ◄      ◄     ►     ►      ►►            │ │
│  │   -10mm   -5mm  -2mm  +1mm  +2mm  +5mm  +10mm      │ │
│  │                                                     │ │
│  │   Geschw.: [___] mm/s        [STOP]                 │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   LIVE-DIAGRAMM (10 Hz)                    [30s ▼]  │ │
│  │                                                     │ │
│  │   ── Temperatur (°C)                                │ │
│  │   ── Kraft (g)                                      │ │
│  │   ── Geschwindigkeit (mm/s)                         │ │
│  │   ┌─────────────────────────────────────────────┐   │ │
│  │   │                                             │   │ │
│  │   │         ~~~~~~~~~~~~~~~                     │   │ │
│  │   │     ~~~                ~~~                  │   │ │
│  │   │  ~~                       ~~~~~             │   │ │
│  │   │                                             │   │ │
│  │   └─────────────────────────────────────────────┘   │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   MESSREIHEN                                        │ │
│  │                                                     │ │
│  │   Nr │ Temp (°C) │ Speed (mm/s) │ Dauer (s) │ Status│ │
│  │   ───┼───────────┼──────────────┼───────────┼───────│ │
│  │    1 │   200     │     3.0      │    60     │  ✓    │ │
│  │    2 │   210     │     3.0      │    60     │  ►    │ │
│  │    3 │   220     │     5.0      │    30     │  ·    │ │
│  │                                                     │ │
│  │   Temp: [___] Speed: [___] Dauer: [___] [HINZUFÜGEN]│ │
│  │                                                     │ │
│  │   [▶ STARTEN]  [⏹ STOPPEN]  [🗑 ALLE LÖSCHEN]      │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
└──────────────────────────────────────────────────────────┘
```

### 3.2  Temperatur-Anzeige

| Element            | Beschreibung                                          |
| ------------------ | ----------------------------------------------------- |
| Ist-Temperatur     | Große Zahl, aktualisiert via WebSocket (10 Hz)        |
| Soll-Temperatur    | Anzeige + Eingabefeld zum Ändern                      |
| Duty-Cycle         | Prozentuale Heizleistung                              |
| Farbindikator      | Grün: ±2 °C vom Soll, Gelb: ±10 °C, Rot: >10 °C ab  |
| Soll setzen        | Eingabefeld + Button, sendet `POST /api/hotend/target`|

### 3.3  Kraft-Anzeige

| Element            | Beschreibung                                          |
| ------------------ | ----------------------------------------------------- |
| Aktuelles Gewicht  | Große Zahl, aktualisiert via WebSocket (10 Hz)        |
| Tarieren-Button    | Sendet `POST /api/loadcell/tare`                      |
| Einheit            | Gramm (g), ggf. umschaltbar auf Newton (N)            |

### 3.4  Motor — Manuelle Steuerung

Sieben Buttons für Jog-Bewegungen in festen Distanzen:

| Button | Distanz  | Richtung   | API-Aufruf                                        |
| ------ | -------- | ---------- | ------------------------------------------------- |
| `◄◄`   | −10 mm   | Reverse    | `POST /api/motor/move_dist {speed, dist:10, rev}` |
| `◄`    | −5 mm    | Reverse    | `POST /api/motor/move_dist {speed, dist:5, rev}`  |
| `◄`    | −2 mm    | Reverse    | `POST /api/motor/move_dist {speed, dist:2, rev}`  |
| `►`    | +1 mm    | Forward    | `POST /api/motor/move_dist {speed, dist:1, fwd}`  |
| `►`    | +2 mm    | Forward    | `POST /api/motor/move_dist {speed, dist:2, fwd}`  |
| `►`    | +5 mm    | Forward    | `POST /api/motor/move_dist {speed, dist:5, fwd}`  |
| `►►`   | +10 mm   | Forward    | `POST /api/motor/move_dist {speed, dist:10, fwd}` |

- Die Geschwindigkeit wird über ein Eingabefeld eingestellt (Default: 3 mm/s).
- Ein **Start**-Button (grün) startet den Motor dauerhaft mit der eingegebenen Geschwindigkeit (`POST /api/motor/move {speed, duration_s: 3600}`).
- Ein **STOP**-Button (rot) bricht die laufende Bewegung sofort ab (`POST /api/motor/stop`).
- Beide Buttons sind gleich breit (100px).
- Die aktuelle Geschwindigkeit wird als große Zahl mit `mm/s` daneben (nowrap) angezeigt.

### 3.5  Live-Diagramm

| Eigenschaft        | Wert                                       |
| ------------------ | ------------------------------------------ |
| Update-Rate        | 10 Hz (via WebSocket)                      |
| Kurven             | Temperatur (°C), Kraft (g), Geschwindigkeit (mm/s) |
| Zeitachse          | Konfigurierbar: 10 s, 30 s, 60 s, 5 min   |
| Y-Achsen           | Dual-Achse: Links = °C, Rechts = g / mm/s  |
| Datenpunkte im RAM | max. 3000 (5 min × 10 Hz)                  |
| Library            | µPlot (≈ 30 KB) oder Chart.js light        |

Das Diagramm zeigt nur die letzten N Sekunden als Sliding Window.
Ältere Daten werden verworfen (die vollständigen Daten liegen auf der SD-Karte).

Ein Dropdown erlaubt die Auswahl des Zeitfensters. Einzelne Kurven können
per Klick auf die Legende ein-/ausgeblendet werden.

### 3.6  Messreihen-Verwaltung

#### Konzept

Eine Messreihe besteht aus einer oder mehreren **Sequenzen**, die
nacheinander abgearbeitet werden. Jede Sequenz definiert:

| Parameter        | Typ    | Einheit | Beschreibung                        |
| ---------------- | ------ | ------- | ----------------------------------- |
| `temperature_c`  | float  | °C      | Soll-Temperatur für diese Sequenz   |
| `speed_mm_s`     | float  | mm/s    | Extrusionsgeschwindigkeit            |
| `duration_s`     | float  | s       | Dauer der Sequenz                    |

#### Ablauf einer Messreihe

```
Sequenz 1          Sequenz 2          Sequenz 3
┌──────────┐       ┌──────────┐       ┌──────────┐
│ T=200°C  │       │ T=210°C  │       │ T=220°C  │
│ v=3 mm/s │──────►│ v=3 mm/s │──────►│ v=5 mm/s │
│ t=60 s   │       │ t=60 s   │       │ t=30 s   │
└──────────┘       └──────────┘       └──────────┘
     │                  │                  │
     ▼                  ▼                  ▼
  SD-Logging         SD-Logging         SD-Logging
  läuft sofort       läuft weiter       bis Ende
```

Zwischen den Sequenzen:
1. Soll-Temperatur wird auf den neuen Wert gesetzt.
2. Das System wartet, bis die Temperatur innerhalb ±2 °C des Sollwerts
   stabilisiert ist (Timeout konfigurierbar, Default: 120 s).
3. Erst dann startet die nächste Sequenz (Motor + Timer).
4. Die Datenaufzeichnung auf der SD-Karte läuft **sofort** ab Start
   der ersten Sequenz — auch während der Aufheizphasen zwischen den Sequenzen.

#### UI-Elemente

| Element                | Beschreibung                                          |
| ---------------------- | ----------------------------------------------------- |
| Sequenz-Tabelle        | Zeigt alle Sequenzen mit Status (ausstehend, aktiv, fertig) |
| Eingabezeile           | Temperatur, Geschwindigkeit, Dauer → Button „Hinzufügen" |
| Reihenfolge ändern     | Drag & Drop oder Pfeiltasten (↑↓)                    |
| Sequenz löschen        | Löschen-Button pro Zeile (nur im Zustand „ausstehend") |
| Starten                | Startet die gesamte Messreihe von Sequenz 1           |
| Stoppen                | Bricht die laufende Messreihe ab, stoppt Motor + Heizung |
| Alle löschen           | Entfernt alle Sequenzen aus der Tabelle               |
| Preset-Dropdown        | Dropdown zum Laden einer gespeicherten Messreihe (SD-Karte) |
| Preset-Name-Eingabe    | Textfeld zur Benennung einer Messreihe                |
| Messreihe speichern    | Speichert aktuelle Sequenzen als benanntes Preset auf SD-Karte (`/presets.json`) |
| Preset löschen (🗑)    | Löscht das ausgewählte Preset von der SD-Karte        |

#### Status-Indikatoren

| Symbol | Bedeutung                                    |
| ------ | -------------------------------------------- |
| `·`    | Ausstehend (noch nicht gestartet)            |
| `⏳`   | Aufheizen (Temperatur wird angefahren)       |
| `►`    | Aktiv (Motor extrudiert, Daten werden geloggt) |
| `✓`    | Abgeschlossen                                |
| `✗`    | Fehler (Thermal Runaway, Timeout, etc.)      |

### 3.7  Manuelle Aufzeichnung (Dashboard-Karte)

Vierte Karte im Dashboard-Grid neben Temperatur, Motor und Kraft.

| Element           | Beschreibung                                                     |
| ----------------- | ---------------------------------------------------------------- |
| Status-Badge      | Zeigt `ready to record` oder blinkendes `recording` (rot, 1 Hz Polling) |
| Dateiname-Eingabe | Optionaler Name für die Aufzeichnungsdatei                       |
| Start-Button      | Startet Aufzeichnung (`POST /api/datalog/start {interval_ms: 100}`) |
| Stop-Button       | Stoppt Aufzeichnung (`POST /api/datalog/stop`)                   |

Der Status wird jede Sekunde via `GET /api/datalog/status` aktualisiert und zeigt
den Aufzeichnungsstatus aller Quellen (manuell, Messreihe, etc.).

### 3.8  SD-Aufzeichnung (Dateiliste)

Zeigt die auf der SD-Karte gespeicherten CSV-Dateien.

| Element              | Beschreibung                                          |
| -------------------- | ----------------------------------------------------- |
| Dateiliste           | Alle `.csv`-Dateien mit Größe, Download- und Lösch-Button |
| Alle Dateien löschen | Löscht alle Dateien auf der SD-Karte                  |

Die Dateiliste wird beim Seitenladen und nach Start/Stop einer Aufzeichnung aktualisiert.

### 3.9  Layout

Das Dashboard verwendet ein 4-Spalten-Grid (`grid4`) für die oberen Karten
(Temperatur, Motor, Kraft, Manuelle Aufzeichnung) mit `max-width: 1500px`.

| Breakpoint  | Layout                |
| ----------- | --------------------- |
| > 900px     | 4 Spalten             |
| 500–900px   | 2 Spalten             |
| < 500px     | 1 Spalte              |

### 3.10  Design / Hintergrund

| Modus | Farbverlauf (135°)                                      |
| ----- | ------------------------------------------------------- |
| Light | Lavendel (#e0e7ff) → Rosa (#fce7f3) → Hellblau (#dbeafe) |
| Dark  | Dunkelblau (#0f172a) → Indigo (#1e1b4b) → Dunkelblau (#0f172a) |

---

## 4  Seite 2 — Einstellungen

### 4.1  Layout-Übersicht

```
┌──────────────────────────────────────────────────────────┐
│  [Logo]        DASHBOARD        EINSTELLUNGEN        [≡] │  ← NavBar
├──────────────────────────────────────────────────────────┤
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   WÄGEZELLE                                         │ │
│  │                                                     │ │
│  │   Aktuell: 24.7 g  │  Rohwert: 1482512             │ │
│  │   Kalibriert: ✓    │  Faktor: 1823.45              │ │
│  │                                                     │ │
│  │   [TARIEREN]                                        │ │
│  │                                                     │ │
│  │   Kalibrierung:                                     │ │
│  │   Referenzgewicht: [___] g  [KALIBRIEREN]           │ │
│  │                                                     │ │
│  │   Status: "Referenzgewicht auflegen, dann klicken"  │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   E-STEPS KALIBRIERUNG                              │ │
│  │                                                     │ │
│  │   Aktuell: 93.00 Steps/mm  (DEFAULT)                │ │
│  │                                                     │ │
│  │   Schritt 1: Filament bei 100 mm markieren          │ │
│  │   Referenzstrecke: [100] mm  Speed: [3.0] mm/s      │ │
│  │   [EXTRUDIEREN]                                     │ │
│  │                                                     │ │
│  │   Schritt 2: Restlänge messen und eingeben          │ │
│  │   Verbleibend: [___] mm  [ÜBERNEHMEN]               │ │
│  │                                                     │ │
│  │   Manuell setzen: [___] Steps/mm  [SETZEN]          │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   TMC2208 — MOTOR-KONFIGURATION                     │ │
│  │                                                     │ │
│  │   Fahrstrom:     [800 ] mA    [SETZEN]              │ │
│  │   Haltestrom:    [400 ] mA    [SETZEN]              │ │
│  │   Mikroschritt:  [ 16 ▼]     [SETZEN]              │ │
│  │   StealthChop:   [✓ AN ]     [SETZEN]              │ │
│  │   Interpolation: [✓ AN ]     [SETZEN]              │ │
│  │                                                     │ │
│  │   ── TMC2208 Status ──                              │ │
│  │   Treiber-Temp:  OK                                 │ │
│  │   OT Warning:    Nein                               │ │
│  │   OT Shutdown:   Nein                               │ │
│  │   Open Load A/B: Nein / Nein                        │ │
│  │   Kurzschluss:   Nein                               │ │
│  │                                                     │ │
│  │   [STATUS AKTUALISIEREN]                            │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │   SD-KARTE & DATEN                                  │ │
│  │                                                     │ │
│  │   Speicher: 1.84 GB frei / 1.86 GB                  │ │
│  │                                                     │ │
│  │   Dateien:                                          │ │
│  │   ┌──────────────────────────────┬───────┬────────┐ │ │
│  │   │ Dateiname                    │ Größe │ Aktion │ │ │
│  │   ├──────────────────────────────┼───────┼────────┤ │ │
│  │   │ log_20260317_130000.csv      │ 1.2MB │ ⬇ 🗑  │ │ │
│  │   │ log_20260317_140000.csv      │ 856KB │ ⬇ 🗑  │ │ │
│  │   │ log_20260317_143000.csv (●)  │ 42KB  │ ⬇ 🗑  │ │ │
│  │   └──────────────────────────────┴───────┴────────┘ │ │
│  │                                                     │ │
│  │   [ALLE LÖSCHEN]                                    │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
└──────────────────────────────────────────────────────────┘
```

### 4.2  Wägezelle — Kalibrierung & Tarierung

| Aktion          | Ablauf im UI                                                |
| --------------- | ----------------------------------------------------------- |
| Tarieren        | Button klicken → Spinner (1 s) → „Tariert! Gewicht: 0.0 g" |
| Kalibrieren     | Gewicht eingeben → Button → Spinner (1 s) → „Kalibriert! Faktor: XXXX" |

Der aktuelle Rohwert und Kalibrierfaktor werden live angezeigt,
damit der Nutzer den Fortschritt nachvollziehen kann.

### 4.3  E-Steps-Kalibrierung

Zweistufiger Wizard, entsprechend der Motor-Spec (Abschnitt 6):

| Phase   | UI-Zustand                                                     |
| ------- | -------------------------------------------------------------- |
| Idle    | Schritt-1-Felder aktiv, Schritt-2-Felder deaktiviert          |
| Phase 1 | „Extrudieren"-Button gedrückt → Fortschrittsbalken → „Fertig, bitte messen" |
| Phase 2 | Schritt-2-Feld aktiv → Restlänge eingeben → „Übernehmen"      |
| Fertig  | Neuer E-Steps-Wert angezeigt, in NVS gespeichert              |

### 4.4  TMC2208-Konfiguration

| Parameter        | UI-Element      | Wertebereich              | API-Endpunkt                  |
| ---------------- | --------------- | ------------------------- | ----------------------------- |
| Fahrstrom        | Eingabefeld     | 100 – 2000 mA            | `POST /api/motor/current`     |
| Haltestrom       | Eingabefeld     | 0 – 2000 mA              | `POST /api/motor/current`     |
| Mikroschritt     | Dropdown        | 1, 2, 4, 8, 16, 32, 64, 128, 256 | `POST /api/motor/microstep` |
| StealthChop      | Toggle          | An / Aus                  | `POST /api/motor/stealthchop` |
| Interpolation    | Toggle          | An / Aus                  | `POST /api/motor/interpolation` |

Der TMC2208-Status wird auf Anfrage gelesen (nicht automatisch),
um den UART-Bus nicht unnötig zu belasten.

### 4.5  SD-Karte & Datei-Download

Dateiliste mit Download- und Lösch-Buttons pro Zeile.
Aktiv beschriebene Dateien werden mit einem Indikator (●) markiert.
Download löst einen Browser-Download aus (`Content-Disposition: attachment`).

---

## 5  WebSocket-Protokoll (Live-Daten)

### 5.1  Verbindung

```
ws://esp32.local/ws
```

Der Client baut beim Laden des Dashboards eine WebSocket-Verbindung auf.
Bei Verbindungsabbruch wird automatisch alle 2 Sekunden ein Reconnect versucht.

### 5.2  Server → Client (10 Hz)

JSON-Nachrichten im 100-ms-Takt:

```json
{
  "t": 142530,
  "temp": 200.3,
  "temp_target": 200.0,
  "duty": 0.42,
  "weight": 24.7,
  "speed": 3.0,
  "motor": 1,
  "seq": 2,
  "seq_state": "running"
}
```

| Feld          | Typ    | Beschreibung                              |
| ------------- | ------ | ----------------------------------------- |
| `t`           | uint32 | Laufzeit in ms                            |
| `temp`        | float  | Ist-Temperatur (°C)                       |
| `temp_target` | float  | Soll-Temperatur (°C)                      |
| `duty`        | float  | Heiz-Duty (0.0 – 1.0)                    |
| `weight`      | float  | Gewicht / Kraft (g)                       |
| `speed`       | float  | Aktuelle Geschwindigkeit (mm/s)           |
| `motor`       | uint8  | Motor aktiv (0/1)                         |
| `seq`         | int    | Aktive Sequenz-Nummer (−1 = keine)        |
| `seq_state`   | string | `idle`, `heating`, `running`, `done`, `error` |
| `seq_remain`  | float  | Verbleibende Sekunden der aktiven Sequenz  |
| `dl_state`    | string | Datalog-Status: `idle`, `recording`, `paused`, `error`, `stopping` |

### 5.3  Client → Server (Events)

Der Client kann über den WebSocket auch Befehle senden:

```json
{"cmd": "set_target", "value": 210.0}
{"cmd": "fan_off_timed", "seconds": 20}
{"cmd": "motor_jog", "dist": 5.0, "dir": "fwd", "speed": 3.0}
{"cmd": "motor_stop"}
{"cmd": "tare"}
```

Das Kommando `fan_off_timed` schaltet den Hotend-Lüfter für die angegebene
Dauer (max. 300 s) komplett aus und kehrt danach automatisch in den
Temperatur-Automodus zurück. Nützlich für kurzzeitige Messungen, bei denen
der Luftstrom die Wägezelle stören würde. Button in der Temperatur-Karte
(Dashboard): `Lüfter 20s AUS`.

Diese werden alternativ zu den REST-Endpunkten akzeptiert.
Für Aktionen, die eine Antwort erfordern (z.B. Kalibrierung),
wird weiterhin die REST-API verwendet.

### 5.4  Bandbreite

Bei 10 Hz und ≈ 150 Byte pro Nachricht: **1,5 KB/s** —
vernachlässigbar für Wi-Fi.

---

## 6  Messreihen-Sequencer (Backend)

### 6.1  Zweck

Der Sequencer ist die Backend-Logik, die eine Messreihe (Liste von Sequenzen)
automatisch abarbeitet. Er koordiniert Hotend-PID, Motor und Datalog-Modul.

### 6.2  Zustandsmaschine

```
          ┌──────┐
          │ IDLE │◄──────────────────────────────┐
          └──┬───┘                               │
   Start     │                                   │ Alle Sequenzen
             ▼                                   │ abgearbeitet
       ┌───────────┐                             │
   ┌──►│ HEATING   │  T_ist < T_soll ± 2°C      │
   │   └─────┬─────┘                             │
   │         │ Temperatur stabil                 │
   │         ▼                                   │
   │   ┌───────────┐                             │
   │   │ RUNNING   │  Motor + Timer aktiv        │
   │   └─────┬─────┘                             │
   │         │ duration_s abgelaufen             │
   │         ▼                                   │
   │   ┌───────────┐                             │
   │   │ NEXT_SEQ  │  Nächste Sequenz laden      │
   │   └─────┬─────┘                             │
   │         │                                   │
   │         ├── Weitere Sequenz vorhanden ──►───┘ (→ HEATING)
   │         │
   │         └── Keine weitere ──────────────────┘ (→ IDLE)
   │
   │   ┌───────────┐
   └───│ ERROR     │  Thermal Runaway, Timeout, etc.
       └───────────┘
```

### 6.3  Datenaufzeichnung

Die SD-Karten-Aufzeichnung wird beim Start der Messreihe aktiviert
(`datalog_start()`) und läuft **ununterbrochen** bis zum Ende der
letzten Sequenz — auch während der Aufheizphasen zwischen den Sequenzen.

So sind die Temperaturrampen zwischen den Sequenzen ebenfalls dokumentiert.

> **Wichtig:** `datalog_start()` darf **nicht** im `async_tcp`-Kontext
> (HTTP-Handler) aufgerufen werden, da die SPI-Mutex-Wartezeit und
> SD-Kartenoperationen den Task-Watchdog auslösen können.
> Stattdessen wird der Aufruf im `sequencer_task` (Core 1) ausgeführt
> bzw. bei manueller Aufzeichnung in einem eigenen Kurzzeit-Task.

### 6.4  Konfigurierbare Parameter

| Parameter               | Default | Kconfig-Key                    |
| ----------------------- | ------- | ------------------------------ |
| Temperatur-Toleranz     | ±2,0 °C | `SEQ_TEMP_TOLERANCE`           |
| Stabilisierungs-Zeit    | 10 s    | `SEQ_TEMP_STABLE_TIME_S`       |
| Aufheiz-Timeout         | 120 s   | `SEQ_HEATING_TIMEOUT_S`        |
| Log-Intervall Messreihe | 100 ms  | `SEQ_LOG_INTERVAL_MS`          |

Das Log-Intervall der Messreihe (100 ms = 10 Hz) ist unabhängig vom
normalen Datalog-Intervall (1 Hz). Während einer Messreihe wird
automatisch auf das höhere Intervall umgeschaltet.

---

## 7  REST-API — Vollständige Endpunkt-Übersicht

### 7.1  Hotend / Temperatur

| Methode | Endpunkt                  | Beschreibung             | Body / Response                |
| ------- | ------------------------- | ------------------------ | ------------------------------ |
| GET     | `/api/hotend`             | Ist, Soll, Duty, Fehler | `{temp, target, duty, fault}`  |
| POST    | `/api/hotend/target`      | Soll-Temperatur setzen   | `{"target_c": 200.0}`         |
| POST    | `/api/hotend/pid`         | PID-Parameter setzen     | `{"kp":15, "ki":1, "kd":40}`  |
| POST    | `/api/hotend/clear_fault` | Fehler quittieren        | —                              |

### 7.2  Wägezelle

| Methode | Endpunkt                  | Beschreibung             | Body / Response                |
| ------- | ------------------------- | ------------------------ | ------------------------------ |
| GET     | `/api/loadcell`           | Gewicht, Rohwert, Status | `{weight_g, raw, calibrated}` |
| POST    | `/api/loadcell/tare`      | Tarieren                 | —                              |
| POST    | `/api/loadcell/cal`       | Kalibrieren              | `{"weight_g": 500.0}`         |
| GET     | `/api/loadcell/cal`       | Kalibrier-Info           | `{factor, valid}`             |

### 7.3  Motor

| Methode | Endpunkt                  | Beschreibung              | Body                            |
| ------- | ------------------------- | ------------------------- | ------------------------------- |
| GET     | `/api/motor/status`       | Status, Speed, E-Steps    | —                               |
| POST    | `/api/motor/move`         | Bewegen (speed + dauer)   | `{speed, duration_s, dir}`      |
| POST    | `/api/motor/move_dist`    | Bewegen (speed + distanz) | `{speed, distance_mm, dir}`     |
| POST    | `/api/motor/stop`         | Sofortstopp               | —                               |
| POST    | `/api/motor/current`      | Strom setzen              | `{run_ma, hold_ma}`            |
| POST    | `/api/motor/microstep`    | Mikroschritt setzen       | `{microstep: 16}`              |
| POST    | `/api/motor/stealthchop`  | StealthChop ein/aus       | `{enable: true}`               |
| POST    | `/api/motor/interpolation`| Interpolation ein/aus     | `{enable: true}`               |
| GET     | `/api/motor/esteps`       | E-Steps abfragen          | `{steps_per_mm, valid}`         |
| POST    | `/api/motor/esteps`       | E-Steps manuell setzen    | `{steps_per_mm: 101.09}`       |
| POST    | `/api/motor/cal/start`    | E-Steps-Kal. Phase 1      | `{distance_mm, speed}`         |
| POST    | `/api/motor/cal/apply`    | E-Steps-Kal. Phase 2      | `{remaining_mm}`               |

### 7.4  Datenlogger / SD-Karte

| Methode | Endpunkt                     | Beschreibung              |
| ------- | ---------------------------- | ------------------------- |
| GET     | `/api/datalog/status`        | Aufzeichnungsstatus       |
| POST    | `/api/datalog/start`         | Aufzeichnung starten      |
| POST    | `/api/datalog/stop`          | Aufzeichnung stoppen      |
| GET     | `/api/datalog/files`         | Dateiliste                |
| GET     | `/api/datalog/files/<name>`  | Datei herunterladen       |
| DELETE  | `/api/datalog/files/<name>`  | Datei löschen             |
| DELETE  | `/api/datalog/files`         | Alle Dateien löschen      |
| GET     | `/api/datalog/sdinfo`        | SD-Karten-Info            |

### 7.5  Messreihen-Sequencer

| Methode | Endpunkt                  | Beschreibung              | Body                                 |
| ------- | ------------------------- | ------------------------- | ------------------------------------ |
| GET     | `/api/sequence`           | Alle Sequenzen + Status   | —                                    |
| POST    | `/api/sequence/add`       | Sequenz hinzufügen        | `{temp_c, speed_mm_s, duration_s}`   |
| POST    | `/api/sequence/reorder`   | Reihenfolge ändern        | `{order: [2, 0, 1]}`                |
| DELETE  | `/api/sequence/<index>`   | Einzelne Sequenz löschen  | —                                    |
| DELETE  | `/api/sequence`           | Alle Sequenzen löschen    | —                                    |
| POST    | `/api/sequence/start`     | Messreihe starten         | —                                    |
| POST    | `/api/sequence/stop`      | Messreihe abbrechen       | —                                    |

### 7.6  Messreihen-Presets (SD-Karte)

| Methode | Endpunkt              | Beschreibung              | Body                                       |
| ------- | --------------------- | ------------------------- | ------------------------------------------ |
| GET     | `/api/preset-list`    | Alle Presets laden        | —                                          |
| POST    | `/api/preset-save`    | Preset speichern          | `{name, sequences: [{temp_c, speed_mm_s, duration_s}]}` |
| POST    | `/api/preset-del`     | Preset löschen            | `{name}`                                   |

Presets werden als `/presets.json` auf der SD-Karte gespeichert (max. 8 KB).
Die Datei-Operationen sind über `datalog_read_raw_file()` / `datalog_write_raw_file()`
SPI-Mutex-geschützt.

---

## 8  Software-Architektur

### 8.1  FreeRTOS-Tasks

| Task-Name          | Stack  | Priorität | Funktion                             |
| ------------------ | ------ | --------- | ------------------------------------ |
| `httpd`            | 8192 B | 3         | HTTP-Server (ESP-IDF intern)         |
| `ws_push_task`     | 4096 B | 3         | WebSocket-Daten alle 100 ms senden   |
| `sequencer_task`   | 4096 B | 4         | Messreihen-Sequencer                 |

Der Sequencer-Task hat Priorität 4 (gleich wie `motor_mgr_task`),
da er Bewegungen koordiniert und rechtzeitig auf Events reagieren muss.

### 8.2  Dateistruktur

```
components/webui/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── webui.h                // webui_init(), webui_stop()
└── src/
    ├── webui.c                 // HTTP-Server Setup, Route-Registrierung
    ├── api_hotend.c            // REST-Handler Hotend
    ├── api_loadcell.c          // REST-Handler Wägezelle
    ├── api_motor.c             // REST-Handler Motor
    ├── api_datalog.c           // REST-Handler Datenlogger
    ├── api_sequence.c          // REST-Handler Messreihen
    ├── ws_handler.c            // WebSocket Upgrade, Push-Task, Cmd-Parser
    ├── sequencer.c             // Messreihen-Sequencer Zustandsmaschine
    ├── sequencer.h
    └── static/                 // Frontend-Dateien (→ SPIFFS/LittleFS)
        ├── index.html
        ├── style.css
        ├── app.js              // Dashboard-Logik, Diagramm, WebSocket
        ├── settings.js         // Einstellungen-Seite
        └── uplot.min.js        // Chart-Library
```

### 8.3  Frontend-Auslieferung

Die Dateien in `static/` werden beim Build in eine SPIFFS- oder
LittleFS-Partition gepackt. Der HTTP-Server liefert sie als statische
Dateien aus, mit gzip-Kompression (`Content-Encoding: gzip`).

| Datei          | Unkomprimiert | Gzipped (ca.) |
| -------------- | ------------- | ------------- |
| index.html     | 8 KB          | 3 KB          |
| style.css      | 6 KB          | 2 KB          |
| app.js         | 15 KB         | 5 KB          |
| settings.js    | 8 KB          | 3 KB          |
| uplot.min.js   | 40 KB         | 15 KB         |
| **Gesamt**     | **77 KB**     | **≈ 28 KB**   |

---

## 9  Wi-Fi-Konfiguration

| Parameter        | Default               | Kconfig-Key             |
| ---------------- | --------------------- | ----------------------- |
| Modus            | AP (Access Point)     | `WEBUI_WIFI_MODE`       |
| SSID (AP)        | `ESP32-Steuerung`     | `WEBUI_AP_SSID`         |
| Passwort (AP)    | `12345678`            | `WEBUI_AP_PASSWORD`     |
| IP (AP)          | 192.168.4.1           | —                       |
| SSID (STA)       | —                     | `WEBUI_STA_SSID`        |
| Passwort (STA)   | —                     | `WEBUI_STA_PASSWORD`    |
| mDNS-Hostname    | `esp32`               | `WEBUI_MDNS_HOSTNAME`   |

Im AP-Modus ist die Steuerung unter `http://192.168.4.1` erreichbar.
Im STA-Modus (eigenes WLAN) unter `http://esp32.local` (via mDNS).

---

## 10  Prioritäten-Übersicht (Gesamtsystem, final)

| Priorität | Task / ISR            | Modul          | Bemerkung                     |
| --------- | --------------------- | -------------- | ----------------------------- |
| 7         | RMT ISR / Callbacks   | Motor          | Hardware-ISR                  |
| 6         | `load_cell_task`      | Wägezelle      | 80 Hz Abtastung               |
| 5         | `hotend_pid_task`     | Hotend-PID     | 4 Hz Regelung                 |
| 4         | `motor_mgr_task`      | Motor          | Bewegungskoordination         |
| 4         | `sequencer_task`      | **Web-UI**     | Messreihen-Steuerung          |
| 3         | `httpd`               | **Web-UI**     | HTTP-Server                   |
| 3         | `ws_push_task`        | **Web-UI**     | WebSocket 10 Hz Push          |
| 3         | `datalog_sample`      | Datenlogger    | 1–10 Hz Sampling              |
| 2         | `datalog_writer`      | Datenlogger    | SD-Karte schreiben            |

---

## 11  Abhängigkeiten

| Abhängigkeit           | Richtung    | Beschreibung                                       |
| ---------------------- | ----------- | -------------------------------------------------- |
| **Hotend-PID-Modul**   | ruft auf    | Temperatur lesen, Soll setzen, Fehler quittieren   |
| **Wägezellen-Modul**   | ruft auf    | Gewicht lesen, tarieren, kalibrieren               |
| **Motor-Modul**        | ruft auf    | Bewegen, stoppen, konfigurieren, E-Steps           |
| **Datenlogger-Modul**  | ruft auf    | Aufzeichnung starten/stoppen, Dateien listen/laden |
| **Wi-Fi / LwIP**       | nutzt       | Netzwerk-Stack                                     |
| **mDNS**               | nutzt       | Hostname-Auflösung                                 |
| **SNTP**               | nutzt       | Zeitsynchronisation für Zeitstempel                |
| **SPIFFS / LittleFS**  | nutzt       | Frontend-Dateien aus Flash-Partition               |
| **ESP Event Loop**     | lauscht     | Events von allen Modulen                            |

---

## 12  Offene Punkte / TODOs

- [ ] Abgleich mit bestehendem Code aus dem Marlin-ESP32-Projekt
- [ ] Frontend-Design: Farbschema, Schriftart, responsive Breakpoints festlegen
- [ ] Chart-Library endgültig wählen (µPlot vs. Chart.js vs. Plotly light)
- [ ] OTA-Update über Web-UI? (Firmware-Upload via Browser)
- [ ] Authentifizierung: Soll das Web-UI passwortgeschützt sein?
- [ ] WebSocket-Reconnect-Strategie und Fehlerhandling im Frontend
- [x] Sequencer: Messreihen als Preset speichern und laden (SD-Karte, `/presets.json`)
- [ ] Sequencer: Soll eine „Warm-up"-Phase vor der ersten Sequenz automatisch ablaufen?
- [ ] Responsive Design für Mobilgeräte (Tablet im Labor)?
- [ ] Lokalisierung: Nur Deutsch oder auch Englisch?
