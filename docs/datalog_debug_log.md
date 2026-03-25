# Datenlogger — Entwicklungs- und Debug-Protokoll

## 1. CSV-Spalten neu definiert (2026-03-22)

**Ausgangslage:** CSV hatte Spalten `timestamp, time_ms, temperature_c, weight_g, speed_mm_s, motor_active`.

**Änderung:** Neue Spalten nach Prüfstand-Anforderungen:
```
sample_seq, millis_ms, gewicht, temperatur, motor_geschwindigkeit, error_flags, sd_write_us, loop_duration_us
```

**Dateien:** `src/datalog/datalog.cpp`, `test/datalog_test/mocks.cpp`

---

## 2. Timing-Messung gefixt — Zwei-Task → Single-Task (2026-03-22)

**Problem 1 — `sd_write_us` nicht pro Zeile:**
Wert blieb über 5 Zeilen identisch (z.B. fünfmal `23607`), weil der alte Writer-Task batch-weise schrieb und nur einmal pro Batch die Zeit maß.

**Problem 2 — `loop_duration_us` zu niedrig (Median 16 µs):**
Der alte Sampler-Task las nur gecachte Float-Werte aus dem RAM (keine echten I2C/SPI-Reads). Die echten Sensor-Reads liefen in separaten Tasks. Die gemessene "Loop-Dauer" war nur die Zeit fürs Lesen von 4 Variablen + Ring-Push.

**Lösung:** Zwei Tasks (Sampler + Writer) zu einem **Single-Task** zusammengeführt:
- Pro Sample: Sensoren lesen → CSV formatieren → direkt auf SD schreiben
- `sd_print_us` und `sd_flush_us` werden pro Zeile individuell gemessen
- `loop_duration_us` umfasst den gesamten Zyklus inkl. SPI-Mutex-Wait und SD-I/O
- Timing-Werte stammen vom vorherigen Durchlauf (erste Zeile = 0,0,0)
- Flush findet alle `DATALOG_FLUSH_INTERVAL_S` Samples statt

**Ergebnis:** `loop_duration_us` jetzt realistisch ~13–20 ms (vorher 8–16 µs).

**Dateien:** `src/datalog/datalog.cpp`

---

## 3. Abtastrate auf 10 Hz (100 ms) erhöht (2026-03-22)

**Anforderung:** Gewicht soll mit 10 Hz abgetastet werden.

**Problem:** `DATALOG_INTERVAL_MS` wurde in `config.h` auf 100 geändert, aber die WebUI (`data/app.js`) sendete hardcoded `interval_ms: 1000` per API-Call und überschrieb den Default.

**Lösung:**
- `data/app.js`: `interval_ms: 1000` → `interval_ms: 100`
- `datalog_start()`: Wenn `interval_ms == 0` → Default aus `DATALOG_INTERVAL_MS` verwenden
- Minimum von 100 ms auf 10 ms gesenkt (für zukünftige höhere Raten)
- Serial-Log hinzugefügt: `[DATALOG] Intervall: X ms`

**Hinweis:** Browser-Cache kann altes JS ausliefern → Hard-Refresh nötig (Ctrl+Shift+R).

**Dateien:** `src/config.h`, `src/datalog/datalog.cpp`, `data/app.js`

---

## 4. Phasen-Timing-Instrumentierung für Spike-Analyse (2026-03-22)

**Problem 1 — Regelmäßige ~60 ms Spikes (alle ~96 Samples / ~9.6 s):**
`loop_duration_us` springt von ~19 ms auf 60–94 ms. `sd_flush_us` und `sd_print_us` dabei normal → Spike kommt von woanders.

**Vermutung:** SPI-Mutex-Blockierung durch MAX31865 Temperaturmessung (~65 ms Konversionszeit bei 60 Hz Filter). WebUI steuert gleichzeitig Temperatur und Motor → zusätzliche SPI-Bus-Last.

**Problem 2 — Einmaliger Crash (Seq 373–374):**
Zwei Loops blockieren massiv (355 ms, 714 ms), ein Sample geht verloren. Mögliche Ursachen: I2C-Bus-Hang, SPI-Bus-Konflikt, TMC2208 UART-Timeout.

**Lösung:** Phasen-Timing als zusätzliche CSV-Spalten:
```
sample_seq, millis_ms, gewicht, temperatur, motor_geschwindigkeit, error_flags,
phase_weight_us, phase_temp_us, phase_motor_us, mutex_wait_us,
sd_print_us, sd_flush_us, loop_duration_us
```

**Neue Spalten:**
| Spalte | Misst |
|---|---|
| `phase_weight_us` | `load_cell_get_weight_g()` Dauer |
| `phase_temp_us` | `hotend_get_temperature()` Dauer |
| `phase_motor_us` | `motor_get_current_speed()` Dauer |
| `mutex_wait_us` | Wartezeit auf SPI-Mutex (Hauptverdächtiger) |

**Error-Flags Bitmask erweitert:**
| Bit | Bedeutung |
|---|---|
| 0 | Hotend-Fault |
| 1 | Sensor-Fault (MAX31865) |
| 2 | SD-Fehler |
| 3 | SPI-Mutex Wait > 1 ms |
| 4 | Loop > 100 ms (Deadline verpasst) |

**Dateien:** `src/datalog/datalog.cpp`

---

## 5. Timing-Analyse: sd_open_us + sd_close_us hinzugefügt (2026-03-22)

**Ergebnis der Phasen-Analyse:** `mutex_wait_us` war NICHT das Problem (immer 2–3 µs).
Stattdessen fehlte die Messung von `SD.open()` und `f.close()`.

**Messung mit sd_open_us + sd_close_us ergab:**
| Phase | Dauer | Anteil |
|---|---|---|
| `sd_close_us` | ~14.5 ms | 76% |
| `sd_open_us` | ~3.5 ms | 18% |
| Sensoren + Mutex + print | ~60 µs | <1% |

**Ursache:** `f.close()` ruft intern `flush()` auf → jedes Sample wurde geflusht,
unabhängig vom expliziten Flush-Intervall. Plus 3.5 ms Overhead für `SD.open()` pro Sample.

Bei den Crash-Samples (373 ms, 679 ms) war `sd_close_us` = 198 ms — SD-Karte brauchte
extrem lange für den internen Flush (vermutlich Wear-Leveling oder Block-Erase).

---

## 6. Fix: Datei offen halten statt open/close pro Sample (2026-03-22)

**Lösung:** Persistenter `File`-Handle (`s_log_file`):
- Öffnen bei `datalog_start()` (einmalig)
- Schließen bei Stop, Pause oder Datei-Rotation
- `f.print()` schreibt in den internen Puffer (~40 µs)
- `f.flush()` nur alle `DATALOG_FLUSH_INTERVAL_S` Samples (explizit kontrolliert)
- Kein `f.close()` pro Sample → **spart ~18 ms pro Sample** (open + close)

**Erwartete Loop-Dauer:** ~1 ms (ohne Flush) statt ~19 ms.

**CSV-Spalten vereinfacht (sd_open_us + sd_close_us entfernt, nicht mehr relevant):**
```
sample_seq, millis_ms, gewicht, temperatur, motor_geschwindigkeit, error_flags,
phase_weight_us, phase_temp_us, phase_motor_us, mutex_wait_us,
sd_print_us, sd_flush_us, loop_duration_us
```

**Dateien:** `src/datalog/datalog.cpp`

---

## 7. Ergebnis persistenter File-Handle (2026-03-22)

**Messung bestätigt:** Loop ohne Flush ~250–500 µs (statt ~19 ms). 60x Verbesserung.

**Verbleibende Auffälligkeiten:**
- `sd_print_us` ~10 ms Spikes alle ~10 Samples → SD-Karte interner Seitenwechsel
- `sd_flush_us` Ausreißer bis 76 ms → SD-Karte Block-Erase/Wear-Leveling

---

## 8. RAM-Puffer: SD-Zugriff komplett aus Sample-Schleife entfernt (2026-03-22)

**Problem:** Auch mit persistentem File-Handle gab es ~10 ms Spikes bei `f.print()`,
weil die SD-Karte intern Pages schreibt wenn ihr Puffer voll ist.

**Lösung:** 4 KB RAM-Puffer (`s_buf`). Samples werden per `snprintf` nur in den RAM geschrieben.
Erst alle `DATALOG_FLUSH_SAMPLES` (default: 10) Samples wird der gesamte Puffer
mit einem einzigen `file.write()` + `file.flush()` auf die SD geschrieben.

**Architektur:**
- 9 von 10 Samples: nur `snprintf` in RAM → **~50 µs** (kein SD-Zugriff, kein SPI-Mutex)
- 1 von 10 Samples: `write()` + `flush()` → ~15–25 ms (1x pro Sekunde bei 10 Hz)
- SPI-Mutex nur noch 1x/Sekunde belegt statt 10x/Sekunde

**CSV-Spalten vereinfacht (Debug-Phasen entfernt):**
```
sample_seq, millis_ms, gewicht, temperatur, motor_geschwindigkeit,
error_flags, sd_write_us, loop_duration_us
```

- `sd_write_us`: Gesamtdauer write+flush (nur in der Flush-Zeile >0, sonst 0)
- `loop_duration_us`: Gesamte Arbeitszeit pro Sample

**Config:** `DATALOG_BUFFER_SIZE = 4096`, `DATALOG_FLUSH_SAMPLES = 10`

**Dateien:** `src/datalog/datalog.cpp`, `src/config.h`

---

## 9. Ergebnis RAM-Puffer mit 10 Samples (2026-03-23)

**Messung bestätigt:**
- 9 von 10 Samples: ~200 µs (kein SD-Zugriff)
- Flush-Sample: ~14–27 ms (normal), Ausreißer 86–218 ms (SD Wear-Leveling)
- 10 Hz stabil, keine verlorenen Samples

**Verbleibendes Problem:** SD-Karte blockiert alle ~22 Sekunden für 87–219 ms
durch internes Wear-Leveling. Das reißt den 100 ms Takt bei der Flush-Zeile.

---

## 10. Puffer auf 50 Samples vergrößert (2026-03-23)

**Motivation:** Weniger SD-Zugriffe = weniger Chancen auf Wear-Leveling-Stall.
Statt 1x/Sekunde nur noch 1x/5 Sekunden flushen.

**Änderungen:**
- `DATALOG_BUFFER_SIZE`: 4096 → **8192 Bytes** (50 Zeilen × ~120 Bytes + Headroom)
- `DATALOG_FLUSH_SAMPLES`: 10 → **50** (alle 5 Sekunden bei 10 Hz)
- Safety-Flush eingebaut: wenn Puffer < 150 Bytes frei → sofort flushen
- Nachteil: bei Stromausfall gehen bis zu 5 Sekunden Daten verloren

**Erwartung:**
- 49 von 50 Samples: ~200 µs (kein SD-Zugriff, kein SPI-Mutex)
- 1 von 50 Samples: SD-Write des gesamten Blocks (~6 KB)
- SD-Zugriffe pro Sekunde: 10 → 1 → **0.2**
- Wear-Leveling-Stalls sollten seltener auftreten (5x weniger Flush-Aufrufe)

**Dateien:** `src/datalog/datalog.cpp`, `src/config.h`

---

## 11. SD-Warmup vor erster Messung (2026-03-23)

**Problem:** Erster SD-Write nach Start blockiert 286 ms (FAT-Allokation + Cluster-Reservierung).

**Lösung:** Nach Header-Write einen Dummy-Block (8 KB) schreiben + flushen,
dann `file.seek()` zurück zum Header-Ende. Erzwingt Cluster-Allokation vorab.

**Ergebnis:** Erster Write 286 ms → 123 ms. Besser, aber nicht eliminiert.

---

## 12. Asynchroner SD-Write: Zwei Tasks auf getrennten Cores (2026-03-23)

**Problem:** SD-Writes (15–120 ms) blockieren den Sampler-Task auf Core 1.
Bei Wear-Leveling-Stalls (bis 286 ms) geht der 10 Hz Takt verloren.

**Lösung:** Sampler und Writer in separate FreeRTOS-Tasks auf getrennten Cores:

| Task | Core | Priorität | Aufgabe |
|---|---|---|---|
| `sampler_task` | Core 1 (Realtime) | 3 (hoch) | Sensoren lesen → Queue |
| `writer_task` | Core 0 (WiFi) | 2 (niedrig) | Queue → RAM-Puffer → SD |

**Core-Aufteilung:** Sampler auf Core 1 (Realtime), Writer auf Core 0 (zusammen mit WiFi/WebSocket).
SD-Writes können Core 0 blockieren, aber der Sampler auf Core 1 wird dadurch NIE beeinflusst.
WiFi-Stack hat höhere Priorität als der Writer und wird ebenfalls nicht blockiert.

**Architektur:**
- FreeRTOS Queue (100 Einträge = 10 Sekunden Puffer bei 10 Hz)
- Sampler schreibt non-blocking in Queue (`xQueueSend` mit Timeout 0)
- Writer wartet auf Queue, sammelt 50 Zeilen in 8 KB RAM-Puffer, schreibt auf SD
- SD-Writes können beliebig lange dauern — Sampler wird trotzdem geweckt (höhere Prio)
- SPI-Mutex nur noch vom Writer-Task (SD) und den Sensor-Tasks (MAX31865) verwendet
- Sampler greift NICHT auf SPI zu (liest nur gecachte Werte)

**Erwartetes Ergebnis:**
- `loop_duration_us` im Sampler: konstant ~5–10 µs (nur Cache-Reads)
- 10 Hz Takt perfekt stabil, auch bei SD-Stalls
- SD-Writes absorbieren Wear-Leveling im Hintergrund

**Dateien:** `src/datalog/datalog.cpp`

---

## 13. SD-Write Retry ohne Puffer-Verlust (2026-03-25)

**Problem:** Der Writer-Task verlor gelegentlich einen kompletten 50-Zeilen-Puffer (5 Sekunden Daten)
wenn ein SD-Write fehlschlug (Token Error / CRC Error). In der CSV sichtbar als Lücke von 50 fehlenden
`sample_seq` (z.B. 249 → 300). Passierte ca. 1× pro 10–15 Minuten bei hoher Heizleistung.

**Analyse der CSV-Daten:**
- `sd_write_us` war durchgehend 0 — Feld wurde nicht befüllt
- `loop_duration_us` Spitzen bis 117 µs korrelierten mit 1 ms Timestamp-Jitter
- Samples 250–299 komplett fehlend → exakt ein 50er-Puffer verloren
- Timing sonst gut: >99.5% der Samples im 100 ms Raster

**Ursache:** Die alte Retry-Logik machte 3 blockierende Retries mit SD-Remount direkt hintereinander.
Dabei wurde `lines_in_buf` nach dem Retry-Block bedingungslos auf 0 zurückgesetzt (Zeile 235),
unabhängig vom Erfolg. Bei einem temporären SD-Fehler (1 fehlgeschlagener Write, nächster sofort OK)
ging der gesamte Puffer verloren.

**Lösung:** Nicht-blockierende Retry-Logik mit Puffer-Erhalt:

| Schritt | Verhalten |
|---|---|
| Write fehlgeschlagen | `buffer_needs_write = true`, Puffer BEHALTEN |
| Retry 1–2 | 100 ms warten, Datei neu öffnen, Write wiederholen |
| Retry 3 (letzter) | SD komplett remounten, dann Write |
| Alle 3 fehlgeschlagen | Puffer verwerfen, `sd_error_flag` setzen |
| Retry erfolgreich | 0 Datenverlust, normal weiter |

**Neue Error-Flag:**
| Bit | Wert | Bedeutung |
|---|---|---|
| 4 | `0x10` | SD-Puffer verloren (nach 3 gescheiterten Retries) |

**Änderungen:**
- `sd_error_flag` (global volatile): Kommunikation Writer → Sampler
- `sampler_task`: Prüft Flag, setzt Bit 4 in `error_flags` der nächsten CSV-Zeile
- `writer_task`: Komplett umgebaut mit `buffer_needs_write`/`write_retries` State-Machine
- `sd_write_us` wird jetzt tatsächlich per `micros()` gemessen (war vorher immer 0)
- Während Retries sammeln sich neue Samples in der Queue (100 Einträge = 10 s Platz)

**Ergebnis:** Bei einem einzelnen SD-Error gehen 0 Samples verloren statt 50.
Nur bei 3 aufeinanderfolgenden Fehlern wird der Puffer verworfen (mit Bit 4 in CSV markiert).

**Dateien:** `src/datalog/datalog.cpp`

---

## 14. Persistenter File-Handle + größerer Flush-Puffer (2026-03-25)

**Beobachtung nach Retry-Fix:** Trotz stabiler CSV ohne Datenlücken lagen einzelne `sd_write_us`
noch bei ~178 ms. Die Ursache war nicht der eigentliche Block-Write, sondern der Overhead aus
`SD.open(FILE_APPEND)` + `flush()` + `close()` bei jedem Flush.

**Ursache im Code:**
- Datei wurde für jeden Flush neu geöffnet
- `FILE_APPEND` muss FAT/Directory auflösen und ans Dateiende springen
- bei 100 ms Logger-Takt addiert sich das unnötig zu großen Write-Latenzen

**Lösung:**
- Datei bei `datalog_start()` einmalig öffnen
- Header schreiben, Handle offen lassen
- im Writer nur noch `write()` + `flush()`
- `close()` nur noch bei Stop, Datei-Rotation oder Fehler-/Retry-Pfad

**Zusätzliche Änderungen:**
- `DATALOG_FLUSH_SAMPLES`: 50 → **100**
- `DATALOG_BUFFER_SIZE`: 8192 → **16384**
- `DATALOG_QUEUE_LEN`: neu **200**
- SD-Init-Speed als Konstante `SD_SPI_MHZ = 4000000`

**Ergebnis in der Praxis:**
- keine `sample_seq`-Lücken mehr
- `sd_write_us` typischerweise ~18–24 ms
- Maximalwerte nur noch ~37–40 ms statt ~178 ms
- `error_flags` in sauberen Läufen durchgehend 0

**Dateien:** `src/config.h`, `src/datalog/datalog.cpp`

---

## 15. I2C-Diagnose korrigiert: nur echte Kommunikationsfehler zählen (2026-03-25)

**Problem:** Die erste Diagnose-Version interpretierte `!nau7802_is_ready()` als I2C-Fehler.
Das erzeugte massenhaft scheinbare "Fehlerbursts", obwohl der NAU7802 oft nur noch keinen neuen
Messwert fertig hatte (`CR=0` bei 80 SPS = normales Verhalten).

**Symptom im Serial-Log:**
- sehr viele Bursts mit genau 1 Fehlversuch
- Abstände von ~12–14 ms
- keine Recoveries, kein realer Bus-Hang

**Ursache:** `not ready` wurde mit "I2C-Kommunikation fehlgeschlagen" verwechselt.

**Lösung:**
- echte I2C-Fehler nur noch direkt an den `Wire`-Transfers erkennen
- in `nau7802.cpp` wird der letzte Kommunikationsstatus gespeichert
- `load_cell_task` zählt nur dann Bursts/Recoveries, wenn der Transfer selbst scheitert
- bei `not ready`, aber gültiger Kommunikation, nur kurzer Delay und kein Log-Spam
- zusätzlich `Wire.setTimeout(10)` im Setup, damit Bus-Hänger keine 1s-Blockade mehr erzeugen

**Ergebnis:**
- Serial-Log bleibt ruhig im Normalbetrieb
- echte I2C-Ausfälle sind weiterhin sichtbar
- Diagnose ist jetzt aussagekräftig statt irreführend

**Dateien:** `src/load_cell/nau7802.cpp`, `src/load_cell/nau7802.h`, `src/load_cell/load_cell.cpp`, `src/main.cpp`

---

## Offene Fragen

- [x] Spike-Ursache: `SD.open()`/`f.close()` → persistenter File-Handle
- [x] Verbleibende sd_print Spikes: SD-Seitenwechsel → RAM-Puffer eliminiert
- [x] Crash-Ursache (373/679 ms): SD-Karten-Latenz → entkoppelt durch Zwei-Task
- [x] Wear-Leveling-Stalls: Writer auf Core 0, Sampler auf Core 1 → physisch getrennt, null Blockierung
- [ ] 320 Hz Gewicht-Abtastung: Mit async Writer jetzt realistisch umsetzbar
