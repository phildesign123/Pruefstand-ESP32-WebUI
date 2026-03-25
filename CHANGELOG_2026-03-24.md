# Änderungen 24.03.2026

## SD-Karte & Datalog

### SD Auto-Mount
- **Problem:** SD-Karte musste manuell über "Mounten"-Button gemountet werden
- **Lösung:** Auto-Mount beim Laden der WebUI (Dashboard + Einstellungen). Mount-Buttons entfernt. SD wird synchron beim Boot gemountet statt im Hintergrund-Task (verhindert Race-Conditions)

### SD-Karte Doppel-Mount Schutz
- **Problem:** `exists(): File system is not mounted` Fehler durch doppeltes `SD.begin()`
- **Lösung:** Guard in `sd_mount()` verhindert mehrfaches Mounting

### SD SPI-Bus Stabilität
- **Problem:** CRC-Fehler und `Card Failed` bei gleichzeitigem Zugriff von SD-Karte und MAX31865 Temperatursensor
- **Lösung:** SPI-Geschwindigkeit von 4 MHz auf 1 MHz reduziert. Datei wird nach jedem Flush geschlossen (kein offener Transfer wenn Sensor den Bus übernimmt)

### SD-Karte Remount bei Fehlern
- **Problem:** Nach CRC-Fehlern (vermutlich durch Heizer-EMV) bleibt SD-Karte in Fehlerzustand
- **Lösung:** Bei Write-Fehlern bis zu 3 Retries mit komplettem SD-Remount (`SD.end()` + `SD.begin()`). Puffer bleibt erhalten wenn alle Versuche scheitern

### Leere CSV-Dateien
- **Problem:** Aufzeichnungsdateien wurden erstellt aber enthielten keine Messdaten
- **Lösung:** Datei wird nach Header-Schreiben in `datalog_start()` geschlossen. Writer-Task öffnet per `FILE_APPEND` (vorher: doppeltes File-Handle Konflikt)

### JSON-Buffer Überlauf bei Dateiliste
- **Problem:** `Bad control character in string literal in JSON at position 2048` bei vielen Dateien
- **Lösung:** Fester 2048-Byte-Buffer durch dynamischen `String` ersetzt

### Dateiname-Format
- **Problem:** Dateinamen waren `log_YYYYMMDD_HHMMSS.csv`
- **Lösung:** Neues Format: `messdaten_DD-MM-YYYY_HH-MM-SS_NNN.csv` mit persistentem Zähler (überlebt Neustarts)

### Messreihen-Preamble in CSV
- **Problem:** Keine Information über die Messreihen-Konfiguration in der CSV-Datei
- **Lösung:** Sequenz-Tabelle (#, Temp, Speed, Dauer) wird als Kommentarzeilen am Anfang der CSV geschrieben

### Dateiliste Sortierung
- **Problem:** Neueste Dateien standen ganz unten
- **Lösung:** Dateiliste wird reversed – neueste Dateien zuerst

## Sequencer / Messreihen

### Sequencer-Crash (FreeRTOS Task Return)
- **Problem:** `FreeRTOS Task "sequencer" should not return, Aborting now!` nach Stop oder Aufheiz-Timeout
- **Lösung:** `break` aus der `for(;;)`-Schleife durch `continue` mit Aufräum-Code ersetzt. Task kehrt nie mehr zurück

### Aufheizphase überspringen
- **Problem:** Bei gleicher Temperatur in aufeinanderfolgenden Messreihen wurde 10s auf Temperaturstabilität gewartet
- **Lösung:** Wenn aktuelle Temperatur bereits im Toleranzbereich liegt, wird Aufheizphase übersprungen

### Nahtloser Motor-Übergang
- **Problem:** 2 Sekunden Pause zwischen Messreihen beim Geschwindigkeitswechsel
- **Lösung:** Motor Idle-Timeout aus `do_move()` in `motor_task()` Queue-Timeout verlagert. Nächster Move-Befehl wird vorab in Queue geschoben wenn gleiche Temperatur

### Optionaler Dateiname für Messreihen
- **Problem:** Kein benutzerdefinierter Dateiname möglich
- **Lösung:** Eingabefeld im Messreihen-Fenster für optionalen Dateinamen. Wird an Sequencer und Datalog weitergereicht

### Messdaten-Nachlauf
- **Problem:** Aufzeichnung stoppte sofort nach letzter Messreihe
- **Lösung:** 2 Sekunden Nachlauf nach der letzten Sequenz

### Aufheiz-Timeout
- **Problem:** 120s Timeout war zu kurz für 200°C
- **Lösung:** Timeout auf 240 Sekunden (4 Minuten) erhöht

### Auto-Refresh Dateiliste
- **Problem:** Nach Ende der Messreihe musste man die Seite manuell refreshen um die neue Datei zu sehen
- **Lösung:** WebSocket-basierte Erkennung: wenn `seq_state` von aktiv auf `idle` wechselt, wird Dateiliste nach 3s automatisch aktualisiert

## Motor / RMT

### RMT 15-Bit Duration Overflow
- **Problem:** Bei niedrigen Geschwindigkeiten (< 31 Hz, z.B. 0.1 mm/s) wurde die Bewegung sofort beendet statt 300s zu laufen
- **Ursache:** RMT-Item Duration ist 15 Bit (max 32767 µs). Bei 9 Hz = 111111 µs pro Step → Overflow
- **Lösung:** Frequenzen < 31 Hz nutzen Einzelschritte mit `vTaskDelay` statt RMT-Blöcke

### Motorrichtung einstellbar
- **Problem:** Motorrichtung konnte nicht über WebUI geändert werden
- **Lösung:** Richtung (Normal/Invertiert) in TMC2208-Konfiguration einstellbar. Persistent in NVS gespeichert

### Mikroschritte und E-Steps Synchronisation
- **Problem:** Änderung der Mikroschritte veränderte die Motorgeschwindigkeit
- **Lösung:** E-Steps werden automatisch proportional angepasst (z.B. 16→32 Mikroschritte: E-Steps verdoppelt)

### TMC2208 Einstellungen persistent
- **Problem:** TMC2208-Konfiguration (Strom, Mikroschritte, StealthChop, Interpolation) ging bei Neustart verloren
- **Lösung:** Alle TMC-Einstellungen werden in NVS gespeichert und beim Boot wiederhergestellt

### TMC2208 Konfiguration auslesen
- **Problem:** Aktuelle TMC-Einstellungen wurden nicht im Einstellungen-Tab angezeigt
- **Lösung:** Neuer API-Endpunkt `/api/motor/config` liest alle Werte direkt vom TMC-Treiber. Felder werden beim Tab-Wechsel automatisch befüllt

## WebUI

### Newton statt Gramm
- **Problem:** Wägezelle zeigte Gramm an
- **Lösung:** Überall in Newton umgerechnet (g × 0.00981): Dashboard, Einstellungen, CSV-Header (`kraft_N`), Serial, Chart

### Chart-Skalen
- **Problem:** Feste Skalen passten nicht zu den Werten, horizontale Gridlinien nicht bündig mit Beschriftung
- **Lösung:** Dynamische Newton- und Speed-Skalen in ganzzahligen Schritten. Gemeinsame Gridlinien für beide rechte Achsen. Temperatur in 50°C-Schritten. Speed- und Newton-Skala nebeneinander statt übereinander

### Pulsierende LED-Status
- **Problem:** Recording-Status und aktive Messreihe waren nur Text/Symbole
- **Lösung:** Rot pulsierende LED für Recording-Status. Grün pulsierende LED für aktive Messreihe in der Dateiliste und Sequenz-Tabelle

### mDNS
- **Problem:** Prüfstand nur über IP-Adresse erreichbar
- **Lösung:** mDNS aktiviert: `http://pruefstand.local`

### Motor-Karte Layout
- **Problem:** Motor-Karte war zu hoch
- **Lösung:** STOP-Button und Geschwindigkeits-Eingabe oben rechts neben der Geschwindigkeitsanzeige

### E-Steps Kalibrierung Eingabefelder
- **Problem:** Eingabefelder zu klein
- **Lösung:** Referenzstrecke, Speed und Steps/mm Felder vergrößert

### Messreihen-Tabelle tfoot Alignment
- **Problem:** Eingabefelder im tfoot nicht bündig mit Datenspalten
- **Lösung:** Leere `<td>` für #-Spalte, `tfoot td { padding: 0 }` und `padding-left: 8px` für Inputs

### LittleFS Corruption
- **Problem:** `Corrupted dir pair` – WebUI nicht erreichbar
- **Lösung:** Flash komplett gelöscht (`esptool erase_flash`) und neu aufgespielt

---

## Offenes Hardware-Problem: SD-Karten CRC-Fehler

### Symptom
Wiederkehrende `sdCommand(): crc error` und `Card Failed` Meldungen während der Aufzeichnung, besonders wenn der Heizer aktiv ist (PWM auf GPIO 25, 8 Hz). Die Software fängt das ab (Remount + Retry), aber Daten können bei längeren Störphasen verloren gehen.

### Ursache
Elektromagnetische Störungen (EMV) vom Heizer-PWM auf dem SPI-Bus. Der Heizer schaltet mit 8 Hz harte Ein/Aus-Flanken über einen MOSFET. Die SPI-Leitungen (GPIO 18/19/23) liegen nahe genug um Störungen einzufangen.

### Pin-Belegung (relevant)
| Signal | GPIO | Funktion |
|--------|------|----------|
| SPI CLK | 18 | SD + MAX31865 |
| SPI MISO | 19 | SD + MAX31865 |
| SPI MOSI | 23 | SD + MAX31865 |
| SD CS | 4 | SD-Karte Chip-Select |
| MAX CS | 5 | Temperatursensor CS |
| Heizer | 25 | PWM 8 Hz → MOSFET |

### Hardware-Maßnahmen (nach Priorität)

**1. Kondensatoren auf SPI-Leitungen (wichtigste Maßnahme)**
- 100nF Keramik-Kondensator (C0G/NP0) zwischen folgenden Leitungen und GND:
  - GPIO 18 (CLK) → GND
  - GPIO 4 (SD_CS) → GND
- **NICHT auf GPIO 23 (MOSI)!** Kondensator verzerrt Signal zum MAX31865 → Temperatur falsch (241°C statt 24.1°C)
- **NICHT auf GPIO 19 (MISO)!** Kondensator verfälscht Temperatursensor-Daten (1.8°C Abweichung)
- So nah wie möglich am **SD-Karten-Modul** löten
- Filtert Störspitzen auf CLK und Chip-Select

**2. Heizer-Kabel physisch trennen**
- Heizer-Kabel (GPIO 25 → MOSFET → Heizelement) mindestens **3 cm Abstand** von SPI-Kabeln
- Heizer Hin- und Rückleitung **verdrillen** (reduziert abgestrahltes Magnetfeld)
- SPI-Kabel und Heizer-Kabel **nicht parallel** führen, idealerweise im 90°-Winkel kreuzen

**3. Pull-up Widerstände auf SPI**
- 10kΩ Pull-up auf SD_CS (GPIO 4) → 3.3V
- 10kΩ Pull-up auf MISO (GPIO 19) → 3.3V
- Verhindert undefinierte Pegel während Chip-Select-Wechsel zwischen SD und MAX31865

**4. Snubber am Heizer-MOSFET**
- RC-Snubber: **100Ω + 100nF in Serie**, parallel zum MOSFET (Drain → Source)
- Dämpft die steilen Schaltflanken des MOSFET und reduziert EMV erheblich
- Alternativ: Freilaufdiode (1N4007) parallel zur Last falls induktiv

**5. Stromversorgung SD-Karte stabilisieren**
- **100µF Elko + 100nF Kerko** direkt am SD-Karten-Modul (VCC → GND)
- SD-Karten ziehen beim Schreiben kurzzeitig bis zu 100mA → Spannungseinbrüche stören SPI
- Falls möglich: separate 3.3V-Versorgung für SD-Karte (z.B. eigener LDO)

**6. SPI-Kabel kürzen**
- Je kürzer die SPI-Verbindung zum SD-Modul, desto weniger Störungen
- Ideal: SD-Modul direkt am ESP32-Board, nicht über lange Kabel

### Software-Gegenmaßnahmen (bereits implementiert)
- SPI-Geschwindigkeit auf 400 kHz reduziert (ursprünglich 4 MHz, dann 1 MHz, jetzt 400 kHz)
- SD-Remount bei CRC-Fehlern (bis zu 3 Versuche mit `SD.end()` + `SD.begin()`)
- RAM-Puffer bewahrt Daten bei temporären SD-Ausfällen
- Aufzeichnung geht nicht in ERROR-State sondern versucht weiter

---

## Weitere Fixes (25.03.2026)

### Sequencer-Crash nach Stop/Timeout
- **Problem:** `FreeRTOS Task "sequencer" should not return, Aborting now!` – ESP32 crashte nach Aufheiz-Timeout oder manuellem Stop
- **Ursache:** `break` verließ die `for(;;)`-Schleife, Task-Funktion kehrte zurück (verboten bei FreeRTOS)
- **Lösung:** `break` durch `continue` mit Aufräum-Code (motor_stop, datalog_stop, hotend_set_target(0)) ersetzt

### I2C-Fehler Wägezelle (NAU7802)
- **Problem:** Massenhaft `i2cWriteReadNonStop returned Error -1` im Leerlauf
- **Ursache:** I2C-Kabel verlängert → 400 kHz zu schnell für die erhöhte Kabelkapazität
- **Lösung:**
  - I2C-Frequenz von 400 kHz auf 100 kHz reduziert
  - I2C-Bus-Recovery implementiert: nach ~1s ohne Antwort werden 16 Clock-Pulse generiert, Wire neu initialisiert, NAU7802 neu konfiguriert
- **Hardware-Empfehlung:** 4.7kΩ Pull-up Widerstände auf SDA (GPIO 21) und SCL (GPIO 22) nach 3.3V nachlöten

### TMC2208 Einstellungen persistent
- **Problem:** Motor-Konfiguration (Strom, Mikroschritte, StealthChop, Interpolation, Richtung) ging bei Neustart verloren
- **Lösung:** Alle Werte werden in NVS gespeichert und beim Boot wiederhergestellt (Standardwerte werden zuerst gesetzt, NVS überschreibt)

### Mikroschritte ändern E-Steps automatisch
- **Problem:** Änderung der Mikroschritte änderte die Motorgeschwindigkeit
- **Lösung:** E-Steps werden proportional angepasst (z.B. 16→32 Mikroschritte → E-Steps × 2)

### Motorrichtung einstellbar
- **Problem:** Motorrichtung konnte nicht über WebUI geändert werden
- **Lösung:** Neue Einstellung "Richtung: Normal/Invertiert" in TMC2208-Konfiguration, persistent in NVS

### TMC2208-Konfiguration live auslesen
- **Problem:** Einstellungen-Tab zeigte Standardwerte statt aktuelle TMC-Konfiguration
- **Lösung:** Neuer API-Endpunkt `/api/motor/config` liest Werte direkt vom Treiber-IC. Alle Felder werden beim Tab-Wechsel automatisch befüllt

### Aufheiz-Timeout erhöht
- **Problem:** 120s Timeout zu kurz für 200°C
- **Lösung:** Auf 240 Sekunden (4 Minuten) erhöht
