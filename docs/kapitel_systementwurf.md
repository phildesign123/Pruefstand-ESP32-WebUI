# Systementwurf

Dieses Kapitel beschreibt den Entwurf der Firmware und des Web-Frontends für den Extruder-Prüfstand. Ausgehend von einer Gesamtübersicht des Systems werden zunächst die Hardware-Schnittstellen des Mikrocontrollers dargestellt, anschließend die modulare Software-Architektur erläutert und die Echtzeitfähigkeit durch die FreeRTOS-Task-Struktur begründet. Darauf folgen die Beschreibung der Datenflüsse zwischen Sensorik, Regelung, Aufzeichnung und Benutzeroberfläche, die eingesetzten Zustandsautomaten, das Kommunikationsprotokoll zwischen Mikrocontroller und Browser sowie das Sicherheitskonzept.


## Systemübersicht

Der Extruder-Prüfstand basiert auf einem ESP32-WROVER-Mikrocontroller mit zwei Prozessorkernen und integriertem WLAN. Das System erfasst drei physikalische Messgrößen -- Temperatur, Extrusionskraft und Motorgeschwindigkeit --, regelt die Hotend-Temperatur über einen PID-Regler, steuert einen Schrittmotor zur Filamentförderung und zeichnet sämtliche Messdaten auf einer SD-Karte auf. Die gesamte Bedienung und Visualisierung erfolgt über eine browserbasierte Benutzeroberfläche, die per WebSocket in Echtzeit mit dem Mikrocontroller kommuniziert.

Abbildung X zeigt die Gesamtarchitektur des Systems. Der Mikrocontroller bildet das Zentrum und verbindet die vier peripheren Hardware-Komponenten -- Temperatursensor (MAX31865), Schrittmotortreiber (TMC2208), Wägezellen-ADC (NAU7802) und SD-Karte -- über drei Bussysteme (SPI, I2C, UART) sowie diskrete GPIO-Signale. Die Firmware gliedert sich in fünf funktionale Module: Hotend, Motor, Wägezelle, Datenlogger und Web-UI. Darüber liegt ein Sequenzer-Modul, das automatisierte Messabläufe orchestriert, indem es die drei Sensorik-Module und den Datenlogger koordiniert.

<!-- Abbildung X: Systemarchitektur-Blockdiagramm -->


## Hardware-Schnittstellen

Die Anbindung der peripheren Komponenten an den ESP32 erfolgt über drei Bussysteme und fünf diskrete GPIO-Leitungen. Tabelle X gibt einen Überblick über die vollständige Pin-Belegung.

<!-- Tabelle X: GPIO-Belegung des ESP32 -->

### SPI-Bus

Der VSPI-Bus des ESP32 wird von zwei Teilnehmern gemeinsam genutzt: dem Temperatursensor MAX31865 und der SD-Karte. Die drei Datenleitungen CLK (GPIO 18), MOSI (GPIO 23) und MISO (GPIO 19) sind physisch geteilt; die Selektion erfolgt über separate Chip-Select-Leitungen (GPIO 5 für den MAX31865, GPIO 4 für die SD-Karte). Da beide Teilnehmer nicht gleichzeitig angesprochen werden dürfen, wird der Buszugriff über einen FreeRTOS-Mutex serialisiert. Diese Entscheidung, nur einen SPI-Bus zu verwenden, reduziert die Anzahl belegter GPIO-Pins, erfordert jedoch eine sorgfältige Zugriffskoordination in der Firmware, die in Abschnitt Task-Architektur beschrieben wird.

### I2C-Bus

Der I2C-Bus verbindet den ESP32 über GPIO 21 (SDA) und GPIO 22 (SCL) mit dem 24-Bit-Analog-Digital-Wandler NAU7802 der Wägezelle. Die Busfrequenz beträgt 100 kHz, um bei den im Prüfstand verwendeten Kabellängen eine zuverlässige Signalübertragung zu gewährleisten. Zusätzlich dient GPIO 34 als Data-Ready-Eingang (DRDY), über den der NAU7802 signalisiert, dass ein neuer Messwert bereitsteht. Zur Absicherung gegen Bus-Hänger, die bei I2C durch fehlende Acknowledge-Signale auftreten können, implementiert die Firmware eine automatische Bus-Recovery-Prozedur, die Taktimpulse auf der SCL-Leitung erzeugt, um einen blockierten Slave-Baustein zurückzusetzen.

### UART

Die Konfiguration des Schrittmotortreibers TMC2208 erfolgt über eine UART-Schnittstelle mit 115.200 Baud (GPIO 33 TX, GPIO 32 RX). Über dieses proprietäre Trinamic-Protokoll mit 8-Byte-Rahmen werden Betriebsparameter wie Motorstrom, Mikroschrittauflösung und der StealthChop-Modus zur Laufzeit gesetzt und gelesen. Da die UART-Kommunikation asynchron zur Motorsteuerung erfolgt, wird sie durch einen eigenen Mutex geschützt.

### PWM- und Diskretsignale

Das Heizelement wird über GPIO 25 mittels einer pulsbreitenmodulierten Ansteuerung (LEDC-Peripherie des ESP32) mit 8 Hz und 7 Bit Auflösung geregelt. Die niedrige Frequenz entspricht dem Soft-PWM-Ansatz, wie er auch in der 3D-Drucker-Firmware Marlin eingesetzt wird, und eignet sich für die träge thermische Last eines Hotends. Der Lüfter wird über GPIO 26 mit 25 kHz bei 8 Bit Auflösung angesteuert. Diese Frequenz liegt oberhalb des hörbaren Bereichs und vermeidet somit störende Geräusche während der Messung.

Die Schrittmotoransteuerung nutzt die RMT-Peripherie (Remote Control Transceiver) des ESP32 auf GPIO 27 zur hardwaregestützten Pulserzeugung. Im Gegensatz zu einer softwarebasierten Pulsgenerierung erzeugt das RMT-Modul die Schrittimpulse autonom mit einer Taktbasis von 1 MHz und entlastet damit die CPU vollständig. Richtung und Freigabe des Motors werden über die digitalen Ausgänge GPIO 14 (DIR) und GPIO 13 (EN, active-low) gesteuert.


## Software-Architektur

Die Firmware ist modular aufgebaut und folgt dem Prinzip der losen Kopplung: Jedes der fünf Hauptmodule kapselt seine Hardware-Abhängigkeiten und stellt nach außen eine definierte C-Schnittstelle bereit. Eine zentrale Header-Datei (`config.h`) fasst sämtliche Konfigurationsparameter -- Pin-Belegungen, PID-Koeffizienten, Sicherheitsgrenzwerte, Task-Prioritäten und Netzwerkeinstellungen -- zusammen und bildet damit den einzigen Konfigurationspunkt des Systems.

### Hotend-Modul

Das Hotend-Modul übernimmt die Temperaturregelung und gliedert sich in sechs Unterkomponenten. Die Sensor-Komponente implementiert einen nicht-blockierenden Zustandsautomaten für die SPI-Kommunikation mit dem MAX31865, der in Abschnitt Zustandsautomaten näher beschrieben wird. Die aus dem Rohwiderstandswert des PT100-Sensors berechnete Temperatur wird über die Callendar-Van-Dusen-Gleichung in Grad Celsius umgerechnet.

Der PID-Regler berechnet aus Soll- und Ist-Temperatur die Stellgröße für das Heizelement. Die Implementierung umfasst eine Anti-Windup-Begrenzung des Integralanteils, eine IIR-Tiefpassfilterung des Differentialanteils sowie eine temperaturabhängige Skalierung der Ki- und Kd-Koeffizienten entsprechend der PID-Zykluszeit von 164 ms. Zur experimentellen Bestimmung optimaler PID-Parameter steht ein Relay-Autotune-Verfahren nach der Ziegler-Nichols-Methode zur Verfügung, das über fünf Oszillationszyklen die kritische Verstärkung und Periodendauer ermittelt.

Die Heizer-Komponente setzt die berechnete Stellgröße in ein PWM-Signal um, während die Lüfter-Komponente den Kühlkörperlüfter temperaturabhängig regelt: Unterhalb von 50 °C bleibt der Lüfter aus, zwischen 50 °C und 80 °C wird die Drehzahl linear hochgefahren, und oberhalb von 80 °C läuft er mit voller Leistung. Die Sicherheitskomponente wird im Abschnitt Sicherheitskonzept behandelt.

### Motor-Modul

Das Motor-Modul steuert den Schrittmotor über eine zweistufige Abstraktion. Die untere Ebene bildet der RMT-Treiber, der Pulsfrequenzen bis 50 kHz erzeugen kann und die Schrittimpulse vollständig in Hardware generiert. Die obere Ebene bietet eine High-Level-API, die Bewegungsbefehle in Form von Geschwindigkeit (mm/s) und Dauer oder Strecke entgegennimmt. Die Umrechnung von Geschwindigkeit in Schrittfrequenz erfolgt über den konfigurierbaren E-Steps-Wert (Schritte pro Millimeter), der im Auslieferungszustand 93 Schritte/mm beträgt und durch eine Kalibrierungsprozedur angepasst werden kann.

Bewegungsbefehle werden über eine FreeRTOS-Queue an den Motor-Task übergeben und dort sequenziell abgearbeitet. Dies entkoppelt die Befehlsquelle (Web-UI oder Sequenzer) vom zeitkritischen Pulsgenerator und verhindert Race Conditions bei gleichzeitigen Zugriffen. Sämtliche Motorparameter -- E-Steps, Mikroschrittauflösung, Motorstrom, Haltestrom und StealthChop-Modus -- werden im Non-Volatile Storage (NVS) des ESP32 persistiert und stehen nach einem Neustart unverändert zur Verfügung.

### Wägezellen-Modul

Das Wägezellen-Modul liest den 24-Bit-ADC NAU7802 über den I2C-Bus aus und bereitet die Rohdaten durch eine zweistufige digitale Filterung auf. In der ersten Stufe eliminiert ein Medianfilter mit einer Fensterbreite von fünf Samples impulsartige Störungen, wie sie bei elektromagnetischen Einstreuungen auftreten können. Die zweite Stufe bildet einen gleitenden Mittelwertfilter über zehn Samples, der das verbleibende Rauschen glättet. Bei einer Abtastrate von 80 Hz ergibt sich eine Filtergruppe mit einer effektiven Latenz von etwa 125 ms, was für die Messung quasi-statischer Extrusionskräfte ausreichend ist.

Die Kalibrierung erfolgt in zwei Schritten: Zunächst wird durch eine Tara-Messung der Leerwert erfasst und als Offset gespeichert, anschließend wird mit einem bekannten Referenzgewicht der Kalibrierfaktor bestimmt. Beide Werte werden im NVS persistiert.

### Datenlogger-Modul

Der Datenlogger implementiert eine Dual-Task-Architektur, die den zeitkritischen Messvorgang von der langsameren SD-Kartenkommunikation entkoppelt. Der Sampler-Task läuft auf dem Echtzeit-Kern (Core 1) und sammelt mit 10 Hz die aktuellen Sensorwerte -- Temperatur, Gewichtskraft, Motorgeschwindigkeit und einen Zeitstempel -- in einer Datenstruktur, die er in eine FreeRTOS-Queue mit 200 Plätzen einreiht. Diese Queue bietet einen Puffer von 20 Sekunden, der ausreicht, um auch längere SD-Karten-Zugriffe zu überbrücken.

Der Writer-Task läuft auf Core 0 (dem Netzwerk-Kern) und entnimmt die Datensätze aus der Queue. Er formatiert sie als CSV-Zeilen und akkumuliert sie zunächst in einem 16 KB großen RAM-Puffer. Alle 100 Samples -- entsprechend einem Intervall von zehn Sekunden -- wird der Puffer in einem einzelnen Schreibvorgang auf die SD-Karte übertragen. Dieses Vorgehen minimiert die Anzahl der SPI-Bustransaktionen und vermeidet damit Konflikte mit dem gleichzeitig laufenden Temperatursensor. Nach jedem Schreibvorgang wird die Datei explizit geschlossen, um den SPI-Bus für den MAX31865 freizugeben. Bei Schreibfehlern unternimmt der Writer bis zu drei Neuversuche mit vollständigem Remount der SD-Karte, bevor er in den Fehlerzustand wechselt.

### Web-UI-Modul

Das Web-UI-Modul umfasst den WiFi-Verbindungsmanager, einen asynchronen HTTP-Server und einen WebSocket-Server. Es stellt die browserbasierte Benutzeroberfläche als Single-Page-Application über das LittleFS-Dateisystem bereit. Das Frontend besteht aus einer HTML-Datei, einer JavaScript-Datei für die Anwendungslogik und einem CSS-Stylesheet mit Dark- und Light-Theme.

Die Echtzeit-Kommunikation zwischen Mikrocontroller und Browser wird im Abschnitt Kommunikationsprotokoll beschrieben.

### Sequenzer-Modul

Der Sequenzer automatisiert mehrstufige Messabläufe. Er verwaltet eine Liste von bis zu 20 Sequenzschritten, die jeweils eine Zieltemperatur, eine Extrusionsgeschwindigkeit und eine Messdauer definieren. Zur Laufzeit durchläuft der Sequenzer diese Schritte nacheinander, indem er die Zieltemperatur setzt, auf Erreichen und Stabilisierung wartet, dann Motor und Datenaufzeichnung startet und nach Ablauf der definierten Dauer zum nächsten Schritt übergeht. Das zugehörige Zustandsdiagramm wird im Abschnitt Zustandsautomaten erläutert. Über die Web-Oberfläche können Sequenzen als Presets gespeichert und wiederverwendet werden.


## Task-Architektur

Die Firmware nutzt das Echtzeitbetriebssystem FreeRTOS, das im ESP-IDF-Framework enthalten ist, zur Verwaltung nebenläufiger Aufgaben. Die Aufteilung der Tasks auf die zwei Prozessorkerne des ESP32 folgt dem Prinzip der Trennung von Echtzeit- und Kommunikationsaufgaben.

### Core-Zuordnung

Core 1 ist den zeitkritischen Aufgaben vorbehalten: Temperaturregelung, Wägezellenmessung, Motorsteuerung, Messsequenzsteuerung, Daten-Sampling und WebSocket-Push. Core 0 übernimmt den WiFi-Stack sowie den Datenlogger-Writer, dessen SD-Kartenzugriffe mehrere hundert Millisekunden dauern können und daher nicht auf dem Echtzeit-Kern ausgeführt werden sollten.

### Task-Priorisierung

Tabelle X listet alle Tasks mit ihren Parametern auf. Die Prioritätsvergabe folgt der Kritikalität der jeweiligen Aufgabe.

<!-- Tabelle X: FreeRTOS-Tasks -->

| Task              | Core | Priorität | Stack    | Zykluszeit     | Aufgabe                              |
|-------------------|------|-----------|----------|----------------|--------------------------------------|
| Wägezelle         | 1    | 6         | 8.192 B  | 12,5 ms (80 Hz)| ADC-Abtastung und Filterung          |
| Hotend            | 1    | 5         | 4.096 B  | 164 ms (PID)   | Temperaturmessung und PID-Regelung   |
| Motor             | 1    | 4         | 4.096 B  | 10 ms (Queue)  | Befehlsverarbeitung, RMT-Steuerung   |
| Sequenzer         | 1    | 4         | 4.096 B  | 100 ms         | Messablauf-Zustandsautomat           |
| Datenlogger-Sampler | 1  | 3         | 2.048 B  | 100 ms (10 Hz) | Sensorwerte lesen und einreihen      |
| WebSocket-Push    | 1    | 3         | 4.096 B  | 100 ms (10 Hz) | JSON-Statusnachrichten an Clients    |
| Datenlogger-Writer | 0   | 2         | 4.096 B  | Ereignisgesteuert | Queue-Daten auf SD-Karte schreiben |
| WiFi-Event        | 0   | (System)  | (System) | Ereignisgesteuert | Verbindungsverwaltung, Reconnect   |

Die höchste Priorität (6) erhält der Wägezellen-Task, da der NAU7802-ADC bei 80 Samples pro Sekunde ein enges Zeitfenster für das Auslesen bietet und verpasste DRDY-Signale zu Datenverlust führen. Der Hotend-Task folgt mit Priorität 5, da die Temperaturregelung sicherheitskritisch ist. Motor- und Sequenzer-Task teilen sich Priorität 4, wobei Konflikte durch die Queue-basierte Entkopplung vermieden werden. Die niedrigste Priorität (2) hat der Datenlogger-Writer, dessen Latenz durch den RAM-Puffer unkritisch ist.

### Synchronisationsmechanismen

Der gleichzeitige Zugriff mehrerer Tasks auf gemeinsame Hardware-Ressourcen erfordert Synchronisationsprimitive. Tabelle X zeigt die eingesetzten Mutexe.

<!-- Tabelle X: Synchronisationsprimitive -->

| Mutex       | Geschützte Ressource              | Zugreifende Module       |
|-------------|-----------------------------------|--------------------------|
| SPI-Mutex   | SPI-Bus (MAX31865 und SD-Karte)   | Hotend, Datenlogger      |
| I2C-Mutex   | I2C-Bus (NAU7802)                 | Wägezelle, Web-UI        |
| UART-Mutex  | UART-Schnittstelle (TMC2208)      | Motor                    |
| Spinlock    | Volatile Temperatur-/Duty-Variablen | Hotend-Task (R/W)     |

Für den besonders häufig zugegriffenen Temperaturwert, der sowohl vom Hotend-Task geschrieben als auch vom WebSocket-Push-Task und dem Datenlogger-Sampler gelesen wird, kommt anstelle eines Mutex ein Spinlock (Critical Section) zum Einsatz. Dieser blockiert Interrupts für wenige Mikrosekunden und vermeidet den Overhead eines vollständigen Kontextwechsels.


## Datenflüsse

Die Daten im System durchlaufen drei Hauptpfade: den Echtzeit-Regelpfad von der Sensorik zur Aktorik, den Aufzeichnungspfad von der Sensorik zur SD-Karte und den Visualisierungspfad von der Sensorik zum Browser.

### Echtzeit-Regelpfad

Der Temperatursensor MAX31865 wird über den SPI-Bus im nicht-blockierenden Verfahren ausgelesen. Der resultierende Widerstandswert wird mittels der Callendar-Van-Dusen-Gleichung in eine Temperatur umgerechnet. Alle 164 ms berechnet der PID-Regler aus der Differenz zwischen Soll- und Ist-Temperatur eine Stellgröße, die als PWM-Tastverhältnis an das Heizelement ausgegeben wird. Parallel dazu regelt die Lüftersteuerung die Kühlung temperaturabhängig.

Die Wägezelle wird über den I2C-Bus mit 80 Hz abgetastet. Jeder Rohwert durchläuft den Median- und den Mittelwertfilter und wird anschließend mit dem gespeicherten Tara-Offset und Kalibrierfaktor in Gramm umgerechnet.

### Aufzeichnungspfad

Der Datenlogger-Sampler liest mit 10 Hz die aktuellen Werte aller drei Messgrößen über die jeweiligen Modulschnittstellen aus und fügt sie als Datensatz in die FreeRTOS-Queue ein. Jeder Datensatz enthält eine laufende Nummer, einen Zeitstempel in Millisekunden seit Systemstart, die Gewichtskraft in Newton, die Temperatur in Grad Celsius, die Motorgeschwindigkeit in Millimetern pro Sekunde, ein Fehler-Byte sowie die Schleifendauer in Mikrosekunden zur Überwachung der Echtzeiteinhaltung.

Der Writer-Task entnimmt die Datensätze, formatiert sie als kommaseparierte Zeilen und akkumuliert sie in einem RAM-Puffer. Alle zehn Sekunden werden die gesammelten Daten in einem Schreibvorgang auf die SD-Karte übertragen. Dieses Batching reduziert die SPI-Bus-Belegung und minimiert die Wechselwirkung mit der gleichzeitig laufenden Temperaturmessung.

### Visualisierungspfad

Der WebSocket-Push-Task sendet mit 10 Hz kompakte JSON-Nachrichten an alle verbundenen Browser-Clients. Jede Nachricht enthält den aktuellen Systemzustand -- Temperatur, Solltemperatur, Heizer-Tastverhältnis, Gewichtskraft, Motorgeschwindigkeit, Sequenzer-Status und Aufzeichnungszustand. Im Browser werden die empfangenen Werte sowohl numerisch im Dashboard als auch grafisch in einem Echtzeit-Diagramm dargestellt, das über HTML5-Canvas gerendert wird und ein konfigurierbares Zeitfenster von 30, 60 oder 300 Sekunden abdeckt.

### Steuerungspfad

Benutzereingaben gelangen vom Browser über WebSocket-Nachrichten im JSON-Format an den Mikrocontroller. Das Web-UI-Modul parst den Befehlstyp und leitet ihn an das zuständige Modul weiter -- etwa eine Solltemperaturänderung an das Hotend-Modul oder einen Bewegungsbefehl an das Motor-Modul. Dieser Pfad ist asynchron und hat keinen Einfluss auf die Echtzeitfähigkeit der Regelung.


## Zustandsautomaten

Mehrere Teilsysteme der Firmware sind als endliche Zustandsautomaten implementiert. Diese Entwurfsentscheidung ermöglicht eine nicht-blockierende Programmstruktur, bei der jeder Aufruf des Automaten nur wenige Mikrosekunden dauert und die CPU sofort für andere Tasks freigegeben wird.

### Temperatursensor

Die SPI-Kommunikation mit dem MAX31865 erfordert definierte Wartezeiten zwischen den einzelnen Schritten einer Messung. Anstatt diese Wartezeiten blockierend zu überbrücken, durchläuft der Sensor-Treiber einen Zustandsautomaten mit vier Zuständen:

1. **IDLE** -- Ruhezustand, keine Messung aktiv.
2. **SETUP_BIAS** -- Die Bias-Spannung des Messumformers wird eingeschaltet. Dieser Vorgang benötigt etwa 1 ms zur Stabilisierung.
3. **SETUP_1_SHOT** -- Eine Einzelmessung wird ausgelöst. Die Analog-Digital-Wandlung dauert circa 65 ms.
4. **READ_RTD** -- Das Ergebnisregister wird ausgelesen und der Widerstandswert über die Callendar-Van-Dusen-Gleichung in Grad Celsius umgerechnet. Anschließend kehrt der Automat in den Zustand IDLE zurück.

Durch diese Aufteilung kann der Hotend-Task in jedem Schleifendurchlauf (1 ms) den Automaten einmal aufrufen, ohne blockiert zu werden. Die effektive Messrate ergibt sich aus der Summe der Wartezeiten zu circa 66 ms pro Messung.

### Sequenzer

Der Sequenzer steuert den automatisierten Messablauf über fünf Zustände:

1. **IDLE** -- Keine Sequenz aktiv. Der Sequenzer wartet auf einen Startbefehl.
2. **HEATING** -- Die Zieltemperatur des aktuellen Schritts wurde gesetzt. Der Sequenzer wartet, bis die Ist-Temperatur innerhalb einer Toleranz von 2 °C um den Sollwert liegt und für mindestens 10 Sekunden stabil bleibt. Ein Timeout von 240 Sekunden begrenzt die maximale Aufheizzeit.
3. **RUNNING** -- Motor und Datenaufzeichnung wurden gestartet. Der Sequenzer überwacht die verbleibende Messdauer.
4. **NEXT** -- Die aktuelle Messung ist abgeschlossen. Existieren weitere Sequenzschritte, wechselt der Automat zurück in den Zustand HEATING mit den Parametern des nächsten Schritts. Andernfalls geht er in den Zustand DONE über.
5. **DONE** -- Alle Sequenzschritte sind abgeschlossen. Heizer und Motor werden abgeschaltet, die Datenaufzeichnung gestoppt und der Sequenzer kehrt in den Zustand IDLE zurück.

### Datenlogger

Der Datenlogger implementiert einen Zustandsautomaten mit vier Zuständen: IDLE, RECORDING, PAUSED und ERROR. Aus dem Zustand RECORDING kann über einen Pause-Befehl in PAUSED gewechselt werden, ohne die laufende Datei zu schließen. Der Zustand ERROR wird bei dreimaligem Fehlschlagen eines SD-Kartenzugriffs erreicht und erfordert einen manuellen Neustart der Aufzeichnung.


## Kommunikationsprotokoll

Die Kommunikation zwischen Mikrocontroller und Browser erfolgt über zwei Kanäle: eine persistente WebSocket-Verbindung für Echtzeitdaten und Steuerbefehle sowie eine REST-API für dateibasierte Operationen.

### WebSocket-Protokoll

Die WebSocket-Verbindung wird auf dem Pfad `/ws` etabliert und dient der bidirektionalen Kommunikation. In Richtung Server-zu-Client sendet der Mikrocontroller mit 10 Hz kompakte JSON-Objekte, die den vollständigen Systemzustand abbilden. Die Felder umfassen den Systemzeitstempel, die aktuelle und die Solltemperatur, das Heizer-Tastverhältnis, die gemessene Gewichtskraft, die aktuelle Motorgeschwindigkeit, den Motorstatus, den Index und Zustand der laufenden Sequenz, die verbleibende Messdauer sowie den Zustand des Datenloggers.

In Richtung Client-zu-Server sendet der Browser JSON-Objekte mit einem Befehlsfeld, das den Typ der gewünschten Aktion spezifiziert, und zusätzlichen Parametern je nach Befehl. Die unterstützten Befehle umfassen die Temperaturvorgabe, PID-Parametrierung, Autotune-Start, Lüftersteuerung, Motorbefehle (Bewegung und Stopp), Wägezellen-Operationen (Tara und Kalibrierung) sowie Sequenzer-Steuerung (Schritte hinzufügen, Sequenz starten und stoppen).

Die maximale Anzahl gleichzeitiger WebSocket-Clients ist auf vier begrenzt, und die Nachrichtenwarteschlange pro Client fasst acht Nachrichten. Kann ein Client die Nachrichten nicht schnell genug verarbeiten, werden ältere Nachrichten verworfen, um den Server nicht zu blockieren.

### REST-API

Für Operationen, die nicht dem Echtzeit-Paradigma folgen, stellt der HTTP-Server eine REST-API bereit. Diese umfasst Endpunkte zum Abrufen des vollständigen Systemstatus, zur Zeitsynchronisation mit dem Browser, zum Auslesen der TMC2208-Konfiguration, zur Verwaltung der auf der SD-Karte gespeicherten Messdatendateien (Auflisten, Herunterladen, Löschen) sowie zur Verwaltung von Sequenz-Presets (Speichern, Laden, Löschen).

Der Dateidownload erfolgt über Chunked Transfer Encoding mit einer Blockgröße von 4 KB, um den Arbeitsspeicher des Mikrocontrollers zu schonen. Während eines Downloads wird der SPI-Mutex für jeden einzelnen Block angefordert und anschließend wieder freigegeben, sodass die Temperaturmessung zwischen den Blöcken weiterhin stattfinden kann.

### WiFi-Verbindungsmanagement

Beim Systemstart versucht der ESP32 zunächst, sich als Station (STA) mit einem konfigurierten WLAN-Netzwerk zu verbinden. Gelingt die Verbindung innerhalb von zehn Sekunden, wird ein mDNS-Dienst unter dem Hostnamen `pruefstand.local` registriert, über den der Browser den Mikrocontroller ohne Kenntnis der IP-Adresse erreichen kann. Schlägt die Verbindung fehl, wechselt das System in den Access-Point-Modus und eröffnet ein eigenes WLAN-Netzwerk mit fester IP-Adresse (192.168.4.1). Dieser Fallback stellt sicher, dass der Prüfstand auch ohne vorhandene Netzwerkinfrastruktur bedient werden kann.


## Persistierung

Konfigurationsparameter, die über Neustarts hinweg erhalten bleiben müssen, werden im Non-Volatile Storage (NVS) des ESP32 gespeichert. Der NVS nutzt einen dedizierten Flash-Partition-Bereich und organisiert die Daten in Namensräumen.

Im Namensraum des Motor-Moduls werden der kalibrierte E-Steps-Wert, die Richtungsinversion, die Mikroschrittauflösung, der Lauf- und Haltestrom sowie der StealthChop-Status persistiert. Der Namensraum des Wägezellen-Moduls speichert den Tara-Offset und den Kalibrierfaktor. Alle Werte werden beim Systemstart geladen und bei Änderungen über die Web-Oberfläche oder die serielle Schnittstelle sofort zurückgeschrieben.


## Sicherheitskonzept

Da der Prüfstand mit einem elektrisch beheizten Hotend arbeitet, das Temperaturen bis 300 °C erreicht, ist ein mehrstufiges Sicherheitskonzept implementiert.

### Thermische Überwachung

Die Sicherheitskomponente des Hotend-Moduls wird bei jedem PID-Zyklus -- also alle 164 ms -- aufgerufen und prüft vier Fehlerbedingungen:

1. **Übertemperatur** -- Überschreitet die gemessene Temperatur den absoluten Grenzwert von 300 °C, wird das Heizelement sofort abgeschaltet. Dieser Schutz greift unabhängig vom Regelzustand.
2. **Sensorsprung** -- Ändert sich die Temperatur innerhalb eines Messzyklus um mehr als 50 °C, deutet dies auf einen defekten oder gelösten Sensor hin. Das Heizelement wird abgeschaltet.
3. **Thermal Runaway (Aufheizphase)** -- Nähert sich die Temperatur trotz voller Heizleistung über einen Zeitraum von 20 Sekunden nicht um mindestens 2 °C dem Sollwert an, liegt ein Defekt vor (beispielsweise ein unterbrochenes Heizelement oder ein schlecht montierter Sensor).
4. **Thermal Runaway (Haltephase)** -- Fällt die Temperatur nach Erreichen des Sollwerts um mehr als 15 °C unter den Zielwert und erholt sich nicht innerhalb von 30 Sekunden, wird ebenfalls abgeschaltet.

In allen vier Fällen setzt die Firmware ein Fault-Flag, das die Heizleistung dauerhaft auf null hält. Die Wiederaufnahme des Betriebs erfordert einen expliziten manuellen Reset über die Web-Oberfläche oder die serielle Schnittstelle. Diese bewusste Designentscheidung verhindert, dass ein fehlerhafter Zustand durch automatische Wiederholungsversuche verschleiert wird.

### Watchdog-Timer

Der Task-Watchdog-Timer des ESP32 ist mit einer Zeitspanne von 15 Sekunden konfiguriert. Meldet sich ein überwachter Task innerhalb dieser Zeitspanne nicht, wird ein automatischer Neustart des Mikrocontrollers ausgelöst. Die großzügige Zeitspanne berücksichtigt, dass WiFi-Verbindungsversuche und SD-Karten-Remounts mehrere Sekunden in Anspruch nehmen können.

### Motor-Sicherheit

Der Schrittmotor wird nach zwei Sekunden Inaktivität automatisch stromlos geschaltet, indem der Enable-Pin auf HIGH gesetzt wird. Dies verhindert eine unnötige thermische Belastung des Motortreibers und des Motors im Ruhezustand.


## Frontend-Architektur

Die Benutzeroberfläche ist als Single-Page-Application realisiert, deren statische Dateien im LittleFS-Dateisystem auf dem Flash-Speicher des ESP32 abgelegt sind. Das Frontend besteht aus drei Dateien: einem HTML-Dokument, das die Seitenstruktur und alle Ansichten (Dashboard, Einstellungen, WiFi-Status, Dateiverwaltung) definiert, einem JavaScript-Modul für die Anwendungslogik und einem CSS-Stylesheet.

Das JavaScript-Modul verwaltet die WebSocket-Verbindung mit automatischer Wiederverbindungslogik, aktualisiert bei jedem eingehenden Statuspaket die numerischen Anzeigeelemente im Dashboard und fügt neue Datenpunkte in einen zirkulären Puffer ein, der das Echtzeit-Diagramm speist. Das Diagramm wird über die Canvas-API gerendert und bietet wählbare Zeitfenster von 30, 60 und 300 Sekunden. Das Stylesheet unterstützt einen Dark- und einen Light-Modus, der sich nach der Systemeinstellung des Nutzers richtet, und implementiert ein responsives Grid-Layout auf Basis des Pico-CSS-Frameworks.


## Zusammenfassung

Der Systementwurf gliedert die Firmware in fünf lose gekoppelte Module, die über definierte Schnittstellen und FreeRTOS-Primitive zusammenarbeiten. Die Trennung in einen Echtzeit-Kern für Sensorik und Regelung und einen Netzwerk-Kern für WiFi und SD-Kartenzugriffe stellt sicher, dass die Temperaturregelung und Kraftmessung auch bei gleichzeitiger Netzwerkkommunikation und Datenaufzeichnung deterministisch ablaufen. Das mehrstufige Sicherheitskonzept schützt vor thermischen Gefahren, und die browserbasierte Bedienoberfläche ermöglicht eine flexible Steuerung und Visualisierung ohne zusätzliche Software auf dem Arbeitsplatzrechner.
